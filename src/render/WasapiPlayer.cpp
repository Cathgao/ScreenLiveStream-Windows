#include "WasapiPlayer.h"
#include "Logger.h"
#include "Protocol.h"
#include <avrt.h>
#include <timeapi.h>
#include <cmath>
#include <algorithm>

#ifndef AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
#define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM 0x80000000
#endif
#ifndef AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000
#endif

WasapiPlayer::WasapiPlayer() {}

WasapiPlayer::~WasapiPlayer() {
    Stop();
}

bool WasapiPlayer::Start(int sampleRate, int channels) {
    Stop();

    m_sampleRate = (sampleRate > 0) ? sampleRate : 48000;
    m_channels = (channels > 0) ? channels : 2;
    size_t prebufferMs = m_isLowLatencyMode.load(std::memory_order_relaxed) ? 35 : 2000;
    m_prebufferSamples = static_cast<size_t>((m_sampleRate * m_channels * prebufferMs) / 1000);
    m_prebuffering = true;
    m_currentAudioPtsMs.store(-1, std::memory_order_relaxed);
    m_lastAudioPtsLocalTimeMs.store(0, std::memory_order_relaxed);

    m_lastStatsTime = std::chrono::steady_clock::now();
    m_statsPruneEvents = 0;
    m_statsUnderruns = 0;
    m_statsPushedFrames = 0;

    m_isPlaying.store(true, std::memory_order_release);
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
    m_ptsSegments.clear();
    m_queueReadOffset = 0;
    m_prebuffering = true;
    m_currentAudioPtsMs.store(-1, std::memory_order_relaxed);
    m_lastAudioPtsLocalTimeMs.store(0, std::memory_order_relaxed);
}

int64_t WasapiPlayer::GetCurrentRenderedAudioPtsMs() const {
    int64_t basePts = m_currentAudioPtsMs.load(std::memory_order_relaxed);
    if (basePts < 0) return -1;
    int64_t elapsed = Protocol::GetCurrentMillis() - m_lastAudioPtsLocalTimeMs.load(std::memory_order_relaxed);
    if (elapsed > 400) {
        // Audio stream paused / stalled
        return -1;
    }
    return basePts + elapsed;
}

void WasapiPlayer::AlignToAnchorPts(int64_t anchorPtsMs) {
    if (anchorPtsMs < 0) return;
    std::lock_guard<std::mutex> lock(m_queueMutex);
    m_prebuffering = false;

    // Drop audio segments prior to anchorPtsMs to ensure video & audio start simultaneously at the same PTS
    size_t dropSamples = 0;
    while (!m_ptsSegments.empty() && (m_ptsSegments.front().ptsMs + 30 < anchorPtsMs)) {
        dropSamples += m_ptsSegments.front().sampleCount;
        m_ptsSegments.pop_front();
    }

    if (dropSamples > 0) {
        m_queueReadOffset = std::min(m_pcmQueue.size(), m_queueReadOffset + dropSamples);
        if (m_queueReadOffset >= m_pcmQueue.size()) {
            m_pcmQueue.clear();
            m_queueReadOffset = 0;
        }
        Logger::I("WasapiPlayer", "[AV_ALIGN] Aligned audio queue to anchorPts=" + std::to_string(anchorPtsMs) +
                  " (dropped " + std::to_string(dropSamples) + " pre-anchor samples)");
    }
}

