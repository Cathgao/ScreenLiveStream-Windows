#include "UdpReceiver.h"
#include "Logger.h"
#include <ws2tcpip.h>
#include <algorithm>

UdpReceiver::UdpReceiver() {}

UdpReceiver::~UdpReceiver() {
    Stop();
}

bool UdpReceiver::Start(uint16_t listenPort) {
    Stop();

    m_listenPort = listenPort;
    m_isRunning = true;
    m_lastAssembledSeq = -1;
    m_udpvBuffers.clear();
    m_recvThread = std::thread(&UdpReceiver::RecvThreadProc, this);
    return true;
}

void UdpReceiver::Stop() {
    if (m_isRunning.exchange(false)) {
        if (m_sock != INVALID_SOCKET) {
            closesocket(m_sock);
            m_sock = INVALID_SOCKET;
        }
        if (m_recvThread.joinable()) {
            m_recvThread.join();
        }
    }
}

void UdpReceiver::RecvThreadProc() {
    m_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_sock == INVALID_SOCKET) return;

    BOOL reuse = TRUE;
    setsockopt(m_sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));

    int rcvBuf = 8 * 1024 * 1024; // 8MB buffer for UDP packets
    setsockopt(m_sock, SOL_SOCKET, SO_RCVBUF, (const char*)&rcvBuf, sizeof(rcvBuf));

    sockaddr_in bindAddr = {};
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_port = htons(m_listenPort);
    bindAddr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(m_sock, (sockaddr*)&bindAddr, sizeof(bindAddr)) == SOCKET_ERROR) {
        Logger::E("UdpReceiver", "Failed to bind UDP port " + std::to_string(m_listenPort));
        closesocket(m_sock);
        m_sock = INVALID_SOCKET;
        return;
    }

    Logger::I("UdpReceiver", "Listening on UDP port " + std::to_string(m_listenPort) + " (FEC Enabled)...");

    uint8_t buffer[4096];

    while (m_isRunning) {
        sockaddr_in fromAddr = {};
        int fromLen = sizeof(fromAddr);
        int bytes = recvfrom(m_sock, (char*)buffer, sizeof(buffer), 0, (sockaddr*)&fromAddr, &fromLen);

        if (bytes <= 0) break;

        m_receivedBytes.fetch_add(bytes);

        // 1. Check for STREAM_STOP packet
        if (bytes >= 22 && buffer[0] == 'Q' && buffer[1] == 'C') {
            if (Protocol::IsStreamStopPacket(buffer, bytes)) {
                Logger::I("UdpReceiver", "Received Stream Stop packet.");
                m_lastAssembledSeq = -1;
                m_udpvBuffers.clear();
                continue;
            }

            auto parsedOpt = Protocol::ParsePacket(buffer, bytes);
            if (parsedOpt) {
                const auto& pkt = *parsedOpt;
                if (pkt.header.isPing()) {
                    if (pkt.payloadSize >= 8) {
                        uint64_t nanos = 0;
                        for (int i = 0; i < 8; ++i) {
                            nanos = (nanos << 8) | pkt.payload[i];
                        }
                        auto reply = Protocol::BuildPingReplyPacket(pkt.header.frameSeq, static_cast<int64_t>(nanos));
                        sendto(m_sock, (const char*)reply.data(), (int)reply.size(), 0, (sockaddr*)&fromAddr, sizeof(fromAddr));
                    }
                    continue;
                }
                if (pkt.header.isPingStats()) {
                    if (pkt.payloadSize >= 8) {
                        int32_t rtt = (pkt.payload[0] << 24) | (pkt.payload[1] << 16) | (pkt.payload[2] << 8) | pkt.payload[3];
                        int32_t loss = (pkt.payload[4] << 24) | (pkt.payload[5] << 16) | (pkt.payload[6] << 8) | pkt.payload[7];
                        if (m_statsCallback) {
                            m_statsCallback(rtt, loss);
                        }
                    }
                    continue;
                }
            }
        }

        // 2. Check for UDPV 28-byte FEC stream packet
        if (bytes >= 28) {
            uint32_t magic = (static_cast<uint32_t>(buffer[0]) << 24) |
                             (static_cast<uint32_t>(buffer[1]) << 16) |
                             (static_cast<uint32_t>(buffer[2]) << 8)  |
                              static_cast<uint32_t>(buffer[3]);

            if (magic == 0x55445056) { // "UDPV"
                ProcessUdpvPacket(buffer, bytes, fromAddr);
                continue;
            }
        }
    }

    closesocket(m_sock);
    m_sock = INVALID_SOCKET;
}

