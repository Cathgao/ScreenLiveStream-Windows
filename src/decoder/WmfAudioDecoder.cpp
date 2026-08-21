#include "WmfAudioDecoder.h"
#include "Logger.h"
#include <wmcodecdsp.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <mmreg.h>

#pragma pack(push, 1)
struct HEAACWAVEFORMAT_STRUCT {
    WAVEFORMATEX wfx;
    WORD wPayloadType;
    WORD wAudioProfileLevelIndication;
    WORD wStructType;
    WORD wReserved1;
    DWORD dwReserved2;
    BYTE pbAudioSpecificConfig[2];
};
#pragma pack(pop)

WmfAudioDecoder::WmfAudioDecoder() {}

WmfAudioDecoder::~WmfAudioDecoder() {
    Shutdown();
}

void WmfAudioDecoder::Shutdown() {
    if (m_decoder) {
        m_decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        m_decoder->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
        m_decoder = nullptr;
    }
    m_isInitialized = false;
    Logger::I("WmfAudioDecoder", "Audio Decoder shutdown complete.");
}

bool WmfAudioDecoder::Initialize(int sampleRate, int channels) {
    Shutdown();

    m_sampleRate = sampleRate;
    m_channels = channels;

    HRESULT hr = CoCreateInstance(
        CLSID_CMSAACDecMFT,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&m_decoder)
    );

    if (FAILED(hr) || !m_decoder) {
        Logger::E("WmfAudioDecoder", "Failed to create CLSID_CMSAACDecMFT, hr = 0x" + std::to_string(hr));
        return false;
    }

    // Initialize using standard HEAACWAVEFORMAT
    HEAACWAVEFORMAT_STRUCT aacFmt = {};
    aacFmt.wfx.wFormatTag = WAVE_FORMAT_MPEG_HEAAC;
    aacFmt.wfx.nChannels = static_cast<WORD>(m_channels);
    aacFmt.wfx.nSamplesPerSec = static_cast<DWORD>(m_sampleRate);
    aacFmt.wfx.nAvgBytesPerSec = 16000;
    aacFmt.wfx.nBlockAlign = 1;
    aacFmt.wfx.wBitsPerSample = 16;
    aacFmt.wfx.cbSize = sizeof(HEAACWAVEFORMAT_STRUCT) - sizeof(WAVEFORMATEX);
    aacFmt.wPayloadType = 0; // Raw AAC
    aacFmt.wAudioProfileLevelIndication = 0xFE;
    aacFmt.wStructType = 0;
    aacFmt.wReserved1 = 0;
    aacFmt.dwReserved2 = 0;

    uint8_t freqIdx = (m_sampleRate == 44100) ? 4 : 3; // 3 = 48000 Hz
    uint16_t asc = static_cast<uint16_t>((2 << 11) | (freqIdx << 7) | (m_channels << 3));
    aacFmt.pbAudioSpecificConfig[0] = static_cast<uint8_t>((asc >> 8) & 0xFF);
    aacFmt.pbAudioSpecificConfig[1] = static_cast<uint8_t>(asc & 0xFF);

    Microsoft::WRL::ComPtr<IMFMediaType> inMediaType;
    hr = MFCreateMediaType(&inMediaType);
    if (SUCCEEDED(hr)) {
        hr = MFInitMediaTypeFromWaveFormatEx(inMediaType.Get(), reinterpret_cast<const WAVEFORMATEX*>(&aacFmt), sizeof(aacFmt));
    }

    if (FAILED(hr)) {
        inMediaType = nullptr;
        MFCreateMediaType(&inMediaType);
        inMediaType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        inMediaType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
        inMediaType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, m_sampleRate);
        inMediaType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, m_channels);
        inMediaType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    }

    hr = m_decoder->SetInputType(0, inMediaType.Get(), 0);
    if (FAILED(hr)) {
        Logger::W("WmfAudioDecoder", "SetInputType failed, trying ADTS payload type, hr = 0x" + std::to_string(hr));
        inMediaType->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 1); // ADTS
        hr = m_decoder->SetInputType(0, inMediaType.Get(), 0);
    }

    if (FAILED(hr)) {
        Logger::W("WmfAudioDecoder", "Failed to set AAC input type, audio disabled for this session.");
        return false;
    }

    // Set Output Media Type (PCM)
    Microsoft::WRL::ComPtr<IMFMediaType> outMediaType;
    MFCreateMediaType(&outMediaType);
    outMediaType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    outMediaType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    outMediaType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, m_sampleRate);
    outMediaType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, m_channels);
    outMediaType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    outMediaType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, m_channels * 2);
    outMediaType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, m_sampleRate * m_channels * 2);

    hr = m_decoder->SetOutputType(0, outMediaType.Get(), 0);
    if (FAILED(hr)) {
        Logger::E("WmfAudioDecoder", "Failed to set PCM output type, hr = 0x" + std::to_string(hr));
        return false;
    }

    hr = m_decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    hr = m_decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    m_isInitialized = true;
    Logger::I("WmfAudioDecoder", "AAC Audio Decoder initialized successfully.");
    return true;
}

