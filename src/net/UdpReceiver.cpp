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
    m_lastSeenSeq = -1;
    m_udpvBuffers.clear();

    m_lastStatsTime = std::chrono::steady_clock::now();
    m_statsPacketsRecv = 0;
    m_statsFramesAssembled = 0;
    m_statsKeyframes = 0;
    m_statsFramesDiscarded = 0;
    m_statsFecRecovered = 0;
    m_statsSeqJumps = 0;
    m_statsTotalAssemblyMs = 0.0;
    m_statsIntervalBytes = 0;

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
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    m_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_sock == INVALID_SOCKET) return;

    BOOL reuse = TRUE;
    setsockopt(m_sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));

    int rcvBuf = 16 * 1024 * 1024; // 16MB buffer for UDP packets
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
        m_statsIntervalBytes += bytes;
        m_statsPacketsRecv++;

        // 1. Check for STREAM_STOP packet
        if (bytes >= 22 && buffer[0] == 'Q' && buffer[1] == 'C') {
            if (Protocol::IsStreamStopPacket(buffer, bytes)) {
                Logger::I("UdpReceiver", "Received Stream Stop packet.");
                m_lastAssembledSeq = -1;
                m_lastSeenSeq = -1;
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
                LogPeriodicStats();
                continue;
            }
        }
    }

    closesocket(m_sock);
    m_sock = INVALID_SOCKET;
}

void UdpReceiver::ProcessUdpvPacket(const uint8_t* buffer, int length, const sockaddr_in& senderAddr) {
    m_senderAddr = senderAddr;
    m_hasSenderAddr = true;
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

    if (m_lastSeenSeq >= 0 && seq != m_lastSeenSeq + 1) {
        if (seq > m_lastSeenSeq + 1) {
            int diff = seq - m_lastSeenSeq - 1;
            m_statsSeqJumps += diff;
            Logger::W("UdpReceiver", "[SEQ_DISCONTINUITY] Video seq jumped from " + std::to_string(m_lastSeenSeq) +
                      " to " + std::to_string(seq) + " (missing " + std::to_string(diff) + " frames)");
        }
    }
    m_lastSeenSeq = seq;

    if (seq <= m_lastAssembledSeq) {
        bool isRestart = (m_lastAssembledSeq - seq > 500);
        if (isRestart) {
            Logger::I("UdpReceiver", "Stream seq reset / restart detected (oldAssembled=" + std::to_string(m_lastAssembledSeq) + ", newSeq=" + std::to_string(seq) + ")");
            m_lastAssembledSeq = -1;
            m_udpvBuffers.clear();
        } else {
            auto itCheck = m_udpvBuffers.find(seq);
            if (itCheck == m_udpvBuffers.end() || itCheck->second.isCompleted) {
                return;
            }
        }
    }

    auto it = m_udpvBuffers.find(seq);
    if (it == m_udpvBuffers.end()) {
        uint8_t fecGroupSize = buffer[25];
        if (fecGroupSize == 0) fecGroupSize = 10;

        UdpvFrameBuffer fb;
        fb.seq = seq;
        fb.totalFragments = totalFragments;
        fb.frameSize = frameSize;
        fb.timestampMs = ts;
        fb.flags = flags;
        fb.fecGroupSize = fecGroupSize;
        fb.frameBytes.resize(frameSize);
        fb.receivedFragments.resize(totalFragments, false);
        fb.receivedDataCount = 0;
        fb.firstArrivalTime = std::chrono::steady_clock::now();
        fb.lastUpdate = fb.firstArrivalTime;
        fb.isCompleted = false;

        m_udpvBuffers[seq] = std::move(fb);
        CleanOldUdpvFrames(seq);
        it = m_udpvBuffers.find(seq);
    }

    if (it == m_udpvBuffers.end()) return;

    auto& fb = it->second;
    fb.lastUpdate = std::chrono::steady_clock::now();

    if (isFec) {
        int groupId = fragIndex;
        if (groupId >= 0 && fb.fecPackets.find(groupId) == fb.fecPackets.end()) {
            size_t fecLen = length - 28;
            fb.fecPackets[groupId] = std::vector<uint8_t>(buffer + 28, buffer + 28 + fecLen);
        }
    } else {
        if (fragIndex < fb.totalFragments && fragIndex < fb.receivedFragments.size() && !fb.receivedFragments[fragIndex]) {
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

    // XOR FEC Recovery using sender-negotiated group size
    int groupSize = fb.fecGroupSize > 0 ? fb.fecGroupSize : 10;
    if (fb.receivedDataCount < fb.totalFragments && !fb.fecPackets.empty()) {
        for (const auto& [groupId, fecPayload] : fb.fecPackets) {
            int startIdx = groupId * groupSize;
            int endIdx = (std::min)(startIdx + groupSize, static_cast<int>(fb.totalFragments));

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
                    fb.fecRecoveredCount++;
                    m_statsFecRecovered++;
                    Logger::I("UdpReceiver", "[FEC] Recovered missing frag " + std::to_string(missingIdx) + "/" +
                              std::to_string(fb.totalFragments) + " for Frame #" + std::to_string(fb.seq) + " (FEC Group " + std::to_string(groupId) + ")");
                }
            } else if (missingCount > 1) {
                Logger::D("UdpReceiver", "[FEC] Group " + std::to_string(groupId) + " of Frame #" + std::to_string(fb.seq) +
                          " missing " + std::to_string(missingCount) + " frags (unrecoverable by single XOR)");
            }
        }
    }

    if (fb.receivedDataCount == fb.totalFragments) {
        fb.isCompleted = true;
        bool isKeyframe = (fb.flags & 1) != 0;
        bool isCodecConfig = (fb.flags & 2) != 0;
        bool isHevc = (fb.flags & 4) != 0;

        double assemblyMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - fb.firstArrivalTime).count();
        m_statsFramesAssembled++;
        m_statsTotalAssemblyMs += assemblyMs;
        if (isKeyframe) m_statsKeyframes++;

        if (assemblyMs > 25.0) {
            Logger::W("UdpReceiver", "[SLOW_ASSEMBLY] Frame #" + std::to_string(fb.seq) + " (" +
                      std::to_string(fb.frameSize) + " bytes, " + std::to_string(fb.totalFragments) +
                      " frags, FEC recovered: " + std::to_string(fb.fecRecoveredCount) + ") took " +
                      std::to_string(assemblyMs) + " ms to assemble");
        }

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
        if (it->first < currentSeq - 15 || ageMs > 150) {
            if (!it->second.isCompleted) {
                m_statsFramesDiscarded++;
                Logger::W("UdpReceiver", "[FRAME_DROP] Discarding incomplete Frame #" + std::to_string(it->first) +
                          " (age=" + std::to_string(ageMs) + "ms, received " +
                          std::to_string(it->second.receivedDataCount) + "/" + std::to_string(it->second.totalFragments) +
                          " data frags, " + std::to_string(it->second.fecPackets.size()) + " FEC frags, size=" +
                          std::to_string(it->second.frameSize) + " bytes, isKey=" + std::to_string((it->second.flags & 1) != 0) + ")");
                RequestKeyframe();
            }
            it = m_udpvBuffers.erase(it);
        } else {
            ++it;
        }
    }
}