void WasapiPlayer::SyncWithVideoPts(int64_t videoPtsMs) {
    if (!m_isPlaying.load(std::memory_order_relaxed) || videoPtsMs < 0) return;

    int64_t audioPts = GetCurrentRenderedAudioPtsMs();
    if (audioPts < 0) return;

    bool isLowLatency = m_isLowLatencyMode.load(std::memory_order_relaxed);
    // In low-latency mode, video takes ~30ms to reach screen through D3D11 swapchain/VSync,
    // so audio target PTS at speaker should be videoPtsMs - 30ms.
    int64_t targetAudioPts = isLowLatency ? (videoPtsMs - 30) : videoPtsMs;
    int64_t diffMs = audioPts - targetAudioPts;

    // If audio is lagging behind video (diffMs < -70ms in large buffer or < -80ms in low latency)
    if (diffMs < -70) {
        int64_t catchUpMs = -diffMs - 20; // catch up most of the gap smoothly
        size_t skipSamples = static_cast<size_t>((m_sampleRate * m_channels * catchUpMs) / 1000);

        std::lock_guard<std::mutex> lock(m_queueMutex);
        size_t available = (m_pcmQueue.size() > m_queueReadOffset) ? (m_pcmQueue.size() - m_queueReadOffset) : 0;
        size_t actualSkip = std::min(available, skipSamples);
        if (actualSkip > 0) {
            m_queueReadOffset += actualSkip;
            size_t rem = actualSkip;
            while (rem > 0 && !m_ptsSegments.empty()) {
                if (m_ptsSegments.front().sampleCount <= rem) {
                    rem -= m_ptsSegments.front().sampleCount;
                    m_ptsSegments.pop_front();
                } else {
                    m_ptsSegments.front().sampleCount -= rem;
                    m_ptsSegments.front().ptsMs += static_cast<int64_t>((rem * 1000.0) / (m_sampleRate * m_channels));
                    rem = 0;
                }
            }
            if (m_queueReadOffset >= m_pcmQueue.size()) {
                m_pcmQueue.clear();
                m_queueReadOffset = 0;
            }
            Logger::W("WasapiPlayer", "[AV_SYNC] Audio lagging behind video by " + std::to_string(-diffMs) +
                      " ms. Fast-forwarded " + std::to_string(actualSkip) + " samples (~" + std::to_string(catchUpMs) + " ms)");
        }
    }
}

void WasapiPlayer::PushPcm(const uint8_t* pcmData, size_t bytes, int64_t timestampMs) {
    if (!m_isPlaying.load(std::memory_order_relaxed) || !pcmData || bytes < sizeof(int16_t)) return;

    size_t sampleCount = bytes / sizeof(int16_t);
    const int16_t* pSrc = reinterpret_cast<const int16_t*>(pcmData);

    std::lock_guard<std::mutex> lock(m_queueMutex);
    m_statsPushedFrames++;

    // Periodic compaction when offset exceeds 8K samples (~85ms)
    if (m_queueReadOffset > 8192) {
        m_pcmQueue.erase(m_pcmQueue.begin(), m_pcmQueue.begin() + m_queueReadOffset);
        m_queueReadOffset = 0;
    }

    // Audio buffer cap (90ms in low latency, 2500ms in large buffer mode)
    bool isLowLatency = m_isLowLatencyMode.load(std::memory_order_relaxed);
    size_t maxBacklogMs = isLowLatency ? 90 : 2500;
    size_t maxBacklog = static_cast<size_t>((m_sampleRate * m_channels * maxBacklogMs) / 1000);
    size_t activeSamples = m_pcmQueue.size() - m_queueReadOffset;
    if (activeSamples > maxBacklog) {
        m_statsPruneEvents++;
        size_t targetActiveMs = isLowLatency ? 35 : 2000;
        size_t targetActive = static_cast<size_t>((m_sampleRate * m_channels * targetActiveMs) / 1000);
        size_t pruneCount = (m_pcmQueue.size() > targetActive) ? (m_pcmQueue.size() - targetActive - m_queueReadOffset) : 0;
        int64_t prunedMs = static_cast<int64_t>((pruneCount * 1000.0) / (m_sampleRate * m_channels));
        Logger::W("WasapiPlayer", "[AUDIO_PRUNE] Backlog reached " + std::to_string(activeSamples * 1000 / (m_sampleRate * m_channels)) +
                  " ms (>" + std::to_string(maxBacklogMs) + "ms max). Pruning " + std::to_string(pruneCount) + " samples (~" + std::to_string(prunedMs) +
                  " ms) to avoid audio latency accumulation");

        m_queueReadOffset = (m_pcmQueue.size() > targetActive) ? (m_pcmQueue.size() - targetActive) : 0;

        // Prune PTS segments to stay synchronized with audio buffer truncation
        while (pruneCount > 0 && !m_ptsSegments.empty()) {
            if (m_ptsSegments.front().sampleCount <= pruneCount) {
                pruneCount -= m_ptsSegments.front().sampleCount;
                m_ptsSegments.pop_front();
            } else {
                m_ptsSegments.front().sampleCount -= pruneCount;
                m_ptsSegments.front().ptsMs += static_cast<int64_t>((pruneCount * 1000.0) / (m_sampleRate * m_channels));
                pruneCount = 0;
            }
        }
    }

    m_pcmQueue.insert(m_pcmQueue.end(), pSrc, pSrc + sampleCount);
    if (timestampMs >= 0) {
        m_ptsSegments.push_back({ timestampMs, sampleCount });
    } else if (!m_ptsSegments.empty()) {
        m_ptsSegments.back().sampleCount += sampleCount;
    }
}

