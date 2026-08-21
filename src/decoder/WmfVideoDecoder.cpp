#include "WmfVideoDecoder.h"
#include "Logger.h"
#include "D3D11Helper.h"

#include <wmcodecdsp.h>
#include <mferror.h>
#include <dxgi.h>

WmfVideoDecoder::WmfVideoDecoder(ID3D11Device* d3d11Device, IMFDXGIDeviceManager* dxgiManager)
    : m_device(d3d11Device), m_dxgiManager(dxgiManager) {
    if (m_device) {
        m_device->GetImmediateContext(&m_context);
    }
}

WmfVideoDecoder::~WmfVideoDecoder() {
    Shutdown();
}

void WmfVideoDecoder::Shutdown() {
    if (m_decoder) {
        m_decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        m_decoder->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
        m_decoder = nullptr;
    }
    m_isInitialized = false;
    Logger::I("WmfVideoDecoder", "Decoder shutdown complete.");
}

bool WmfVideoDecoder::ConfigureOutputType() {
    if (!m_decoder) return false;

    DWORD typeIndex = 0;
    while (true) {
        Microsoft::WRL::ComPtr<IMFMediaType> outType;
        HRESULT hr = m_decoder->GetOutputAvailableType(0, typeIndex++, &outType);
        if (FAILED(hr)) break;

        GUID subtype = GUID_NULL;
        outType->GetGUID(MF_MT_SUBTYPE, &subtype);
        if (subtype == MFVideoFormat_NV12) {
            hr = m_decoder->SetOutputType(0, outType.Get(), 0);
            if (SUCCEEDED(hr)) {
                UINT32 w = 0, h = 0;
                MFGetAttributeSize(outType.Get(), MF_MT_FRAME_SIZE, &w, &h);
                m_videoWidth = w;
                m_videoHeight = h;
                return true;
            }
        }
    }
    return false;
}

bool WmfVideoDecoder::Initialize(VideoCodecType codecType) {
    Shutdown();

    m_codecType = codecType;

    auto TryEnumerateAndActivateDecoder = [this](GUID subtype, const std::string& name) -> bool {
        MFT_REGISTER_TYPE_INFO inputTypeInfo = { MFMediaType_Video, subtype };
        IMFActivate** ppActivate = nullptr;
        UINT32 count = 0;

        HRESULT hr = MFTEnumEx(
            MFT_CATEGORY_VIDEO_DECODER,
            MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
            &inputTypeInfo,
            nullptr,
            &ppActivate,
            &count
        );

        if (FAILED(hr) || count == 0) {
            Logger::W("WmfVideoDecoder", "No hardware " + name + " decoder found, querying all decoders...");
            hr = MFTEnumEx(
                MFT_CATEGORY_VIDEO_DECODER,
                MFT_ENUM_FLAG_ALL | MFT_ENUM_FLAG_SORTANDFILTER,
                &inputTypeInfo,
                nullptr,
                &ppActivate,
                &count
            );
        }

        Logger::I("WmfVideoDecoder", "Found " + std::to_string(count) + " candidates for " + name);

        for (UINT32 i = 0; i < count; ++i) {
            WCHAR friendlyName[256] = {};
            ppActivate[i]->GetString(MFT_FRIENDLY_NAME_Attribute, friendlyName, 256, nullptr);
            char szName[256] = {};
            WideCharToMultiByte(CP_UTF8, 0, friendlyName, -1, szName, sizeof(szName), nullptr, nullptr);
            Logger::I("WmfVideoDecoder", "Trying decoder [" + std::to_string(i) + "]: " + std::string(szName));

            Microsoft::WRL::ComPtr<IMFTransform> decoder;
            hr = ppActivate[i]->ActivateObject(IID_PPV_ARGS(&decoder));
            if (SUCCEEDED(hr) && decoder) {
                Microsoft::WRL::ComPtr<IMFAttributes> attributes;
                if (SUCCEEDED(decoder->GetAttributes(&attributes)) && attributes) {
                    attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
                    attributes->SetUINT32(MF_LOW_LATENCY, TRUE);
                }

                if (m_dxgiManager) {
                    decoder->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(m_dxgiManager.Get()));
                }

                // Set initial input media type with basic frame dimensions to prevent MFT internal access violations
                Microsoft::WRL::ComPtr<IMFMediaType> inMediaType;
                MFCreateMediaType(&inMediaType);
                inMediaType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
                inMediaType->SetGUID(MF_MT_SUBTYPE, subtype);
                MFSetAttributeSize(inMediaType.Get(), MF_MT_FRAME_SIZE, 1920, 1080);
                MFSetAttributeRatio(inMediaType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
                inMediaType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);

                hr = decoder->SetInputType(0, inMediaType.Get(), 0);
                if (FAILED(hr)) {
                    inMediaType->DeleteItem(MF_MT_PIXEL_ASPECT_RATIO);
                    inMediaType->DeleteItem(MF_MT_INTERLACE_MODE);
                    hr = decoder->SetInputType(0, inMediaType.Get(), 0);
                }

                if (SUCCEEDED(hr)) {
                    m_decoder = decoder;
                    ConfigureOutputType();
                    m_decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
                    m_decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

                    Logger::I("WmfVideoDecoder", "Successfully activated decoder: " + std::string(szName) + " (VFR Enabled)");
                    for (UINT32 j = 0; j < count; ++j) ppActivate[j]->Release();
                    CoTaskMemFree(ppActivate);
                    return true;
                } else {
                    Logger::W("WmfVideoDecoder", "SetInputType failed on " + std::string(szName) + ", hr = 0x" + std::to_string(hr));
                }
            }
        }

        if (ppActivate) {
            for (UINT32 j = 0; j < count; ++j) ppActivate[j]->Release();
            CoTaskMemFree(ppActivate);
        }
        return false;
    };

    GUID preferredSubtype = (m_codecType == VideoCodecType::H265_HEVC) ? MFVideoFormat_HEVC : MFVideoFormat_H264;
    std::string preferredName = (m_codecType == VideoCodecType::H265_HEVC) ? "HEVC" : "H.264";

    if (TryEnumerateAndActivateDecoder(preferredSubtype, preferredName)) {
        m_isInitialized = true;
        return true;
    }

    if (m_codecType == VideoCodecType::H265_HEVC) {
        Logger::W("WmfVideoDecoder", "HEVC decoder unavailable, trying fallback to H.264...");
        if (TryEnumerateAndActivateDecoder(MFVideoFormat_H264, "H.264")) {
            m_codecType = VideoCodecType::H264;
            m_isInitialized = true;
            return true;
        }
    }

    Logger::E("WmfVideoDecoder", "Failed to initialize any video decoder MFT.");
    return false;
}