void UdpReceiver::ProcessUdpvPacket(const uint8_t* buffer, int length, const sockaddr_in& /*senderAddr*/) {
    int32_t seq = (static_cast<int32_t>(buffer[4]) << 24) |
                  (static_cast<int32_t>(buffer[5]) << 16) |
                  (static_cast<int32_t>(buffer[6]) << 8)  |
                   static_cast<int32_t>(buffer[7]);

    int64_t ts = (static_cast<int64_t>(buffer[8])  << 56) |
                 (static_cast<int64_t>(buffer[9])  << 48) |
                 (static_cast<int64_t>(buffer[10]) << 40) |
                 (static_cast<int64_t>(buffer[11]) << 32) |
                 (static_cast<int64_t>(buffer[12]) << 24) |
                 (static_cast<int64_t>(buffer[13]) << 16) |
                 (static_cast<int64_t>(buffer[14]) << 8)  |
                  static_cast<int64_t>(buffer[15]);

    uint8_t flags = buffer[16];
    uint16_t fragIndex = (static_cast<uint16_t>(buffer[17]) << 8) | static_cast<uint16_t>(buffer[18]);
    uint16_t totalFragments = (static_cast<uint16_t>(buffer[19]) << 8) | static_cast<uint16_t>(buffer[20]);
    uint32_t frameSize = (static_cast<uint32_t>(buffer[21]) << 24) |
                         (static_cast<uint32_t>(buffer[22]) << 16) |
                         (static_cast<uint32_t>(buffer[23]) << 8)  |
                          static_cast<uint32_t>(buffer[24]);

    bool isKeyframe = (flags & 1) != 0;
    bool isFec = (flags & 8) != 0;
    bool isAudio = (flags & 16) != 0;
    bool isBeacon = (flags & 64) != 0;

    if (isBeacon) {
        if (length >= 28 + 8) {
            int32_t rtt = (buffer[28] << 24) | (buffer[29] << 16) | (buffer[30] << 8) | buffer[31];
            int32_t lossBp = (buffer[32] << 24) | (buffer[33] << 16) | (buffer[34] << 8) | buffer[35];
            if (m_statsCallback) {
                m_statsCallback(rtt, lossBp);
            }
        }
        return;
    }

    if (isAudio) {
        int payloadSize = length - 28;
        if (payloadSize > 0 && m_audioCallback) {
            m_audioCallback(buffer + 28, payloadSize, ts);
        }
        return;
    }

    if (totalFragments == 0 || totalFragments > 2000) return;
    if (fragIndex >= totalFragments) return;
    if (frameSize == 0 || frameSize > 16 * 1024 * 1024) return;

    if (seq <= m_lastAssembledSeq) {
        bool isRestart = isKeyframe || (m_lastAssembledSeq - seq > 50);
        if (isRestart) {
            m_lastAssembledSeq = -1;
            m_udpvBuffers.clear();
        } else {
            return;
        }
    }

    auto it = m_udpvBuffers.find(seq);
    if (it == m_udpvBuffers.end()) {
        UdpvFrameBuffer fb;
        fb.seq = seq;
        fb.totalFragments = totalFragments;
        fb.frameSize = frameSize;
        fb.timestampMs = ts;
        fb.flags = flags;
        fb.frameBytes.resize(frameSize);
        fb.receivedFragments.resize(totalFragments, false);
        fb.receivedDataCount = 0;
        fb.lastUpdate = std::chrono::steady_clock::now();
        fb.isCompleted = false;

        m_udpvBuffers[seq] = std::move(fb);
        CleanOldUdpvFrames(seq);
        it = m_udpvBuffers.find(seq);
    }

    auto& fb = it->second;
    fb.lastUpdate = std::chrono::steady_clock::now();

    if (isFec) {
        int groupId = fragIndex;
        if (fb.fecPackets.find(groupId) == fb.fecPackets.end()) {
            size_t fecLen = length - 28;
            fb.fecPackets[groupId] = std::vector<uint8_t>(buffer + 28, buffer + 28 + fecLen);
        }
    } else {
        if (!fb.receivedFragments[fragIndex]) {
            size_t fragOffset = fragIndex * 1300;
            size_t fragLength = length - 28;
            if (fragOffset + fragLength <= fb.frameBytes.size()) {
                std::memcpy(fb.frameBytes.data() + fragOffset, buffer + 28, fragLength);
                fb.receivedFragments[fragIndex] = true;
                fb.receivedDataCount++;
            }
        }
    }

    CheckAndAssembleUdpv(fb);
}

