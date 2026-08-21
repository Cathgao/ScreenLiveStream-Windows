#include "WasapiCapture.h"
#include "Logger.h"
#include "Protocol.h"
#include <avrt.h>
#include <algorithm>

WasapiCapture::WasapiCapture() {}

WasapiCapture::~WasapiCapture() {
    Stop();
}

bool WasapiCapture::Start() {
    Stop();

    m_isCapturing = true;
    m_captureThread = std::thread(&WasapiCapture::CaptureThreadProc, this);
    return true;
}

void WasapiCapture::Stop() {
    if (m_isCapturing.exchange(false)) {
        if (m_captureThread.joinable()) {
            m_captureThread.join();
        }
    }
}

void WasapiCapture::CaptureThreadProc() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    DWORD taskIndex = 0;
    HANDLE hAvrt = AvSetMmThreadCharacteristicsA("Audio", &taskIndex);

    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator),
        nullptr,
        CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator),
        (void**)&enumerator
    );

    if (FAILED(hr)) {
        Logger::E("WASAPI", "Failed to create MMDeviceEnumerator");
        CoUninitialize();
        return;
    }

    Microsoft::WRL::ComPtr<IMMDevice> device;
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device);
    if (FAILED(hr)) {
        Logger::E("WASAPI", "Failed to get default audio endpoint");
        CoUninitialize();
        return;
    }

    Microsoft::WRL::ComPtr<IAudioClient> audioClient;
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&audioClient);
    if (FAILED(hr)) {
        Logger::E("WASAPI", "Failed to activate IAudioClient");
        CoUninitialize();
        return;
    }

    WAVEFORMATEX* mixFormat = nullptr;
    hr = audioClient->GetMixFormat(&mixFormat);
    if (FAILED(hr) || !mixFormat) {
        Logger::E("WASAPI", "Failed to get mix format");
        CoUninitialize();
        return;
    }

    m_sampleRate = mixFormat->nSamplesPerSec;
    m_channels = mixFormat->nChannels;

    bool isFloat = false;
    if (mixFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        isFloat = true;
    } else if (mixFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        auto* ex = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mixFormat);
        if (ex->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) {
            isFloat = true;
        }
    }

    REFERENCE_TIME hnsBufferDuration = 500000; // 50ms buffer
    hr = audioClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK,
        hnsBufferDuration,
        0,
        mixFormat,
        nullptr
    );

    if (FAILED(hr)) {
        Logger::E("WASAPI", "Failed to initialize audio client for loopback");
        CoTaskMemFree(mixFormat);
        CoUninitialize();
        return;
    }

    Microsoft::WRL::ComPtr<IAudioCaptureClient> captureClient;
    hr = audioClient->GetService(__uuidof(IAudioCaptureClient), (void**)&captureClient);
    if (FAILED(hr)) {
        Logger::E("WASAPI", "Failed to get IAudioCaptureClient");
        CoTaskMemFree(mixFormat);
        CoUninitialize();
        return;
    }

    hr = audioClient->Start();
    if (FAILED(hr)) {
        Logger::E("WASAPI", "Failed to start audio client");
        CoTaskMemFree(mixFormat);
        CoUninitialize();
        return;
    }

    Logger::I("WASAPI", "Loopback capture started: " + std::to_string(m_sampleRate) + " Hz, " + std::to_string(m_channels) + " channels, float=" + std::to_string(isFloat));

    std::vector<int16_t> pcm16Buffer;
    auto lastPacketTime = std::chrono::steady_clock::now();

    while (m_isCapturing) {
        UINT32 packetLength = 0;
        hr = captureClient->GetNextPacketSize(&packetLength);

        if (SUCCEEDED(hr) && packetLength > 0) {
            BYTE* pData = nullptr;
            UINT32 numFramesRead = 0;
            DWORD flags = 0;
            UINT64 devPos = 0;
            UINT64 qpcPos = 0;

            hr = captureClient->GetBuffer(&pData, &numFramesRead, &flags, &devPos, &qpcPos);
            if (SUCCEEDED(hr) && numFramesRead > 0) {
                lastPacketTime = std::chrono::steady_clock::now();
                int64_t timestampNs = Protocol::GetCurrentNanos();

                // Convert float/ext to 16-bit PCM (2 channels)
                size_t totalSamples = numFramesRead * m_channels;
                pcm16Buffer.resize(totalSamples);

                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    std::fill(pcm16Buffer.begin(), pcm16Buffer.end(), static_cast<int16_t>(0));
                } else if (isFloat) {
                    const float* fData = reinterpret_cast<const float*>(pData);
                    for (size_t i = 0; i < totalSamples; ++i) {
                        float sample = fData[i];
                        if (sample > 1.0f) sample = 1.0f;
                        if (sample < -1.0f) sample = -1.0f;
                        pcm16Buffer[i] = static_cast<int16_t>(sample * 32767.0f);
                    }
                } else {
                    const int16_t* sData = reinterpret_cast<const int16_t*>(pData);
                    std::copy(sData, sData + totalSamples, pcm16Buffer.begin());
                }

                if (m_audioCallback) {
                    m_audioCallback(
                        reinterpret_cast<const uint8_t*>(pcm16Buffer.data()),
                        pcm16Buffer.size() * sizeof(int16_t),
                        timestampNs
                    );
                }

                captureClient->ReleaseBuffer(numFramesRead);
            }
        } else {
            // Keep-alive silence injection if quiet for > 20ms
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastPacketTime).count() >= 20) {
                lastPacketTime = now;
                int silentFrames = (m_sampleRate * 20) / 1000;
                size_t totalSamples = silentFrames * m_channels;
                pcm16Buffer.assign(totalSamples, 0);
                int64_t timestampNs = Protocol::GetCurrentNanos();

                if (m_audioCallback) {
                    m_audioCallback(
                        reinterpret_cast<const uint8_t*>(pcm16Buffer.data()),
                        pcm16Buffer.size() * sizeof(int16_t),
                        timestampNs
                    );
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    audioClient->Stop();
    CoTaskMemFree(mixFormat);

    if (hAvrt) {
        AvRevertMmThreadCharacteristics(hAvrt);
    }
    CoUninitialize();
    Logger::I("WASAPI", "Loopback capture stopped.");
}
