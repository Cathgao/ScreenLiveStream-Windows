#include "TcpStreamer.h"
#include "Logger.h"
#include <ws2tcpip.h>

TcpStreamer::TcpStreamer() {}

TcpStreamer::~TcpStreamer() {
    Stop();
}

bool TcpStreamer::Start(const std::string& targetIp, uint16_t targetPort) {
    Stop();

    m_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (m_sock == INVALID_SOCKET) {
        Logger::E("TcpStreamer", "Failed to create socket");
        return false;
    }

    BOOL noDelay = TRUE;
    setsockopt(m_sock, IPPROTO_TCP, TCP_NODELAY, (const char*)&noDelay, sizeof(noDelay));

    int sndBuf = 2 * 1024 * 1024;
    setsockopt(m_sock, SOL_SOCKET, SO_SNDBUF, (const char*)&sndBuf, sizeof(sndBuf));

    sockaddr_in serverAddr = {};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(targetPort);
    inet_pton(AF_INET, targetIp.c_str(), &serverAddr.sin_addr);

    Logger::I("TcpStreamer", "Connecting TCP stream to " + targetIp + ":" + std::to_string(targetPort) + "...");
    if (connect(m_sock, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        Logger::E("TcpStreamer", "Failed to connect to " + targetIp + ":" + std::to_string(targetPort));
        closesocket(m_sock);
        m_sock = INVALID_SOCKET;
        return false;
    }

    m_isConnected = true;
    m_videoSeq = 0;
    m_audioSeq = 0;
    Logger::I("TcpStreamer", "Connected successfully to " + targetIp + ":" + std::to_string(targetPort));
    return true;
}

void TcpStreamer::Stop() {
    if (m_isConnected.exchange(false)) {
        std::lock_guard<std::mutex> lock(m_sendMutex);
        if (m_sock != INVALID_SOCKET) {
            uint8_t stopHeader[20] = {};
            stopHeader[0] = 0x51; // 'Q'
            stopHeader[1] = 0x43; // 'C'
            stopHeader[2] = 0x01; // Version
            stopHeader[3] = 0x82; // Stream Stop
            send(m_sock, reinterpret_cast<const char*>(stopHeader), 20, 0);

            closesocket(m_sock);
            m_sock = INVALID_SOCKET;
        }
    }
}

void TcpStreamer::SendFrame(
    const uint8_t* data,
    size_t size,
    int64_t timestampMs,
    bool isKeyframe,
    bool isCodecConfig,
    bool isHevc,
    bool isAudio
) {
    if (!m_isConnected || m_sock == INVALID_SOCKET || !data || size == 0) return;

    std::lock_guard<std::mutex> lock(m_sendMutex);
    if (!m_isConnected || m_sock == INVALID_SOCKET) return;

    uint32_t currentSeq = isAudio ? (++m_audioSeq) : (++m_videoSeq);

    uint8_t flags = 0;
    if (isKeyframe) flags |= 0x01;
    if (isCodecConfig) flags |= 0x02;
    if (isHevc) flags |= 0x04;
    if (isAudio) flags |= 0x08;

    uint8_t header[20];
    header[0] = 0x51; // 'Q'
    header[1] = 0x43; // 'C'
    header[2] = 0x01; // Version
    header[3] = flags;

    header[4] = static_cast<uint8_t>((currentSeq >> 24) & 0xFF);
    header[5] = static_cast<uint8_t>((currentSeq >> 16) & 0xFF);
    header[6] = static_cast<uint8_t>((currentSeq >> 8) & 0xFF);
    header[7] = static_cast<uint8_t>(currentSeq & 0xFF);

    for (int i = 0; i < 8; ++i) {
        header[8 + i] = static_cast<uint8_t>((timestampMs >> (56 - i * 8)) & 0xFF);
    }

    uint32_t payloadLen = static_cast<uint32_t>(size);
    header[16] = static_cast<uint8_t>((payloadLen >> 24) & 0xFF);
    header[17] = static_cast<uint8_t>((payloadLen >> 16) & 0xFF);
    header[18] = static_cast<uint8_t>((payloadLen >> 8) & 0xFF);
    header[19] = static_cast<uint8_t>(payloadLen & 0xFF);

    // Use WSASend with scatter-gather buffers to transmit Header + Payload in a single kernel packet
    WSABUF wsaBufs[2];
    wsaBufs[0].buf = reinterpret_cast<char*>(header);
    wsaBufs[0].len = 20;
    wsaBufs[1].buf = const_cast<char*>(reinterpret_cast<const char*>(data));
    wsaBufs[1].len = static_cast<ULONG>(size);

    DWORD bytesSent = 0;
    DWORD totalExpected = 20 + static_cast<DWORD>(size);

    int ret = WSASend(m_sock, wsaBufs, 2, &bytesSent, 0, nullptr, nullptr);
    if (ret == 0 && bytesSent == totalExpected) {
        m_sentBytes.fetch_add(bytesSent);
        return;
    }

    if (ret == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK) {
        m_isConnected = false;
        return;
    }

    DWORD totalSent = bytesSent;
    m_sentBytes.fetch_add(bytesSent);

    // Fallback loop if partial socket transmission occurred
    while (totalSent < totalExpected && m_isConnected) {
        if (totalSent < 20) {
            int n = send(m_sock, reinterpret_cast<const char*>(header) + totalSent, 20 - totalSent, 0);
            if (n <= 0) {
                m_isConnected = false;
                return;
            }
            totalSent += n;
            m_sentBytes.fetch_add(n);
        } else {
            DWORD payloadOffset = totalSent - 20;
            int n = send(m_sock, reinterpret_cast<const char*>(data) + payloadOffset, static_cast<int>(size - payloadOffset), 0);
            if (n <= 0) {
                m_isConnected = false;
                return;
            }
            totalSent += n;
            m_sentBytes.fetch_add(n);
        }
    }
}
