#include "UdpStreamer.h"
#include "Logger.h"
#include <ws2tcpip.h>
#include <cstring>

static inline void WriteBigEndian16(uint8_t* dst, uint16_t val) {
    dst[0] = static_cast<uint8_t>((val >> 8) & 0xFF);
    dst[1] = static_cast<uint8_t>(val & 0xFF);
}

static inline void WriteBigEndian32(uint8_t* dst, uint32_t val) {
    dst[0] = static_cast<uint8_t>((val >> 24) & 0xFF);
    dst[1] = static_cast<uint8_t>((val >> 16) & 0xFF);
    dst[2] = static_cast<uint8_t>((val >> 8) & 0xFF);
    dst[3] = static_cast<uint8_t>(val & 0xFF);
}

static inline void WriteBigEndian64(uint8_t* dst, uint64_t val) {
    dst[0] = static_cast<uint8_t>((val >> 56) & 0xFF);
    dst[1] = static_cast<uint8_t>((val >> 48) & 0xFF);
    dst[2] = static_cast<uint8_t>((val >> 40) & 0xFF);
    dst[3] = static_cast<uint8_t>((val >> 32) & 0xFF);
    dst[4] = static_cast<uint8_t>((val >> 24) & 0xFF);
    dst[5] = static_cast<uint8_t>((val >> 16) & 0xFF);
    dst[6] = static_cast<uint8_t>((val >> 8) & 0xFF);
    dst[7] = static_cast<uint8_t>(val & 0xFF);
}

static inline void SpinPacingNanos(int64_t nanos) {
    auto target = std::chrono::high_resolution_clock::now() + std::chrono::nanoseconds(nanos);
    while (std::chrono::high_resolution_clock::now() < target) {
        YieldProcessor();
    }
}

UdpStreamer::UdpStreamer() {}

UdpStreamer::~UdpStreamer() {
    Stop();
}

bool UdpStreamer::Start(const std::string& targetIp, uint16_t targetPort) {
    Stop();

    m_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_sock == INVALID_SOCKET) {
        Logger::E("UdpStreamer", "Failed to create UDP socket");
        return false;
    }

    int sndBuf = 4 * 1024 * 1024; // 4MB send buffer
    setsockopt(m_sock, SOL_SOCKET, SO_SNDBUF, (const char*)&sndBuf, sizeof(sndBuf));

    int rcvBuf = 512 * 1024;
    setsockopt(m_sock, SOL_SOCKET, SO_RCVBUF, (const char*)&rcvBuf, sizeof(rcvBuf));

    DWORD timeout = 500;
    setsockopt(m_sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));

    m_targetAddr = {};
    m_targetAddr.sin_family = AF_INET;
    m_targetAddr.sin_port = htons(targetPort);
    inet_pton(AF_INET, targetIp.c_str(), &m_targetAddr.sin_addr);

    m_isRunning = true;
    m_videoSeq = 0;
    m_audioSeq = 0;
    m_pingSeq = 0;

    m_recvThread = std::thread(&UdpStreamer::RecvThreadProc, this);
    Logger::I("UdpStreamer", "Started UDP Streamer (UDPV FEC Enabled) to " + targetIp + ":" + std::to_string(targetPort));
    return true;
}

void UdpStreamer::Stop() {
    if (m_isRunning.exchange(false)) {
        {
            std::lock_guard<std::mutex> lock(m_sendMutex);
            if (m_sock != INVALID_SOCKET) {
                auto stopPacket = Protocol::BuildStreamStopPacket();
                sendto(m_sock, (const char*)stopPacket.data(), (int)stopPacket.size(), 0, (sockaddr*)&m_targetAddr, sizeof(m_targetAddr));
                closesocket(m_sock);
                m_sock = INVALID_SOCKET;
            }
        }
        if (m_recvThread.joinable()) {
            m_recvThread.join();
        }
    }
}