void UdpReceiver::RequestKeyframe() {
    if (m_sock == INVALID_SOCKET || !m_hasSenderAddr) return;
    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastKeyframeRequestTime).count() < 100) {
        return;
    }
    m_lastKeyframeRequestTime = now;
    uint8_t idrReq[22] = {};
    idrReq[0] = 'Q';
    idrReq[1] = 'C';
    idrReq[2] = 0x01;
    idrReq[3] = (uint8_t)0x80; // FLAG_IDR_REQUEST = 0x80
    sendto(m_sock, (const char*)idrReq, 22, 0, (sockaddr*)&m_senderAddr, sizeof(m_senderAddr));
    Logger::I("UdpReceiver", "[IDR_REQUEST] Sent instant IDR Keyframe Request (PLI) to sender.");
}

void UdpReceiver::LogPeriodicStats() {
    auto now = std::chrono::steady_clock::now();
    auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastStatsTime).count();
    if (elapsedMs >= 1000) {
        double mbps = (m_statsIntervalBytes * 8.0) / (elapsedMs * 1000.0);
        double avgAssembly = m_statsFramesAssembled > 0 ? (m_statsTotalAssemblyMs / m_statsFramesAssembled) : 0.0;
        Logger::I("UdpReceiver", "[STATS 1s] Recv: " + std::to_string(m_statsPacketsRecv) + " pkts (" +
                  std::to_string(mbps) + " Mbps), Assembled: " + std::to_string(m_statsFramesAssembled) +
                  " frames (Key: " + std::to_string(m_statsKeyframes) + "), Discarded: " +
                  std::to_string(m_statsFramesDiscarded) + ", FEC Recovered: " +
                  std::to_string(m_statsFecRecovered) + ", Seq Discontinuities: " +
                  std::to_string(m_statsSeqJumps) + ", Avg Assembly: " + std::to_string(avgAssembly) + " ms");

        m_statsPacketsRecv = 0;
        m_statsFramesAssembled = 0;
        m_statsKeyframes = 0;
        m_statsFramesDiscarded = 0;
        m_statsFecRecovered = 0;
        m_statsSeqJumps = 0;
        m_statsTotalAssemblyMs = 0.0;
        m_statsIntervalBytes = 0;
        m_lastStatsTime = now;
    }
}
