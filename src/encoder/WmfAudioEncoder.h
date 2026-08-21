#pragma once
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wrl/client.h>
#include <vector>
#include <functional>
#include <cstdint>

class WmfAudioEncoder {
public:
    using AudioEncodedCallback = std::function<void(const uint8_t* data, size_t size, int64_t timestampMs)>;

    WmfAudioEncoder();
    ~WmfAudioEncoder();

    bool Initialize(int sampleRate = 48000, int channels = 2, int bitrateBps = 128000);
    void Shutdown();

    bool EncodePcm(const uint8_t* pcm16Data, size_t bytes, int64_t timestampNs);
    void SetEncodedCallback(AudioEncodedCallback cb) { m_encodedCallback = cb; }

    bool IsInitialized() const { return m_isInitialized; }

private:
    Microsoft::WRL::ComPtr<IMFTransform> m_encoder;
    int m_sampleRate = 48000;
    int m_channels = 2;
    int m_bitrateBps = 128000;
    bool m_isInitialized = false;
    AudioEncodedCallback m_encodedCallback;

    void DrainOutput(int64_t timestampMs);
};
