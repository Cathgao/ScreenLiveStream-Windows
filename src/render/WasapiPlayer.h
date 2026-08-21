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
    bool IsPlaying() const { return m_isPlaying; }

    void PushPcm(const uint8_t* pcmData, size_t bytes);

private:
    std::atomic<bool> m_isPlaying{ false };
    std::thread m_playThread;

    int m_sampleRate = 48000;
    int m_channels = 2;

    std::mutex m_queueMutex;
    std::vector<uint8_t> m_pcmQueue;
    size_t m_queueReadOffset = 0;

    void PlayThreadProc();
};
