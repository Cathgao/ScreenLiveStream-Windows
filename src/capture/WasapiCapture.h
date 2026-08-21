#pragma once
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <wrl/client.h>
#include <vector>
#include <functional>
#include <thread>
#include <atomic>
#include <cstdint>

class WasapiCapture {
public:
    // Callback receiving 16-bit stereo PCM audio (48kHz default)
    using AudioCallback = std::function<void(const uint8_t* pcmData, size_t bytes, int64_t timestampNs)>;

    WasapiCapture();
    ~WasapiCapture();

    bool Start();
    void Stop();
    bool IsCapturing() const { return m_isCapturing; }

    void SetAudioCallback(AudioCallback cb) { m_audioCallback = cb; }

    int GetSampleRate() const { return m_sampleRate; }
    int GetChannels() const { return m_channels; }

private:
    std::atomic<bool> m_isCapturing{ false };
    std::thread m_captureThread;
    AudioCallback m_audioCallback;

    int m_sampleRate = 48000;
    int m_channels = 2;

    void CaptureThreadProc();
};