void UdpStreamer::SendFrame(
    const uint8_t* data,
    size_t size,
    int64_t timestampMs,
    bool isKeyframe,
    bool isCodecConfig,
    bool isHevc,
    bool isAudio
) {
    if (!m_isRunning || m_sock == INVALID_SOCKET || !data || size == 0) return;

    std::lock_guard<std::mutex> lock(m_sendMutex);
    if (!m_isRunning || m_sock == INVALID_SOCKET) return;

    uint32_t currentSeq = isAudio ? (++m_audioSeq) : (++m_videoSeq);

    if (isAudio) {
        uint8_t audioFlags = 0x10; // FLAG_AUDIO
        if (isCodecConfig) audioFlags |= 0x20; // FLAG_CODEC_CONFIG

        // Single packet for audio frame (stack buffer up to 1500 bytes, fallback vector if unusually large)
        if (28 + size <= 1500) {
            uint8_t packet[1500];
            packet[0] = 'U'; packet[1] = 'D'; packet[2] = 'P'; packet[3] = 'V';
            WriteBigEndian32(&packet[4], currentSeq);
            WriteBigEndian64(&packet[8], static_cast<uint64_t>(timestampMs));
            packet[16] = audioFlags;
            WriteBigEndian16(&packet[17], 0); // fragIndex = 0
            WriteBigEndian16(&packet[19], 1); // totalFragments = 1
            WriteBigEndian32(&packet[21], static_cast<uint32_t>(size));
            packet[25] = packet[26] = packet[27] = 0;
            std::memcpy(&packet[28], data, size);

            int sent = sendto(m_sock, reinterpret_cast<const char*>(packet), static_cast<int>(28 + size), 0, (sockaddr*)&m_targetAddr, sizeof(m_targetAddr));
            if (sent > 0) m_sentBytes.fetch_add(sent);
        } else {
            std::vector<uint8_t> packet(28 + size);
            packet[0] = 'U'; packet[1] = 'D'; packet[2] = 'P'; packet[3] = 'V';
            WriteBigEndian32(&packet[4], currentSeq);
            WriteBigEndian64(&packet[8], static_cast<uint64_t>(timestampMs));
            packet[16] = audioFlags;
            WriteBigEndian16(&packet[17], 0);
            WriteBigEndian16(&packet[19], 1);
            WriteBigEndian32(&packet[21], static_cast<uint32_t>(size));
            packet[25] = packet[26] = packet[27] = 0;
            std::memcpy(&packet[28], data, size);

            int sent = sendto(m_sock, reinterpret_cast<const char*>(packet.data()), static_cast<int>(packet.size()), 0, (sockaddr*)&m_targetAddr, sizeof(m_targetAddr));
            if (sent > 0) m_sentBytes.fetch_add(sent);
        }
        return;
    }

    // Video Frame: Fragment into 1300-byte chunks with Interleaved XOR FEC
    const size_t CHUNK_SIZE = 1300;
    uint16_t totalFragments = static_cast<uint16_t>((size + CHUNK_SIZE - 1) / CHUNK_SIZE);
    if (totalFragments == 0) totalFragments = 1;

    uint8_t flags = 0;
    if (isKeyframe) flags |= 0x01;
    if (isCodecConfig && !isKeyframe) flags |= 0x02;
    if (isHevc) flags |= 0x04;

    const int FEC_GROUP_SIZE = 8;
    const int MAX_FEC_GROUPS = 64;
    int numFecGroups = (totalFragments > 1) ? ((totalFragments + FEC_GROUP_SIZE - 1) / FEC_GROUP_SIZE) : 0;
    if (numFecGroups > MAX_FEC_GROUPS) numFecGroups = MAX_FEC_GROUPS;

    uint8_t fecBuffers[MAX_FEC_GROUPS][1500];
    size_t fecSizes[MAX_FEC_GROUPS] = {};
    bool fecInitialized[MAX_FEC_GROUPS] = {};

    static std::atomic<int> s_sentVideoFrames{ 0 };
    int sNum = ++s_sentVideoFrames;
    if (sNum == 1 || sNum % 180 == 0) {
        Logger::I("UdpStreamer", "Streaming video frame #" + std::to_string(sNum) + " (" + std::to_string(size) + " bytes, " + std::to_string(totalFragments) + " frags, " + std::to_string(numFecGroups) + " FEC groups, flags=0x" + std::to_string(flags) + ")");
    }

    uint8_t packet[1500];
    packet[0] = 'U'; packet[1] = 'D'; packet[2] = 'P'; packet[3] = 'V';
    packet[16] = flags;
    packet[25] = static_cast<uint8_t>(FEC_GROUP_SIZE);
    packet[26] = packet[27] = 0;

    bool isLargeFrame = totalFragments > 4;
    int64_t paceNanos = isLargeFrame ? 30000 : 0;

    size_t offset = 0;
    for (uint16_t i = 0; i < totalFragments; ++i) {
        size_t chunk = (size - offset > CHUNK_SIZE) ? CHUNK_SIZE : (size - offset);

        if (numFecGroups > 0) {
            int g = i % numFecGroups;
            if (!fecInitialized[g]) {
                std::memcpy(fecBuffers[g], data + offset, chunk);
                fecSizes[g] = chunk;
                fecInitialized[g] = true;
            } else {
                if (chunk > fecSizes[g]) {
                    std::memset(fecBuffers[g] + fecSizes[g], 0, chunk - fecSizes[g]);
                    fecSizes[g] = chunk;
                }
                size_t numLongs = chunk / sizeof(uint64_t);
                uint64_t* dst64 = reinterpret_cast<uint64_t*>(fecBuffers[g]);
                const uint64_t* src64 = reinterpret_cast<const uint64_t*>(data + offset);
                for (size_t k = 0; k < numLongs; ++k) {
                    dst64[k] ^= src64[k];
                }
                for (size_t k = numLongs * sizeof(uint64_t); k < chunk; ++k) {
                    fecBuffers[g][k] ^= data[offset + k];
                }
            }
        }

        WriteBigEndian32(&packet[4], currentSeq);
        WriteBigEndian64(&packet[8], static_cast<uint64_t>(timestampMs));
        WriteBigEndian16(&packet[17], i);
        WriteBigEndian16(&packet[19], totalFragments);
        WriteBigEndian32(&packet[21], static_cast<uint32_t>(size));
        std::memcpy(&packet[28], data + offset, chunk);

        int sent = sendto(m_sock, reinterpret_cast<const char*>(packet), static_cast<int>(28 + chunk), 0, (sockaddr*)&m_targetAddr, sizeof(m_targetAddr));
        if (sent > 0) m_sentBytes.fetch_add(sent);

        if (paceNanos > 0 && i < totalFragments - 1) {
            SpinPacingNanos(paceNanos);
        }

        offset += chunk;
    }

    // Send Interleaved FEC Parity Packets
    if (numFecGroups > 0) {
        for (int g = 0; g < numFecGroups; ++g) {
            if (!fecInitialized[g]) continue;
            uint8_t fecPkt[1500];
            fecPkt[0] = 'U'; fecPkt[1] = 'D'; fecPkt[2] = 'P'; fecPkt[3] = 'V';
            WriteBigEndian32(&fecPkt[4], currentSeq);
            WriteBigEndian64(&fecPkt[8], static_cast<uint64_t>(timestampMs));
            fecPkt[16] = flags | 0x08; // FLAG_FEC
            WriteBigEndian16(&fecPkt[17], static_cast<uint16_t>(g));
            WriteBigEndian16(&fecPkt[19], totalFragments);
            WriteBigEndian32(&fecPkt[21], static_cast<uint32_t>(size));
            fecPkt[25] = static_cast<uint8_t>(FEC_GROUP_SIZE);
            fecPkt[26] = fecPkt[27] = 0;
            std::memcpy(&fecPkt[28], fecBuffers[g], fecSizes[g]);

            int fecSent = sendto(m_sock, reinterpret_cast<const char*>(fecPkt), static_cast<int>(28 + fecSizes[g]), 0, (sockaddr*)&m_targetAddr, sizeof(m_targetAddr));
            if (fecSent > 0) m_sentBytes.fetch_add(fecSent);

            if (paceNanos > 0 && g < numFecGroups - 1) {
                SpinPacingNanos(paceNanos);
            }
        }
    }
}

