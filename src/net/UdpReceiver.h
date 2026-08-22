#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include "Protocol.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <thread>
#include <atomic>
#include <functional>
#include <chrono>

class UdpReceiver {
public:
    using VideoFrameCallback = std::function<void(const uint8_t* data, size_t size, int64_t timestampMs, bool isKeyframe, bool isCodecConfig, bool isHevc)>;
    using AudioFrameCallback = std::function<void(const uint8_t* data, size_t size, int64_t timestampMs)>;
    using StatsCallback = std::function<void(int rttMs, int lossPercentX100)>;

    UdpReceiver();
    ~UdpReceiver();

    bool Start(uint16_t listenPort = 8888);
    void Stop();
    bool IsRunning() const { return m_isRunning; }
    void SetLowLatencyMode(bool enabled) { m_isLowLatencyMode.store(enabled); }

    void SetVideoCallback(VideoFrameCallback cb) { m_videoCallback = cb; }
    void SetAudioCallback(AudioFrameCallback cb) { m_audioCallback = cb; }
    void SetStatsCallback(StatsCallback cb) { m_statsCallback = cb; }

    uint64_t GetAndResetReceivedBytes() {
        return m_receivedBytes.exchange(0);
    }

private:
    std::atomic<bool> m_isRunning{ false };
    std::atomic<bool> m_isLowLatencyMode{ false };
    uint16_t m_listenPort = 8888;
    SOCKET m_sock = INVALID_SOCKET;
    std::thread m_recvThread;

    std::atomic<uint64_t> m_receivedBytes{ 0 };

    VideoFrameCallback m_videoCallback;
    AudioFrameCallback m_audioCallback;
    StatsCallback m_statsCallback;

    // UDPV 28-byte FEC Assembly FrameBuffer
    struct UdpvFrameBuffer {
        int32_t seq = 0;
        uint16_t totalFragments = 0;
        uint32_t frameSize = 0;
        int64_t timestampMs = 0;
        uint8_t flags = 0;
        uint8_t fecGroupSize = 10;
        std::vector<uint8_t> frameBytes;
        std::vector<bool> receivedFragments;
        int receivedDataCount = 0;
        std::unordered_map<int, std::vector<uint8_t>> fecPackets; // groupId -> payload
        std::chrono::steady_clock::time_point firstArrivalTime;
        std::chrono::steady_clock::time_point lastUpdate;
        int fecRecoveredCount = 0;
        bool isCompleted = false;

        size_t GetExpectedFragLength(size_t index) const {
            if (index < totalFragments - 1) return 1300;
            return frameSize - (totalFragments - 1) * 1300;
        }
    };

    std::unordered_map<int32_t, UdpvFrameBuffer> m_udpvBuffers;
    int32_t m_lastAssembledSeq = -1;
    int32_t m_lastSeenSeq = -1;

    // 1-second Periodic Stats Tracking
    std::chrono::steady_clock::time_point m_lastStatsTime;
    uint32_t m_statsPacketsRecv = 0;
    uint32_t m_statsFramesAssembled = 0;
    uint32_t m_statsKeyframes = 0;
    uint32_t m_statsFramesDiscarded = 0;
    uint32_t m_statsFecRecovered = 0;
    uint32_t m_statsSeqJumps = 0;
    double m_statsTotalAssemblyMs = 0.0;
    uint64_t m_statsIntervalBytes = 0;

    sockaddr_in m_senderAddr = {};
    bool m_hasSenderAddr = false;
    std::chrono::steady_clock::time_point m_lastKeyframeRequestTime;

    void RecvThreadProc();
    void ProcessUdpvPacket(const uint8_t* buffer, int length, const sockaddr_in& senderAddr);
    void CheckAndAssembleUdpv(UdpvFrameBuffer& fb);
    void CleanOldUdpvFrames(int32_t currentSeq);
    void RequestKeyframe();
    void LogPeriodicStats();
};
