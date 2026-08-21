#include "WasapiCapture.h"
#include "Logger.h"
#include "Protocol.h"
#include <avrt.h>
#include <timeapi.h>
#include <algorithm>
#include <cmath>

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
    m_audioCallback = nullptr;
}

void WasapiCapture::CaptureThreadProc() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    timeBeginPeriod(1);

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
        timeEndPeriod(1);
        CoUninitialize();
        return;
    }

    Microsoft::WRL::ComPtr<IMMDevice> device;
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device);
    if (FAILED(hr)) {
        Logger::E("WASAPI", "Failed to get default audio endpoint");
        timeEndPeriod(1);
        CoUninitialize();
        return;
    }

    Microsoft::WRL::ComPtr<IAudioClient> audioClient;
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&audioClient);
    if (FAILED(hr)) {
        Logger::E("WASAPI", "Failed to activate IAudioClient");
        timeEndPeriod(1);
        CoUninitialize();
        return;
    }

    WAVEFORMATEX* mixFormat = nullptr;
    hr = audioClient->GetMixFormat(&mixFormat);
    if (FAILED(hr) || !mixFormat) {
        Logger::E("WASAPI", "Failed to get mix format");
        timeEndPeriod(1);
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
        timeEndPeriod(1);
        CoUninitialize();
        return;
    }

    Microsoft::WRL::ComPtr<IAudioCaptureClient> captureClient;
    hr = audioClient->GetService(__uuidof(IAudioCaptureClient), (void**)&captureClient);
    if (FAILED(hr)) {
        Logger::E("WASAPI", "Failed to get IAudioCaptureClient");
        CoTaskMemFree(mixFormat);
        timeEndPeriod(1);
        CoUninitialize();
        return;
    }

    hr = audioClient->Start();
    if (FAILED(hr)) {
        Logger::E("WASAPI", "Failed to start audio client");
        CoTaskMemFree(mixFormat);
        timeEndPeriod(1);
        CoUninitialize();
        return;
    }

    Logger::I("WASAPI", "Loopback capture started: " + std::to_string(m_sampleRate) + " Hz, " + std::to_string(m_channels) + " channels, float=" + std::to_string(isFloat) + " -> Target 48000Hz Stereo");

    std::vector<int16_t> pcm16Buffer;
    std::vector<float> inFloatL;
    std::vector<float> inFloatR;
    double captureResamplePos = 0.0;
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

                inFloatL.resize(numFramesRead);
                inFloatR.resize(numFramesRead);

                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    std::fill(inFloatL.begin(), inFloatL.end(), 0.0f);
                    std::fill(inFloatR.begin(), inFloatR.end(), 0.0f);
                } else if (isFloat) {
                    const float* fData = reinterpret_cast<const float*>(pData);
                    for (UINT32 i = 0; i < numFramesRead; ++i) {
                        inFloatL[i] = fData[i * m_channels + 0];
                        inFloatR[i] = (m_channels > 1) ? fData[i * m_channels + 1] : inFloatL[i];
                    }
                } else {
                    const int16_t* sData = reinterpret_cast<const int16_t*>(pData);
                    for (UINT32 i = 0; i < numFramesRead; ++i) {
                        inFloatL[i] = sData[i * m_channels + 0] / 32768.0f;
                        inFloatR[i] = (m_channels > 1) ? sData[i * m_channels + 1] / 32768.0f : inFloatL[i];
                    }
                }

                if (m_sampleRate == 48000) {
                    pcm16Buffer.resize(numFramesRead * 2);
                    for (UINT32 i = 0; i < numFramesRead; ++i) {
                        float l = std::clamp(inFloatL[i], -1.0f, 1.0f);
                        float r = std::clamp(inFloatR[i], -1.0f, 1.0f);
                        pcm16Buffer[i * 2 + 0] = static_cast<int16_t>(l * 32767.0f);
                        pcm16Buffer[i * 2 + 1] = static_cast<int16_t>(r * 32767.0f);
                    }
                } else {
                    // Resample hardware sampleRate -> 48000Hz Stereo
                    double ratio = static_cast<double>(m_sampleRate) / 48000.0;
                    size_t outFrames = static_cast<size_t>((numFramesRead - captureResamplePos) / ratio);
                    if (outFrames > 0) {
                        pcm16Buffer.resize(outFrames * 2);
                        for (size_t i = 0; i < outFrames; ++i) {
                            double pos = captureResamplePos + i * ratio;
                            size_t idx = static_cast<size_t>(pos);
                            double frac = pos - idx;

                            float l = 0.0f;
                            float r = 0.0f;
                            if (idx + 1 < numFramesRead) {
                                l = static_cast<float>((1.0 - frac) * inFloatL[idx] + frac * inFloatL[idx + 1]);
                                r = static_cast<float>((1.0 - frac) * inFloatR[idx] + frac * inFloatR[idx + 1]);
                            } else if (idx < numFramesRead) {
                                l = inFloatL[idx];
                                r = inFloatR[idx];
                            }

                            l = std::clamp(l, -1.0f, 1.0f);
                            r = std::clamp(r, -1.0f, 1.0f);
                            pcm16Buffer[i * 2 + 0] = static_cast<int16_t>(l * 32767.0f);
                            pcm16Buffer[i * 2 + 1] = static_cast<int16_t>(r * 32767.0f);
                        }
                        double finalPos = captureResamplePos + outFrames * ratio;
                        captureResamplePos = finalPos - numFramesRead;
                        if (captureResamplePos < 0.0) captureResamplePos = 0.0;
                    } else {
                        captureResamplePos -= numFramesRead;
                        pcm16Buffer.clear();
                    }
                }

                if (m_audioCallback && !pcm16Buffer.empty()) {
                    m_audioCallback(
                        reinterpret_cast<const uint8_t*>(pcm16Buffer.data()),
                        pcm16Buffer.size() * sizeof(int16_t),
                        timestampNs
                    );
                }

                captureClient->ReleaseBuffer(numFramesRead);
            }
        } else {
            // Keep-alive silence injection if quiet for > 20ms (48kHz stereo: 960 frames = 1920 samples)
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastPacketTime).count() >= 20) {
                lastPacketTime = now;
                pcm16Buffer.assign(960 * 2, 0);
                int64_t timestampNs = Protocol::GetCurrentNanos();

                if (m_audioCallback) {
                    m_audioCallback(
                        reinterpret_cast<const uint8_t*>(pcm16Buffer.data()),
                        pcm16Buffer.size() * sizeof(int16_t),
                        timestampNs
                    );
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    audioClient->Stop();
    CoTaskMemFree(mixFormat);

    if (hAvrt) {
        AvRevertMmThreadCharacteristics(hAvrt);
    }
    timeEndPeriod(1);
    CoUninitialize();
    Logger::I("WASAPI", "Loopback capture stopped.");
}
