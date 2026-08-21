#include "WasapiPlayer.h"
#include "Logger.h"
#include <avrt.h>

WasapiPlayer::WasapiPlayer() {}

WasapiPlayer::~WasapiPlayer() {
    Stop();
}

bool WasapiPlayer::Start(int sampleRate, int channels) {
    Stop();

    m_sampleRate = sampleRate;
    m_channels = channels;
    m_isPlaying = true;
    m_playThread = std::thread(&WasapiPlayer::PlayThreadProc, this);
    return true;
}

void WasapiPlayer::Stop() {
    if (m_isPlaying.exchange(false)) {
        if (m_playThread.joinable()) {
            m_playThread.join();
        }
    }
    std::lock_guard<std::mutex> lock(m_queueMutex);
    m_pcmQueue.clear();
}

void WasapiPlayer::PushPcm(const uint8_t* pcmData, size_t bytes) {
    if (!m_isPlaying || !pcmData || bytes == 0) return;

    std::lock_guard<std::mutex> lock(m_queueMutex);
    // Limit buffer to max 200ms to avoid audio latency buildup
    size_t maxBytes = (m_sampleRate * m_channels * 2 * 200) / 1000;
    if (m_pcmQueue.size() > maxBytes) {
        m_pcmQueue.erase(m_pcmQueue.begin(), m_pcmQueue.begin() + (m_pcmQueue.size() - maxBytes));
    }
    m_pcmQueue.insert(m_pcmQueue.end(), pcmData, pcmData + bytes);
}

void WasapiPlayer::PlayThreadProc() {
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
        CoUninitialize();
        return;
    }

    Microsoft::WRL::ComPtr<IMMDevice> device;
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device);
    if (FAILED(hr)) {
        CoUninitialize();
        return;
    }

    Microsoft::WRL::ComPtr<IAudioClient> audioClient;
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&audioClient);
    if (FAILED(hr)) {
        CoUninitialize();
        return;
    }

    WAVEFORMATEX* mixFormat = nullptr;
    hr = audioClient->GetMixFormat(&mixFormat);
    if (FAILED(hr) || !mixFormat) {
        Logger::E("WasapiPlayer", "Failed to get mix format");
        CoUninitialize();
        return;
    }

    bool isFloat = false;
    if (mixFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        isFloat = true;
    } else if (mixFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        auto* ex = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mixFormat);
        if (ex->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) {
            isFloat = true;
        }
    }

    int devChannels = mixFormat->nChannels;

    REFERENCE_TIME hnsBufferDuration = 500000; // 50ms buffer
    hr = audioClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        0,
        hnsBufferDuration,
        0,
        mixFormat,
        nullptr
    );

    if (FAILED(hr)) {
        Logger::E("WasapiPlayer", "Failed to initialize audio renderer, hr = " + std::to_string(hr));
        CoTaskMemFree(mixFormat);
        CoUninitialize();
        return;
    }

    UINT32 bufferFrameCount = 0;
    hr = audioClient->GetBufferSize(&bufferFrameCount);
    if (FAILED(hr)) {
        CoTaskMemFree(mixFormat);
        CoUninitialize();
        return;
    }

    Microsoft::WRL::ComPtr<IAudioRenderClient> renderClient;
    hr = audioClient->GetService(__uuidof(IAudioRenderClient), (void**)&renderClient);
    if (FAILED(hr)) {
        CoTaskMemFree(mixFormat);
        CoUninitialize();
        return;
    }

    hr = audioClient->Start();
    if (FAILED(hr)) {
        CoTaskMemFree(mixFormat);
        CoUninitialize();
        return;
    }

    Logger::I("WasapiPlayer", "Audio playback started (Device: " + std::to_string(mixFormat->nSamplesPerSec) + " Hz, " + std::to_string(devChannels) + " ch, float=" + std::to_string(isFloat) + ")");

    std::vector<int16_t> popBuffer;

    while (m_isPlaying) {
        UINT32 numPaddingFrames = 0;
        hr = audioClient->GetCurrentPadding(&numPaddingFrames);
        if (SUCCEEDED(hr)) {
            UINT32 numFramesAvailable = bufferFrameCount - numPaddingFrames;
            if (numFramesAvailable > 0) {
                BYTE* pData = nullptr;
                hr = renderClient->GetBuffer(numFramesAvailable, &pData);
                if (SUCCEEDED(hr) && pData) {
                    size_t inBytesNeeded = numFramesAvailable * m_channels * sizeof(int16_t);
                    bool hasData = false;

                    {
                        std::lock_guard<std::mutex> lock(m_queueMutex);
                        if (m_pcmQueue.size() >= inBytesNeeded) {
                            popBuffer.assign(
                                reinterpret_cast<const int16_t*>(m_pcmQueue.data()),
                                reinterpret_cast<const int16_t*>(m_pcmQueue.data()) + (numFramesAvailable * m_channels)
                            );
                            m_pcmQueue.erase(m_pcmQueue.begin(), m_pcmQueue.begin() + inBytesNeeded);
                            hasData = true;
                        }
                    }

                    if (hasData) {
                        if (isFloat) {
                            float* fDst = reinterpret_cast<float*>(pData);
                            for (UINT32 i = 0; i < numFramesAvailable; ++i) {
                                float left = popBuffer[i * m_channels + 0] / 32768.0f;
                                float right = (m_channels > 1) ? popBuffer[i * m_channels + 1] / 32768.0f : left;

                                fDst[i * devChannels + 0] = left;
                                if (devChannels > 1) fDst[i * devChannels + 1] = right;
                                for (int ch = 2; ch < devChannels; ++ch) {
                                    fDst[i * devChannels + ch] = 0.0f;
                                }
                            }
                        } else {
                            int16_t* sDst = reinterpret_cast<int16_t*>(pData);
                            for (UINT32 i = 0; i < numFramesAvailable; ++i) {
                                int16_t left = popBuffer[i * m_channels + 0];
                                int16_t right = (m_channels > 1) ? popBuffer[i * m_channels + 1] : left;

                                sDst[i * devChannels + 0] = left;
                                if (devChannels > 1) sDst[i * devChannels + 1] = right;
                                for (int ch = 2; ch < devChannels; ++ch) {
                                    sDst[i * devChannels + ch] = 0;
                                }
                            }
                        }
                        renderClient->ReleaseBuffer(numFramesAvailable, 0);
                    } else {
                        // Underflow: write silence
                        renderClient->ReleaseBuffer(numFramesAvailable, AUDCLNT_BUFFERFLAGS_SILENT);
                    }
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    audioClient->Stop();
    CoTaskMemFree(mixFormat);

    if (hAvrt) {
        AvRevertMmThreadCharacteristics(hAvrt);
    }
    CoUninitialize();
}