void UdpReceiver::CheckAndAssembleUdpv(UdpvFrameBuffer& fb) {
    if (fb.isCompleted) return;

    // XOR FEC Recovery
    const int FEC_GROUP_SIZE = 12;
    if (fb.receivedDataCount < fb.totalFragments && !fb.fecPackets.empty()) {
        for (const auto& [groupId, fecPayload] : fb.fecPackets) {
            int startIdx = groupId * FEC_GROUP_SIZE;
            int endIdx = (std::min)(startIdx + FEC_GROUP_SIZE, static_cast<int>(fb.totalFragments));

            int missingCount = 0;
            int missingIdx = -1;
            for (int i = startIdx; i < endIdx; ++i) {
                if (!fb.receivedFragments[i]) {
                    missingCount++;
                    missingIdx = i;
                }
            }

            if (missingCount == 1) {
                std::vector<uint8_t> recovered(fecPayload);
                for (int i = startIdx; i < endIdx; ++i) {
                    if (i != missingIdx && fb.receivedFragments[i]) {
                        size_t fragOffset = i * 1300;
                        size_t fragLen = fb.GetExpectedFragLength(i);
                        for (size_t k = 0; k < fragLen && k < recovered.size(); ++k) {
                            recovered[k] ^= fb.frameBytes[fragOffset + k];
                        }
                    }
                }

                size_t expectedLen = fb.GetExpectedFragLength(missingIdx);
                size_t copyLen = (std::min)(expectedLen, recovered.size());
                size_t missingOffset = missingIdx * 1300;
                if (missingOffset + copyLen <= fb.frameBytes.size()) {
                    std::memcpy(fb.frameBytes.data() + missingOffset, recovered.data(), copyLen);
                    fb.receivedFragments[missingIdx] = true;
                    fb.receivedDataCount++;
                }
            }
        }
    }

    if (fb.receivedDataCount == fb.totalFragments) {
        fb.isCompleted = true;
        bool isKeyframe = (fb.flags & 1) != 0;
        bool isCodecConfig = (fb.flags & 2) != 0;
        bool isHevc = (fb.flags & 4) != 0;

        if (fb.seq > m_lastAssembledSeq) {
            m_lastAssembledSeq = fb.seq;
        }

        if (m_videoCallback) {
            m_videoCallback(fb.frameBytes.data(), fb.frameBytes.size(), fb.timestampMs, isKeyframe, isCodecConfig, isHevc);
        }
    }
}

void UdpReceiver::CleanOldUdpvFrames(int32_t currentSeq) {
    auto now = std::chrono::steady_clock::now();
    for (auto it = m_udpvBuffers.begin(); it != m_udpvBuffers.end();) {
        auto ageMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second.lastUpdate).count();
        if (it->first < currentSeq - 30 || ageMs > 1000) {
            it = m_udpvBuffers.erase(it);
        } else {
            ++it;
        }
    }
}
