#include "WmfAudioEncoder.h"
#include "Logger.h"
#include <wmcodecdsp.h>
#include <mferror.h>
#include <mfapi.h>

// Static GUID fallback for AAC Encoder MFT
static const GUID CLSID_AAC_Encoder_MFT = { 0x93305e27, 0x4894, 0x4388, { 0xbf, 0x7b, 0x17, 0x23, 0xb5, 0x84, 0x52, 0xb6 } };

WmfAudioEncoder::WmfAudioEncoder() {}

WmfAudioEncoder::~WmfAudioEncoder() {
    Shutdown();
}

void WmfAudioEncoder::Shutdown() {
    m_isInitialized = false;
    std::lock_guard<std::mutex> lock(m_audioMutex);
    if (m_encoder) {
        m_encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        m_encoder->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
        m_encoder = nullptr;
    }
    m_encodedCallback = nullptr;
}

bool WmfAudioEncoder::Initialize(int sampleRate, int channels, int bitrateBps) {
    Shutdown();

    m_sampleRate = sampleRate;
    m_channels = channels;
    m_bitrateBps = bitrateBps;

    // Try MFTEnumEx first
    MFT_REGISTER_TYPE_INFO inputTypeInfo = { MFMediaType_Audio, MFAudioFormat_PCM };
    MFT_REGISTER_TYPE_INFO outputTypeInfo = { MFMediaType_Audio, MFAudioFormat_AAC };

    IMFActivate** ppActivate = nullptr;
    UINT32 count = 0;
    HRESULT hr = MFTEnumEx(
        MFT_CATEGORY_AUDIO_ENCODER,
        MFT_ENUM_FLAG_ALL | MFT_ENUM_FLAG_SORTANDFILTER,
        &inputTypeInfo,
        &outputTypeInfo,
        &ppActivate,
        &count
    );

    if (SUCCEEDED(hr) && count > 0) {
        hr = ppActivate[0]->ActivateObject(IID_PPV_ARGS(&m_encoder));
        for (UINT32 i = 0; i < count; ++i) {
            ppActivate[i]->Release();
        }
        CoTaskMemFree(ppActivate);
    }

    if (!m_encoder) {
        hr = CoCreateInstance(
            CLSID_AAC_Encoder_MFT,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&m_encoder)
        );
    }

    if (FAILED(hr) || !m_encoder) {
        Logger::E("WmfAudioEncoder", "Failed to create AAC Encoder MFT, hr = " + std::to_string(hr));
        return false;
    }

    // Configure Output Media Type (AAC)
    Microsoft::WRL::ComPtr<IMFMediaType> outMediaType;
    MFCreateMediaType(&outMediaType);
    outMediaType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    outMediaType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
    outMediaType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, m_sampleRate);
    outMediaType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, m_channels);
    outMediaType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    outMediaType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, m_bitrateBps / 8);
    outMediaType->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0); // 0 = RAW AAC frames (matching Android MediaCodec)

    hr = m_encoder->SetOutputType(0, outMediaType.Get(), 0);
    if (FAILED(hr)) {
        Logger::E("WmfAudioEncoder", "Failed to set AAC output type, hr = " + std::to_string(hr));
        return false;
    }

    // Configure Input Media Type (PCM 16-bit)
    Microsoft::WRL::ComPtr<IMFMediaType> inMediaType;
    MFCreateMediaType(&inMediaType);
    inMediaType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    inMediaType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    inMediaType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, m_sampleRate);
    inMediaType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, m_channels);
    inMediaType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    inMediaType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, m_channels * 2);
    inMediaType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, m_sampleRate * m_channels * 2);

    hr = m_encoder->SetInputType(0, inMediaType.Get(), 0);
    if (FAILED(hr)) {
        Logger::E("WmfAudioEncoder", "Failed to set PCM input type, hr = " + std::to_string(hr));
        return false;
    }

    hr = m_encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    hr = m_encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    m_isInitialized = true;
    Logger::I("WmfAudioEncoder", "AAC Audio Encoder initialized (" + std::to_string(m_sampleRate) + " Hz, " + std::to_string(m_channels) + " ch, " + std::to_string(m_bitrateBps / 1000) + " kbps)");
    return true;
}

