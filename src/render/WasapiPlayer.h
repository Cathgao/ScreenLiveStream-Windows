#pragma once
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <wrl/client.h>
#include <vector>
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

    void SetAudioDelayMs(int delayMs);
    int GetAudioDelayMs() const { return m_audioDelayMs.load(std::memory_order_relaxed); }

private:
    std::atomic<bool> m_isPlaying{ false };
    std::atomic<int> m_audioDelayMs{ 0 };
    std::thread m_playThread;

    int m_sampleRate = 48000;
    int m_channels = 2;

    std::mutex m_queueMutex;
    std::vector<int16_t> m_pcmQueue;
    size_t m_queueReadOffset = 0;
    bool m_prebuffering = true;
    size_t m_prebufferSamples = 1440; // ~15ms @ 48kHz stereo

    void PlayThreadProc();
};