void UdpStreamer::RecvThreadProc() {
    uint8_t buf[2048];
    auto lastPingTime = std::chrono::steady_clock::now();

    while (m_isRunning) {
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastPingTime).count() >= 500) {
            lastPingTime = now;
            int64_t nanos = Protocol::GetCurrentNanos();
            auto pingPkt = Protocol::BuildPingPacket(++m_pingSeq, nanos);
            std::lock_guard<std::mutex> lock(m_sendMutex);
            int sent = sendto(m_sock, (const char*)pingPkt.data(), (int)pingPkt.size(), 0, (sockaddr*)&m_targetAddr, sizeof(m_targetAddr));
            if (sent > 0) m_sentBytes.fetch_add(sent);
        }

        sockaddr_in fromAddr = {};
        int fromLen = sizeof(fromAddr);
        int bytes = recvfrom(m_sock, (char*)buf, sizeof(buf), 0, (sockaddr*)&fromAddr, &fromLen);

        if (bytes >= Protocol::HEADER_SIZE + 8) {
            auto parsed = Protocol::ParsePacket(buf, bytes);
            if (parsed && parsed->header.isPingReply()) {
                // Echo received
                int64_t nowNanos = Protocol::GetCurrentNanos();
                uint64_t originalNanos = 0;
                for (int i = 0; i < 8; ++i) {
                    originalNanos = (originalNanos << 8) | parsed->payload[i];
                }
                int rtt = static_cast<int>((nowNanos - static_cast<int64_t>(originalNanos)) / 1000000);
                if (rtt >= 0 && rtt < 10000) {
                    m_lastRttMs = rtt;

                    // Send stats beacon
                    auto statsPkt = Protocol::BuildPingStatsPacket(rtt, 0);
                    std::lock_guard<std::mutex> lock(m_sendMutex);
                    int sent = sendto(m_sock, (const char*)statsPkt.data(), (int)statsPkt.size(), 0, (sockaddr*)&m_targetAddr, sizeof(m_targetAddr));
                    if (sent > 0) m_sentBytes.fetch_add(sent);
                }
            }
        }
    }
}