bool WmfAudioEncoder::EncodePcm(const uint8_t* pcm16Data, size_t bytes, int64_t timestampNs) {
    if (!m_isInitialized || !pcm16Data || bytes == 0) return false;

    std::lock_guard<std::mutex> lock(m_audioMutex);
    if (!m_isInitialized || !m_encoder || !pcm16Data || bytes == 0) return false;

    Microsoft::WRL::ComPtr<IMFMediaBuffer> mediaBuffer;
    HRESULT hr = MFCreateMemoryBuffer(static_cast<DWORD>(bytes), &mediaBuffer);
    if (FAILED(hr)) return false;

    BYTE* pDst = nullptr;
    if (SUCCEEDED(mediaBuffer->Lock(&pDst, nullptr, nullptr)) && pDst) {
        std::memcpy(pDst, pcm16Data, bytes);
        mediaBuffer->Unlock();
        mediaBuffer->SetCurrentLength(static_cast<DWORD>(bytes));
    }

    Microsoft::WRL::ComPtr<IMFSample> sample;
    hr = MFCreateSample(&sample);
    if (FAILED(hr)) return false;

    sample->AddBuffer(mediaBuffer.Get());
    LONGLONG sampleTimeHns = timestampNs / 100;
    LONGLONG sampleDurationHns = (bytes / (m_channels * 2)) * 10000000 / m_sampleRate;
    sample->SetSampleTime(sampleTimeHns);
    sample->SetSampleDuration(sampleDurationHns);

    hr = m_encoder->ProcessInput(0, sample.Get(), 0);
    if (FAILED(hr) && hr != MF_E_NOTACCEPTING) {
        Logger::W("WmfAudioEncoder", "ProcessInput failed, hr = " + std::to_string(hr));
    }

    DrainOutput(timestampNs / 1000000);
    return true;
}

void WmfAudioEncoder::DrainOutput(int64_t timestampMs) {
    MFT_OUTPUT_DATA_BUFFER outputBuffer = {};
    MFT_OUTPUT_STREAM_INFO streamInfo = {};
    m_encoder->GetOutputStreamInfo(0, &streamInfo);

    while (true) {
        Microsoft::WRL::ComPtr<IMFSample> outSample;
        Microsoft::WRL::ComPtr<IMFMediaBuffer> outMediaBuffer;

        if ((streamInfo.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) == 0) {
            MFCreateSample(&outSample);
            MFCreateMemoryBuffer(streamInfo.cbSize > 0 ? streamInfo.cbSize : 4096, &outMediaBuffer);
            outSample->AddBuffer(outMediaBuffer.Get());
            outputBuffer.pSample = outSample.Get();
        }

        DWORD status = 0;
        HRESULT hr = m_encoder->ProcessOutput(0, 1, &outputBuffer, &status);
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            break;
        }
        if (FAILED(hr)) {
            break;
        }

        IMFSample* pSample = outputBuffer.pSample;
        if (!pSample) break;

        Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
        if (SUCCEEDED(pSample->ConvertToContiguousBuffer(&buffer)) && buffer) {
            BYTE* pData = nullptr;
            DWORD maxLen = 0, currentLen = 0;
            if (SUCCEEDED(buffer->Lock(&pData, &maxLen, &currentLen)) && pData && currentLen > 0) {
                if (m_encodedCallback) {
                    m_encodedCallback(pData, currentLen, timestampMs);
                }
                buffer->Unlock();
            }
        }

        if (outputBuffer.pEvents) {
            outputBuffer.pEvents->Release();
            outputBuffer.pEvents = nullptr;
        }
    }
}
