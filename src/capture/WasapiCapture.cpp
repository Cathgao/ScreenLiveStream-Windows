#include "WasapiCapture.h"
#include "Logger.h"
#include "Protocol.h"
#include <wrl/implements.h>
#include <avrt.h>
#include <timeapi.h>
#include <algorithm>
#include <cmath>

namespace {

class AudioEndpointNotifier : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
    IMMNotificationClient>
{
public:
    AudioEndpointNotifier(std::atomic<bool>& deviceChanged) : m_deviceChanged(deviceChanged) {}

    STDMETHODIMP OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR /*pwstrDeviceId*/) override {
        if (flow == eRender && (role == eMultimedia || role == eConsole)) {
            m_deviceChanged.store(true, std::memory_order_release);
        }
        return S_OK;
    }

    STDMETHODIMP OnDeviceStateChanged(LPCWSTR /*pwstrDeviceId*/, DWORD /*dwNewState*/) override {
        m_deviceChanged.store(true, std::memory_order_release);
        return S_OK;
    }

    STDMETHODIMP OnDeviceAdded(LPCWSTR /*pwstrDeviceId*/) override { return S_OK; }
    STDMETHODIMP OnDeviceRemoved(LPCWSTR /*pwstrDeviceId*/) override { return S_OK; }
    STDMETHODIMP OnPropertyValueChanged(LPCWSTR /*pwstrDeviceId*/, const PROPERTYKEY /*key*/) override { return S_OK; }

private:
    std::atomic<bool>& m_deviceChanged;
};

} // namespace

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

    if (FAILED(hr) || !enumerator) {
        Logger::E("WASAPI", "Failed to create MMDeviceEnumerator");
        timeEndPeriod(1);
        if (hAvrt) AvRevertMmThreadCharacteristics(hAvrt);
        CoUninitialize();
        return;
    }

    std::atomic<bool> deviceChanged{ false };
    auto notifier = Microsoft::WRL::Make<AudioEndpointNotifier>(deviceChanged);
    enumerator->RegisterEndpointNotificationCallback(notifier.Get());

    auto initLoopback = [&](
        Microsoft::WRL::ComPtr<IMMDevice>& outDevice,
        Microsoft::WRL::ComPtr<IAudioClient>& outClient,
        Microsoft::WRL::ComPtr<IAudioCaptureClient>& outCapture,
        WAVEFORMATEX*& outFormat,
        bool& outIsFloat
    ) -> bool {
        if (outClient) {
            outClient->Stop();
            outClient = nullptr;
        }
        outCapture = nullptr;
        outDevice = nullptr;
        if (outFormat) {
            CoTaskMemFree(outFormat);
            outFormat = nullptr;
        }

        HRESULT localHr = enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &outDevice);
        if (FAILED(localHr) || !outDevice) {
            return false;
        }

        localHr = outDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&outClient);
        if (FAILED(localHr) || !outClient) {
            return false;
        }

        localHr = outClient->GetMixFormat(&outFormat);
        if (FAILED(localHr) || !outFormat) {
            return false;
        }

        m_sampleRate.store(outFormat->nSamplesPerSec, std::memory_order_relaxed);
        m_channels.store(outFormat->nChannels, std::memory_order_relaxed);

        outIsFloat = false;
        if (outFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
            outIsFloat = true;
        } else if (outFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
            auto* ex = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(outFormat);
            if (ex->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) {
                outIsFloat = true;
            }
        }

        REFERENCE_TIME hnsBufferDuration = 500000; // 50ms buffer
        localHr = outClient->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_LOOPBACK,
            hnsBufferDuration,
            0,
            outFormat,
            nullptr
        );
        if (FAILED(localHr)) {
            CoTaskMemFree(outFormat);
            outFormat = nullptr;
            outClient = nullptr;
            return false;
        }

        localHr = outClient->GetService(__uuidof(IAudioCaptureClient), (void**)&outCapture);
        if (FAILED(localHr) || !outCapture) {
            CoTaskMemFree(outFormat);
            outFormat = nullptr;
            outClient = nullptr;
            return false;
        }

        localHr = outClient->Start();
        if (FAILED(localHr)) {
            CoTaskMemFree(outFormat);
            outFormat = nullptr;
            outCapture = nullptr;
            outClient = nullptr;
            return false;
        }

        Logger::I("WASAPI", "Loopback capture initialized: " +
                  std::to_string(m_sampleRate.load()) + " Hz, " +
                  std::to_string(m_channels.load()) + " ch, float=" +
                  std::to_string(outIsFloat) + " -> Target 48000Hz Stereo");
        return true;
    };

    Microsoft::WRL::ComPtr<IMMDevice> device;
    Microsoft::WRL::ComPtr<IAudioClient> audioClient;
    Microsoft::WRL::ComPtr<IAudioCaptureClient> captureClient;
    WAVEFORMATEX* mixFormat = nullptr;
    bool isFloat = false;

    bool clientReady = initLoopback(device, audioClient, captureClient, mixFormat, isFloat);

    std::vector<int16_t> pcm16Buffer;
    std::vector<float> inFloatL;
    std::vector<float> inFloatR;
    double captureResamplePos = 0.0;
    auto lastPacketTime = std::chrono::steady_clock::now();

    while (m_isCapturing) {
        bool needsReinit = deviceChanged.exchange(false);

        if (!clientReady || needsReinit) {
            if (needsReinit) {
                Logger::I("WASAPI", "Default audio device change detected. Re-initializing loopback capture...");
            }
            clientReady = initLoopback(device, audioClient, captureClient, mixFormat, isFloat);
            if (clientReady) {
                captureResamplePos = 0.0;
                lastPacketTime = std::chrono::steady_clock::now();
            } else {
                // If device switch is in transition, inject keep-alive silence
                auto now = std::chrono::steady_clock::now();
                if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastPacketTime).count() >= 20) {
                    lastPacketTime = now;
                    pcm16Buffer.assign(960 * 2, 0);
                    int64_t timestampNs = Protocol::GetCurrentNanos();
                    if (m_audioCallback) {
                        m_audioCallback(reinterpret_cast<const uint8_t*>(pcm16Buffer.data()), pcm16Buffer.size() * sizeof(int16_t), timestampNs);
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
        }

        UINT32 packetLength = 0;
        hr = captureClient->GetNextPacketSize(&packetLength);

        if (hr == AUDCLNT_E_DEVICE_INVALIDATED ||
            hr == AUDCLNT_E_RESOURCES_INVALIDATED ||
            hr == AUDCLNT_E_SERVICE_NOT_RUNNING ||
            FAILED(hr))
        {
            Logger::W("WASAPI", "Audio device invalidated or error (hr = 0x" + std::to_string(hr) + "). Triggering re-initialization...");
            clientReady = false;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        if (packetLength > 0) {
            BYTE* pData = nullptr;
            UINT32 numFramesRead = 0;
            DWORD flags = 0;
            UINT64 devPos = 0;
            UINT64 qpcPos = 0;

            hr = captureClient->GetBuffer(&pData, &numFramesRead, &flags, &devPos, &qpcPos);
            if (hr == AUDCLNT_E_DEVICE_INVALIDATED || hr == AUDCLNT_E_RESOURCES_INVALIDATED) {
                Logger::W("WASAPI", "GetBuffer device invalidated. Triggering re-initialization...");
                clientReady = false;
                continue;
            }

            if (SUCCEEDED(hr) && numFramesRead > 0) {
                lastPacketTime = std::chrono::steady_clock::now();
                int64_t timestampNs = Protocol::GetCurrentNanos();

                int curSampleRate = m_sampleRate.load(std::memory_order_relaxed);
                int curChannels = m_channels.load(std::memory_order_relaxed);

                inFloatL.resize(numFramesRead);
                inFloatR.resize(numFramesRead);

                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    std::fill(inFloatL.begin(), inFloatL.end(), 0.0f);
                    std::fill(inFloatR.begin(), inFloatR.end(), 0.0f);
                } else if (isFloat) {
                    const float* fData = reinterpret_cast<const float*>(pData);
                    for (UINT32 i = 0; i < numFramesRead; ++i) {
                        inFloatL[i] = fData[i * curChannels + 0];
                        inFloatR[i] = (curChannels > 1) ? fData[i * curChannels + 1] : inFloatL[i];
                    }
                } else {
                    const int16_t* sData = reinterpret_cast<const int16_t*>(pData);
                    for (UINT32 i = 0; i < numFramesRead; ++i) {
                        inFloatL[i] = sData[i * curChannels + 0] / 32768.0f;
                        inFloatR[i] = (curChannels > 1) ? sData[i * curChannels + 1] / 32768.0f : inFloatL[i];
                    }
                }

                if (curSampleRate == 48000) {
                    pcm16Buffer.resize(numFramesRead * 2);
                    for (UINT32 i = 0; i < numFramesRead; ++i) {
                        float l = std::clamp(inFloatL[i], -1.0f, 1.0f);
                        float r = std::clamp(inFloatR[i], -1.0f, 1.0f);
                        pcm16Buffer[i * 2 + 0] = static_cast<int16_t>(l * 32767.0f);
                        pcm16Buffer[i * 2 + 1] = static_cast<int16_t>(r * 32767.0f);
                    }
                } else {
                    // Resample hardware sampleRate -> 48000Hz Stereo
                    double ratio = static_cast<double>(curSampleRate) / 48000.0;
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

    if (enumerator && notifier) {
        enumerator->UnregisterEndpointNotificationCallback(notifier.Get());
    }

    if (audioClient) {
        audioClient->Stop();
    }
    if (mixFormat) {
        CoTaskMemFree(mixFormat);
    }

    if (hAvrt) {
        AvRevertMmThreadCharacteristics(hAvrt);
    }
    timeEndPeriod(1);
    CoUninitialize();
    Logger::I("WASAPI", "Loopback capture stopped.");
}