void WasapiPlayer::PlayThreadProc() {
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
        timeEndPeriod(1);
        CoUninitialize();
        return;
    }

    Microsoft::WRL::ComPtr<IMMDevice> device;
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device);
    if (FAILED(hr)) {
        timeEndPeriod(1);
        CoUninitialize();
        return;
    }

    Microsoft::WRL::ComPtr<IAudioClient> audioClient;
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&audioClient);
    if (FAILED(hr)) {
        timeEndPeriod(1);
        CoUninitialize();
        return;
    }

    // 1. Try Native WASAPI AutoConvertPCM (Windows 10/11 system resampler)
    WAVEFORMATEX clientFmt = {};
    clientFmt.wFormatTag = WAVE_FORMAT_PCM;
    clientFmt.nChannels = static_cast<WORD>(m_channels);
    clientFmt.nSamplesPerSec = static_cast<DWORD>(m_sampleRate);
    clientFmt.wBitsPerSample = 16;
    clientFmt.nBlockAlign = (clientFmt.nChannels * clientFmt.wBitsPerSample) / 8;
    clientFmt.nAvgBytesPerSec = clientFmt.nSamplesPerSec * clientFmt.nBlockAlign;
    clientFmt.cbSize = 0;

    REFERENCE_TIME hnsBufferDuration = 200000; // 20ms low-latency hardware buffer
    bool useNativeAutoConvert = false;
    WAVEFORMATEX* mixFormat = nullptr;

    hr = audioClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
        hnsBufferDuration,
        0,
        &clientFmt,
        nullptr
    );

    if (SUCCEEDED(hr)) {
        useNativeAutoConvert = true;
        Logger::I("WasapiPlayer", "WASAPI initialized with native AutoConvertPCM (" + std::to_string(m_sampleRate) + " Hz, " + std::to_string(m_channels) + " ch)");
    } else {
        Logger::W("WasapiPlayer", "Native AutoConvertPCM initialize failed (hr = 0x" + std::to_string(hr) + "), using device MixFormat & software resampler fallback.");
        hr = audioClient->GetMixFormat(&mixFormat);
        if (FAILED(hr) || !mixFormat) {
            Logger::E("WasapiPlayer", "Failed to get mix format");
            timeEndPeriod(1);
            if (hAvrt) AvRevertMmThreadCharacteristics(hAvrt);
            CoUninitialize();
            return;
        }

        hr = audioClient->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            0,
            hnsBufferDuration,
            0,
            mixFormat,
            nullptr
        );

        if (FAILED(hr)) {
            Logger::E("WasapiPlayer", "Failed to initialize audio renderer with mixFormat, hr = 0x" + std::to_string(hr));
            CoTaskMemFree(mixFormat);
            timeEndPeriod(1);
            if (hAvrt) AvRevertMmThreadCharacteristics(hAvrt);
            CoUninitialize();
            return;
        }
    }

    UINT32 bufferFrameCount = 0;
    hr = audioClient->GetBufferSize(&bufferFrameCount);
    if (FAILED(hr)) {
        if (mixFormat) CoTaskMemFree(mixFormat);
        timeEndPeriod(1);
        if (hAvrt) AvRevertMmThreadCharacteristics(hAvrt);
        CoUninitialize();
        return;
    }

    Microsoft::WRL::ComPtr<IAudioRenderClient> renderClient;
    hr = audioClient->GetService(__uuidof(IAudioRenderClient), (void**)&renderClient);
    if (FAILED(hr)) {
        if (mixFormat) CoTaskMemFree(mixFormat);
        timeEndPeriod(1);
        if (hAvrt) AvRevertMmThreadCharacteristics(hAvrt);
        CoUninitialize();
        return;
    }

    hr = audioClient->Start();
    if (FAILED(hr)) {
        if (mixFormat) CoTaskMemFree(mixFormat);
        timeEndPeriod(1);
        if (hAvrt) AvRevertMmThreadCharacteristics(hAvrt);
        CoUninitialize();
        return;
    }

    int devSampleRate = m_sampleRate;
    int devChannels = m_channels;
    bool isFloat = false;

    if (!useNativeAutoConvert && mixFormat) {
        devSampleRate = mixFormat->nSamplesPerSec;
        devChannels = mixFormat->nChannels;
        if (mixFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
            isFloat = true;
        } else if (mixFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
            auto* ex = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mixFormat);
            if (ex->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) {
                isFloat = true;
            }
        }
        Logger::I("WasapiPlayer", "Audio playback fallback (Device: " + std::to_string(devSampleRate) + " Hz, " + std::to_string(devChannels) + " ch, float=" + std::to_string(isFloat) + ")");
    }

    std::vector<int16_t> tempInputPcm;
    double softwareResamplePos = 0.0;

    auto updateAudioPts = [&](size_t consumedSamples, UINT32 paddingFrames) {
        size_t remaining = consumedSamples;
        int64_t currentPts = -1;
        while (remaining > 0 && !m_ptsSegments.empty()) {
            if (m_ptsSegments.front().sampleCount <= remaining) {
                remaining -= m_ptsSegments.front().sampleCount;
                currentPts = m_ptsSegments.front().ptsMs;
                m_ptsSegments.pop_front();
            } else {
                currentPts = m_ptsSegments.front().ptsMs;
                m_ptsSegments.front().sampleCount -= remaining;
                m_ptsSegments.front().ptsMs += static_cast<int64_t>((remaining * 1000.0) / (m_sampleRate * m_channels));
                remaining = 0;
            }
        }
        if (currentPts >= 0) {
            int64_t wasapiDelayMs = (static_cast<int64_t>(paddingFrames) * 1000LL) / m_sampleRate;
            int64_t actualSpeakerPts = (std::max)(int64_t(0), currentPts - wasapiDelayMs);
            m_currentAudioPtsMs.store(actualSpeakerPts, std::memory_order_relaxed);
            m_lastAudioPtsLocalTimeMs.store(Protocol::GetCurrentMillis(), std::memory_order_relaxed);
        }
    };

    while (m_isPlaying.load(std::memory_order_relaxed)) {
        UINT32 numPaddingFrames = 0;
        hr = audioClient->GetCurrentPadding(&numPaddingFrames);
        if (SUCCEEDED(hr)) {
            UINT32 numFramesAvailable = (bufferFrameCount > numPaddingFrames) ? (bufferFrameCount - numPaddingFrames) : 0;
            if (numFramesAvailable > 0) {
                BYTE* pData = nullptr;
                hr = renderClient->GetBuffer(numFramesAvailable, &pData);
                if (SUCCEEDED(hr) && pData) {
                    if (useNativeAutoConvert) {
                        size_t samplesNeeded = numFramesAvailable * m_channels;
                        int16_t* pDst16 = reinterpret_cast<int16_t*>(pData);

                        std::lock_guard<std::mutex> lock(m_queueMutex);
                        size_t availableSamples = (m_pcmQueue.size() > m_queueReadOffset) ? (m_pcmQueue.size() - m_queueReadOffset) : 0;

                        if (m_prebuffering) {
                            if (availableSamples >= m_prebufferSamples) {
                                m_prebuffering = false;
                            } else {
                                std::memset(pData, 0, samplesNeeded * sizeof(int16_t));
                                renderClient->ReleaseBuffer(numFramesAvailable, 0);
                                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                                continue;
                            }
                        }

                        if (availableSamples >= samplesNeeded) {
                            const int16_t* pSrc = m_pcmQueue.data() + m_queueReadOffset;
                            std::memcpy(pDst16, pSrc, samplesNeeded * sizeof(int16_t));
                            m_queueReadOffset += samplesNeeded;
                            updateAudioPts(samplesNeeded, numPaddingFrames);
                            if (m_queueReadOffset >= m_pcmQueue.size()) {
                                m_pcmQueue.clear();
                                m_queueReadOffset = 0;
                            }
                            renderClient->ReleaseBuffer(numFramesAvailable, 0);
                        } else if (availableSamples > 0) {
                            // Partial read: copy available samples and zero-pad remainder to maintain smooth timing
                            const int16_t* pSrc = m_pcmQueue.data() + m_queueReadOffset;
                            std::memcpy(pDst16, pSrc, availableSamples * sizeof(int16_t));
                            std::memset(pDst16 + availableSamples, 0, (samplesNeeded - availableSamples) * sizeof(int16_t));
                            updateAudioPts(availableSamples, numPaddingFrames);
                            m_pcmQueue.clear();
                            m_queueReadOffset = 0;
                            renderClient->ReleaseBuffer(numFramesAvailable, 0);
                        } else {
                            std::memset(pData, 0, samplesNeeded * sizeof(int16_t));
                            renderClient->ReleaseBuffer(numFramesAvailable, AUDCLNT_BUFFERFLAGS_SILENT);
                        }
                    } else {
                        // Software Resampling Fallback
                        double ratio = static_cast<double>(m_sampleRate) / static_cast<double>(devSampleRate);
                        double totalInFramesNeeded = numFramesAvailable * ratio + 2.0;
                        size_t inFramesToFetch = static_cast<size_t>(std::ceil(totalInFramesNeeded));
                        size_t inSamplesToFetch = inFramesToFetch * m_channels;

                        size_t availableSamples = 0;
                        {
                            std::lock_guard<std::mutex> lock(m_queueMutex);
                            availableSamples = (m_pcmQueue.size() > m_queueReadOffset) ? (m_pcmQueue.size() - m_queueReadOffset) : 0;

                            if (m_prebuffering) {
                                if (availableSamples >= m_prebufferSamples) {
                                    m_prebuffering = false;
                                } else {
                                    renderClient->ReleaseBuffer(numFramesAvailable, AUDCLNT_BUFFERFLAGS_SILENT);
                                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                                    continue;
                                }
                            }

                            size_t copySamples = std::min(availableSamples, inSamplesToFetch);
                            if (copySamples > 0) {
                                const int16_t* pSrc = m_pcmQueue.data() + m_queueReadOffset;
                                tempInputPcm.assign(pSrc, pSrc + copySamples);
                            } else {
                                tempInputPcm.clear();
                            }
                        }

                        size_t inFramesAvailable = tempInputPcm.size() / m_channels;

                        if (inFramesAvailable >= 2) {
                            if (isFloat) {
                                float* fDst = reinterpret_cast<float*>(pData);
                                for (UINT32 i = 0; i < numFramesAvailable; ++i) {
                                    double pos = softwareResamplePos + i * ratio;
                                    size_t idx = static_cast<size_t>(pos);
                                    double frac = pos - idx;

                                    if (idx + 1 < inFramesAvailable) {
                                        float left = static_cast<float>((1.0 - frac) * tempInputPcm[idx * m_channels + 0] + frac * tempInputPcm[(idx + 1) * m_channels + 0]) / 32768.0f;
                                        float right = (m_channels > 1) ?
                                            static_cast<float>((1.0 - frac) * tempInputPcm[idx * m_channels + 1] + frac * tempInputPcm[(idx + 1) * m_channels + 1]) / 32768.0f : left;

                                        fDst[i * devChannels + 0] = left;
                                        if (devChannels > 1) fDst[i * devChannels + 1] = right;
                                        for (int ch = 2; ch < devChannels; ++ch) fDst[i * devChannels + ch] = 0.0f;
                                    } else {
                                        for (int ch = 0; ch < devChannels; ++ch) fDst[i * devChannels + ch] = 0.0f;
                                    }
                                }
                            } else {
                                int16_t* sDst = reinterpret_cast<int16_t*>(pData);
                                for (UINT32 i = 0; i < numFramesAvailable; ++i) {
                                    double pos = softwareResamplePos + i * ratio;
                                    size_t idx = static_cast<size_t>(pos);
                                    double frac = pos - idx;

                                    if (idx + 1 < inFramesAvailable) {
                                        int16_t left = static_cast<int16_t>(std::clamp((1.0 - frac) * tempInputPcm[idx * m_channels + 0] + frac * tempInputPcm[(idx + 1) * m_channels + 0], -32768.0, 32767.0));
                                        int16_t right = (m_channels > 1) ?
                                            static_cast<int16_t>(std::clamp((1.0 - frac) * tempInputPcm[idx * m_channels + 1] + frac * tempInputPcm[(idx + 1) * m_channels + 1], -32768.0, 32767.0)) : left;

                                        sDst[i * devChannels + 0] = left;
                                        if (devChannels > 1) sDst[i * devChannels + 1] = right;
                                        for (int ch = 2; ch < devChannels; ++ch) sDst[i * devChannels + ch] = 0;
                                    } else {
                                        for (int ch = 0; ch < devChannels; ++ch) sDst[i * devChannels + ch] = 0;
                                    }
                                }
                            }

                            double finalPos = softwareResamplePos + numFramesAvailable * ratio;
                            size_t consumedInFrames = static_cast<size_t>(finalPos);
                            softwareResamplePos = finalPos - consumedInFrames;

                            {
                                std::lock_guard<std::mutex> lock(m_queueMutex);
                                size_t consumedSamples = std::min(consumedInFrames * m_channels, m_pcmQueue.size() - m_queueReadOffset);
                                m_queueReadOffset += consumedSamples;
                                updateAudioPts(consumedSamples, numPaddingFrames);
                                if (m_queueReadOffset >= m_pcmQueue.size()) {
                                    m_pcmQueue.clear();
                                    m_queueReadOffset = 0;
                                }
                            }
                            renderClient->ReleaseBuffer(numFramesAvailable, 0);
                        } else {
                            m_statsUnderruns++;
                            softwareResamplePos = 0.0;
                            renderClient->ReleaseBuffer(numFramesAvailable, AUDCLNT_BUFFERFLAGS_SILENT);
                        }
                    }
                }
            }
        }
        LogPeriodicStats();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    audioClient->Stop();
    if (mixFormat) {
        CoTaskMemFree(mixFormat);
    }

    if (hAvrt) {
        AvRevertMmThreadCharacteristics(hAvrt);
    }
    timeEndPeriod(1);
    CoUninitialize();
}

void WasapiPlayer::LogPeriodicStats() {
    auto now = std::chrono::steady_clock::now();
    auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastStatsTime).count();
    if (elapsedMs >= 1000) {
        size_t bufferedSamples = 0;
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            bufferedSamples = (m_pcmQueue.size() > m_queueReadOffset) ? (m_pcmQueue.size() - m_queueReadOffset) : 0;
        }
        double bufferMs = (bufferedSamples * 1000.0) / (m_sampleRate * m_channels);
        int64_t renderedPts = GetCurrentRenderedAudioPtsMs();
        Logger::I("WasapiPlayer", "[STATS 1s] Rendered Audio PTS: " + std::to_string(renderedPts) +
                  " ms, Buffer: " + std::to_string(bufferedSamples) + " samples (" + std::to_string(bufferMs) +
                  " ms), In: " + std::to_string(m_statsPushedFrames) + " frames, Underruns: " +
                  std::to_string(m_statsUnderruns) + ", Prunes: " + std::to_string(m_statsPruneEvents));

        m_statsPruneEvents = 0;
        m_statsUnderruns = 0;
        m_statsPushedFrames = 0;
        m_lastStatsTime = now;
    }
}



