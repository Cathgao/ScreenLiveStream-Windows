#include "TcpReceiver.h"
#include "Logger.h"
#include <ws2tcpip.h>

TcpReceiver::TcpReceiver() {}

TcpReceiver::~TcpReceiver() {
    Stop();
}

bool TcpReceiver::Start(uint16_t listenPort) {
    Stop();

    m_listenPort = listenPort;
    m_isRunning = true;
    m_listenThread = std::thread(&TcpReceiver::ListenThreadProc, this);
    return true;
}

void TcpReceiver::Stop() {
    if (m_isRunning.exchange(false)) {
        if (m_clientSock != INVALID_SOCKET) {
            closesocket(m_clientSock);
            m_clientSock = INVALID_SOCKET;
        }
        if (m_listenSock != INVALID_SOCKET) {
            closesocket(m_listenSock);
            m_listenSock = INVALID_SOCKET;
        }
        if (m_listenThread.joinable()) {
            m_listenThread.join();
        }
    }
}

void TcpReceiver::ListenThreadProc() {
    m_listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (m_listenSock == INVALID_SOCKET) return;

    BOOL reuse = TRUE;
    setsockopt(m_listenSock, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));

    sockaddr_in bindAddr = {};
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_port = htons(m_listenPort);
    bindAddr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(m_listenSock, (sockaddr*)&bindAddr, sizeof(bindAddr)) == SOCKET_ERROR) {
        Logger::E("TcpReceiver", "Failed to bind TCP port " + std::to_string(m_listenPort));
        closesocket(m_listenSock);
        m_listenSock = INVALID_SOCKET;
        return;
    }

    if (listen(m_listenSock, 1) == SOCKET_ERROR) {
        Logger::E("TcpReceiver", "Listen failed on port " + std::to_string(m_listenPort));
        closesocket(m_listenSock);
        m_listenSock = INVALID_SOCKET;
        return;
    }

    Logger::I("TcpReceiver", "Listening for incoming TCP stream on port " + std::to_string(m_listenPort) + "...");

    while (m_isRunning) {
        sockaddr_in clientAddr = {};
        int clientLen = sizeof(clientAddr);
        m_clientSock = accept(m_listenSock, (sockaddr*)&clientAddr, &clientLen);

        if (m_clientSock == INVALID_SOCKET) {
            break;
        }

        BOOL noDelay = TRUE;
        setsockopt(m_clientSock, IPPROTO_TCP, TCP_NODELAY, (const char*)&noDelay, sizeof(noDelay));

        int rcvBuf = 2 * 1024 * 1024;
        setsockopt(m_clientSock, SOL_SOCKET, SO_RCVBUF, (const char*)&rcvBuf, sizeof(rcvBuf));

        char ipStr[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &clientAddr.sin_addr, ipStr, sizeof(ipStr));
        Logger::I("TcpReceiver", "Accepted TCP connection from " + std::string(ipStr));

        HandleClient(m_clientSock);

        closesocket(m_clientSock);
        m_clientSock = INVALID_SOCKET;
    }
}

static bool RecvExact(SOCKET s, uint8_t* buf, size_t len, const std::atomic<bool>& isRunning) {
    size_t total = 0;
    while (total < len && isRunning) {
        int n = recv(s, reinterpret_cast<char*>(buf) + total, static_cast<int>(len - total), 0);
        if (n <= 0) return false;
        total += n;
    }
    return total == len;
}

void TcpReceiver::HandleClient(SOCKET clientSock) {
    uint8_t headerBuf[20];
    std::vector<uint8_t> payloadBuf;

    while (m_isRunning) {
        if (!RecvExact(clientSock, headerBuf, 20, m_isRunning)) {
            break;
        }
        m_receivedBytes.fetch_add(20);

        if (headerBuf[0] != 0x51 || headerBuf[1] != 0x43) { // 'Q', 'C'
            Logger::W("TcpReceiver", "Invalid TCP packet magic, dropping connection.");
            break;
        }

        uint8_t flags = headerBuf[3];
        bool isKeyframe = (flags & 0x01) != 0;
        bool isCodecConfig = (flags & 0x02) != 0;
        bool isHevc = (flags & 0x04) != 0;
        bool isAudio = (flags & 0x08) != 0;
        bool isPingStats = (flags & 0x40) != 0;
        bool isStreamStop = (flags == 0x82);

        if (isStreamStop) {
            Logger::I("TcpReceiver", "Received Stream Stop packet via TCP.");
            break;
        }

        uint32_t seq = (headerBuf[4] << 24) | (headerBuf[5] << 16) | (headerBuf[6] << 8) | headerBuf[7];
        (void)seq;

        int64_t timestampMs = 0;
        for (int i = 0; i < 8; ++i) {
            timestampMs = (timestampMs << 8) | headerBuf[8 + i];
        }

        uint32_t payloadSize = (headerBuf[16] << 24) | (headerBuf[17] << 16) | (headerBuf[18] << 8) | headerBuf[19];

        if (payloadSize > 32 * 1024 * 1024) { // Protection against malformed size
            Logger::E("TcpReceiver", "Payload size too large: " + std::to_string(payloadSize));
            break;
        }

        if (isPingStats) {
            uint8_t statsBuf[8];
            if (RecvExact(clientSock, statsBuf, 8, m_isRunning)) {
                m_receivedBytes.fetch_add(8);
                // RTT and loss
            }
            continue;
        }

        if (payloadSize > 0) {
            payloadBuf.resize(payloadSize);
            if (!RecvExact(clientSock, payloadBuf.data(), payloadSize, m_isRunning)) {
                break;
            }
            m_receivedBytes.fetch_add(payloadSize);
        }

        if (isAudio) {
            if (m_audioCallback && payloadSize > 0) {
                m_audioCallback(payloadBuf.data(), payloadSize, timestampMs);
            }
        } else {
            if (m_videoCallback && payloadSize > 0) {
                m_videoCallback(payloadBuf.data(), payloadSize, timestampMs, isKeyframe, isCodecConfig, isHevc);
            }
        }
    }
}