bool WmfAudioDecoder::DecodeAac(const uint8_t* aacData, size_t bytes, int64_t timestampMs) {
    if (!m_isInitialized || !m_decoder || !aacData || bytes <= 4) return false;

    Microsoft::WRL::ComPtr<IMFMediaBuffer> mediaBuffer;
    HRESULT hr = MFCreateMemoryBuffer(static_cast<DWORD>(bytes), &mediaBuffer);
    if (FAILED(hr)) return false;

    BYTE* pDst = nullptr;
    if (SUCCEEDED(mediaBuffer->Lock(&pDst, nullptr, nullptr)) && pDst) {
        std::memcpy(pDst, aacData, bytes);
        mediaBuffer->Unlock();
        mediaBuffer->SetCurrentLength(static_cast<DWORD>(bytes));
    }

    Microsoft::WRL::ComPtr<IMFSample> sample;
    hr = MFCreateSample(&sample);
    if (FAILED(hr)) return false;

    sample->AddBuffer(mediaBuffer.Get());
    sample->SetSampleTime(timestampMs * 10000);

    hr = m_decoder->ProcessInput(0, sample.Get(), 0);
    if (FAILED(hr) && hr != MF_E_NOTACCEPTING) {
        // Ignored
    }

    DrainOutput(timestampMs);
    return true;
}

void WmfAudioDecoder::DrainOutput(int64_t timestampMs) {
    MFT_OUTPUT_DATA_BUFFER outputBuffer = {};
    MFT_OUTPUT_STREAM_INFO streamInfo = {};
    m_decoder->GetOutputStreamInfo(0, &streamInfo);

    while (true) {
        outputBuffer.pSample = nullptr;
        outputBuffer.pEvents = nullptr;
        outputBuffer.dwStatus = 0;

        if ((streamInfo.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) == 0) {
            IMFMediaBuffer* pMediaBuffer = nullptr;
            DWORD bufSize = streamInfo.cbSize > 0 ? streamInfo.cbSize : 16384;
            if (SUCCEEDED(MFCreateMemoryBuffer(bufSize, &pMediaBuffer)) && pMediaBuffer) {
                if (SUCCEEDED(MFCreateSample(&outputBuffer.pSample)) && outputBuffer.pSample) {
                    outputBuffer.pSample->AddBuffer(pMediaBuffer);
                }
                pMediaBuffer->Release();
            }
        }

        DWORD status = 0;
        HRESULT hr = m_decoder->ProcessOutput(0, 1, &outputBuffer, &status);
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT || FAILED(hr)) {
            if (outputBuffer.pSample) {
                outputBuffer.pSample->Release();
                outputBuffer.pSample = nullptr;
            }
            if (outputBuffer.pEvents) {
                outputBuffer.pEvents->Release();
                outputBuffer.pEvents = nullptr;
            }
            break;
        }

        IMFSample* pSample = outputBuffer.pSample;
        if (pSample) {
            Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
            if (SUCCEEDED(pSample->ConvertToContiguousBuffer(&buffer)) && buffer) {
                BYTE* pData = nullptr;
                DWORD maxLen = 0, currentLen = 0;
                if (SUCCEEDED(buffer->Lock(&pData, &maxLen, &currentLen)) && pData && currentLen > 0) {
                    if (m_decodedCallback) {
                        m_decodedCallback(pData, currentLen, timestampMs);
                    }
                    buffer->Unlock();
                }
            }
        }

        if (outputBuffer.pSample) {
            outputBuffer.pSample->Release();
            outputBuffer.pSample = nullptr;
        }
        if (outputBuffer.pEvents) {
            outputBuffer.pEvents->Release();
            outputBuffer.pEvents = nullptr;
        }
    }
}
