#pragma once
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wrl/client.h>
#include <vector>
#include <functional>
#include <cstdint>

class WmfAudioDecoder {
public:
    using AudioDecodedCallback = std::function<void(const uint8_t* pcmData, size_t bytes, int64_t timestampMs)>;

    WmfAudioDecoder();
    ~WmfAudioDecoder();

    bool Initialize(int sampleRate = 48000, int channels = 2);
    void Shutdown();

    bool DecodeAac(const uint8_t* aacData, size_t bytes, int64_t timestampMs);
    void SetDecodedCallback(AudioDecodedCallback cb) { m_decodedCallback = cb; }

    bool IsInitialized() const { return m_isInitialized; }

private:
    Microsoft::WRL::ComPtr<IMFTransform> m_decoder;
    int m_sampleRate = 48000;
    int m_channels = 2;
    bool m_isInitialized = false;
    AudioDecodedCallback m_decodedCallback;

    void DrainOutput(int64_t timestampMs);
};
