#pragma once
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <wrl/client.h>
#include <vector>
#include <deque>
#include <mutex>
#include <thread>
#include <atomic>
#include <cstdint>
#include <chrono>

class WasapiPlayer {
public:
    WasapiPlayer();
    ~WasapiPlayer();

    bool Start(int sampleRate = 48000, int channels = 2);
    void Stop();
    bool IsPlaying() const { return m_isPlaying.load(std::memory_order_relaxed); }
    void SetLowLatencyMode(bool enabled) { m_isLowLatencyMode.store(enabled); }

    void PushPcm(const uint8_t* pcmData, size_t bytes, int64_t timestampMs = -1);

    // Audio Master Clock: Returns the estimated PTS (in milliseconds) of audio currently emitted by speakers
    int64_t GetCurrentRenderedAudioPtsMs() const;

    // Synchronize Audio with Video anchor PTS in large buffer mode
    void AlignToAnchorPts(int64_t anchorPtsMs);

    // Active AV-Sync drift correction based on current Video PTS
    void SyncWithVideoPts(int64_t videoPtsMs);

private:
    std::atomic<bool> m_isPlaying{ false };
    std::atomic<bool> m_isLowLatencyMode{ false };
    std::thread m_playThread;

    int m_sampleRate = 48000;
    int m_channels = 2;

    struct AudioSegment {
        int64_t ptsMs = -1;
        size_t sampleCount = 0;
    };

    std::mutex m_queueMutex;
    std::vector<int16_t> m_pcmQueue;
    std::deque<AudioSegment> m_ptsSegments;
    size_t m_queueReadOffset = 0;
    bool m_prebuffering = true;
    size_t m_prebufferSamples = 0;

    std::atomic<int64_t> m_currentAudioPtsMs{ -1 };
    std::atomic<int64_t> m_lastAudioPtsLocalTimeMs{ 0 };

    // 1-second Periodic Stats Tracking
    std::chrono::steady_clock::time_point m_lastStatsTime;
    uint32_t m_statsPruneEvents = 0;
    uint32_t m_statsUnderruns = 0;
    uint32_t m_statsPushedFrames = 0;

    void PlayThreadProc();
    void LogPeriodicStats();
};

