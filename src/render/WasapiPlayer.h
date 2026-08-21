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

class WasapiPlayer {
public:
    WasapiPlayer();
    ~WasapiPlayer();

    bool Start(int sampleRate = 48000, int channels = 2);
    void Stop();
    bool IsPlaying() const { return m_isPlaying.load(std::memory_order_relaxed); }

    void PushPcm(const uint8_t* pcmData, size_t bytes, int64_t timestampMs = -1);

    // Audio Master Clock: Returns the estimated PTS (in milliseconds) of audio currently emitted by speakers
    int64_t GetCurrentRenderedAudioPtsMs() const;

private:
    std::atomic<bool> m_isPlaying{ false };
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
    size_t m_prebufferSamples = 1440; // ~15ms minimal hardware prebuffer (zero artificial pipeline delay)

    std::atomic<int64_t> m_currentAudioPtsMs{ -1 };
    std::atomic<int64_t> m_lastAudioPtsLocalTimeMs{ 0 };

    void PlayThreadProc();
};