bool WmfVideoDecoder::DecodeNalu(const uint8_t* data, size_t size, int64_t timestampMs) {
    if (!m_isInitialized || !m_decoder || !data || size == 0) return false;

    Microsoft::WRL::ComPtr<IMFMediaBuffer> mediaBuffer;
    HRESULT hr = MFCreateMemoryBuffer(static_cast<DWORD>(size), &mediaBuffer);
    if (FAILED(hr)) return false;

    BYTE* pDst = nullptr;
    if (SUCCEEDED(mediaBuffer->Lock(&pDst, nullptr, nullptr)) && pDst) {
        std::memcpy(pDst, data, size);
        mediaBuffer->Unlock();
        mediaBuffer->SetCurrentLength(static_cast<DWORD>(size));
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

void WmfVideoDecoder::DrainOutput(int64_t timestampMs) {
    MFT_OUTPUT_DATA_BUFFER outputBuffer = {};
    MFT_OUTPUT_STREAM_INFO streamInfo = {};
    m_decoder->GetOutputStreamInfo(0, &streamInfo);

    while (true) {
        Microsoft::WRL::ComPtr<IMFSample> allocatedSample;
        Microsoft::WRL::ComPtr<IMFMediaBuffer> allocatedBuffer;

        outputBuffer.pSample = nullptr;
        outputBuffer.pEvents = nullptr;
        outputBuffer.dwStatus = 0;

        if ((streamInfo.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) == 0) {
            MFCreateSample(&allocatedSample);
            MFCreateMemoryBuffer(streamInfo.cbSize > 0 ? streamInfo.cbSize : (1920 * 1080 * 2), &allocatedBuffer);
            allocatedSample->AddBuffer(allocatedBuffer.Get());
            outputBuffer.pSample = allocatedSample.Get();
        }

        DWORD status = 0;
        HRESULT hr = m_decoder->ProcessOutput(0, 1, &outputBuffer, &status);
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            ConfigureOutputType();
            if (outputBuffer.pSample) outputBuffer.pSample->Release();
            if (outputBuffer.pEvents) outputBuffer.pEvents->Release();
            continue;
        }
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            if (outputBuffer.pSample) outputBuffer.pSample->Release();
            if (outputBuffer.pEvents) outputBuffer.pEvents->Release();
            break;
        }
        if (FAILED(hr)) {
            if (outputBuffer.pSample) outputBuffer.pSample->Release();
            if (outputBuffer.pEvents) outputBuffer.pEvents->Release();
            break;
        }

        IMFSample* pSample = outputBuffer.pSample;
        if (pSample) {
            Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
            if (SUCCEEDED(pSample->GetBufferByIndex(0, &buffer)) && buffer) {
                Microsoft::WRL::ComPtr<IMFDXGIBuffer> dxgiBuffer;
                if (SUCCEEDED(buffer.As(&dxgiBuffer)) && dxgiBuffer) {
                    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
                    if (SUCCEEDED(dxgiBuffer->GetResource(IID_PPV_ARGS(&texture))) && texture) {
                        D3D11_TEXTURE2D_DESC desc;
                        texture->GetDesc(&desc);
                        if (m_decodedCallback) {
                            m_decodedCallback(texture.Get(), timestampMs, desc.Width, desc.Height);
                        }
                    }
                }
            }
        }

        // CRITICAL: Always release sample and events to return surfaces back to MFT texture pool
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
