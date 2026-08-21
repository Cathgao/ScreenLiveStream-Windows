#include "WmfVideoEncoder.h"
#include "Logger.h"
#include "D3D11Helper.h"
#include "Protocol.h"

#include <wmcodecdsp.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <propvarutil.h>

WmfVideoEncoder::WmfVideoEncoder(ID3D11Device* d3d11Device, IMFDXGIDeviceManager* dxgiManager)
    : m_device(d3d11Device), m_dxgiManager(dxgiManager) {
    if (m_device) {
        m_device->GetImmediateContext(&m_context);
    }
}

WmfVideoEncoder::~WmfVideoEncoder() {
    Shutdown();
}

void WmfVideoEncoder::Shutdown() {
    m_isEventThreadRunning = false;
    if (m_eventGenerator) {
        Microsoft::WRL::ComPtr<IMFShutdown> shutdown;
        if (SUCCEEDED(m_eventGenerator.As(&shutdown)) && shutdown) {
            shutdown->Shutdown();
        }
    }
    if (m_eventThread.joinable()) {
        m_eventThread.join();
    }
    m_eventGenerator = nullptr;

    if (m_encoder) {
        m_encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        m_encoder->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
        m_encoder = nullptr;
    }
    m_codecApi = nullptr;
    m_outputView = nullptr;
    m_inputView = nullptr;
    m_videoProcessor = nullptr;
    m_videoProcessorEnum = nullptr;
    m_videoContext = nullptr;
    m_videoDevice = nullptr;
    m_nv12Texture = nullptr;
    m_isInitialized = false;
    m_cachedConfigData.clear();
    Logger::I("WmfVideoEncoder", "Encoder shutdown complete.");
}

bool WmfVideoEncoder::InitColorConverter() {
    if (!m_device || !m_context) {
        Logger::E("WmfVideoEncoder", "D3D11 device/context is null.");
        return false;
    }

    HRESULT hr = m_device.As(&m_videoDevice);
    if (FAILED(hr)) {
        Logger::E("WmfVideoEncoder", "Failed to query ID3D11VideoDevice, hr = 0x" + std::to_string(hr));
        return false;
    }

    hr = m_context.As(&m_videoContext);
    if (FAILED(hr)) {
        Logger::E("WmfVideoEncoder", "Failed to query ID3D11VideoContext, hr = 0x" + std::to_string(hr));
        return false;
    }

    D3D11_TEXTURE2D_DESC nv12Desc = {};
    nv12Desc.Width = m_width;
    nv12Desc.Height = m_height;
    nv12Desc.MipLevels = 1;
    nv12Desc.ArraySize = 1;
    nv12Desc.Format = DXGI_FORMAT_NV12;
    nv12Desc.SampleDesc.Count = 1;
    nv12Desc.Usage = D3D11_USAGE_DEFAULT;
    nv12Desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    nv12Desc.MiscFlags = 0;

    hr = m_device->CreateTexture2D(&nv12Desc, nullptr, &m_nv12Texture);
    if (FAILED(hr)) {
        nv12Desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        hr = m_device->CreateTexture2D(&nv12Desc, nullptr, &m_nv12Texture);
        if (FAILED(hr)) {
            Logger::E("WmfVideoEncoder", "Failed to create NV12 texture, hr = 0x" + std::to_string(hr));
            return false;
        }
    }

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC contentDesc = {};
    contentDesc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    contentDesc.InputWidth = m_width;
    contentDesc.InputHeight = m_height;
    contentDesc.OutputWidth = m_width;
    contentDesc.OutputHeight = m_height;
    contentDesc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

    hr = m_videoDevice->CreateVideoProcessorEnumerator(&contentDesc, &m_videoProcessorEnum);
    if (FAILED(hr)) {
        Logger::E("WmfVideoEncoder", "Failed to create VideoProcessorEnumerator, hr = 0x" + std::to_string(hr));
        return false;
    }

    hr = m_videoDevice->CreateVideoProcessor(m_videoProcessorEnum.Get(), 0, &m_videoProcessor);
    if (FAILED(hr)) {
        Logger::E("WmfVideoEncoder", "Failed to create VideoProcessor, hr = 0x" + std::to_string(hr));
        return false;
    }

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outViewDesc = {};
    outViewDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    outViewDesc.Texture2D.MipSlice = 0;
    hr = m_videoDevice->CreateVideoProcessorOutputView(m_nv12Texture.Get(), m_videoProcessorEnum.Get(), &outViewDesc, &m_outputView);
    if (FAILED(hr)) {
        Logger::E("WmfVideoEncoder", "Failed to create VideoProcessorOutputView, hr = 0x" + std::to_string(hr));
        return false;
    }

    return true;
}

bool WmfVideoEncoder::ConvertBgraToNv12(ID3D11Texture2D* bgraTexture) {
    if (!bgraTexture || !m_videoContext || !m_videoProcessor || !m_outputView) return false;

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inViewDesc = {};
    inViewDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    inViewDesc.Texture2D.MipSlice = 0;
    inViewDesc.Texture2D.ArraySlice = 0;

    Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> inputView;
    HRESULT hr = m_videoDevice->CreateVideoProcessorInputView(bgraTexture, m_videoProcessorEnum.Get(), &inViewDesc, &inputView);
    if (FAILED(hr)) {
        static int s_err1 = 0;
        if (s_err1++ < 3) {
            Logger::E("WmfVideoEncoder", "CreateVideoProcessorInputView failed, hr = 0x" + std::to_string(hr));
        }
        return false;
    }

    D3D11_VIDEO_PROCESSOR_STREAM stream = {};
    stream.Enable = TRUE;
    stream.pInputSurface = inputView.Get();

    hr = m_videoContext->VideoProcessorBlt(m_videoProcessor.Get(), m_outputView.Get(), 0, 1, &stream);
    if (FAILED(hr)) {
        static int s_err2 = 0;
        if (s_err2++ < 3) {
            Logger::E("WmfVideoEncoder", "VideoProcessorBlt failed, hr = 0x" + std::to_string(hr));
        }
        return false;
    }
    return true;
}

static bool TrySetupEncoder(
    IMFTransform* encoder,
    IMFDXGIDeviceManager* dxgiManager,
    GUID targetSubtype,
    int width,
    int height,
    int fps,
    int bitrateKbps,
    Microsoft::WRL::ComPtr<ICodecAPI>& outCodecApi
) {
    if (!encoder) return false;

    Microsoft::WRL::ComPtr<IMFAttributes> attributes;
    if (SUCCEEDED(encoder->GetAttributes(&attributes)) && attributes) {
        attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
        attributes->SetUINT32(MF_LOW_LATENCY, TRUE);
    }

    if (dxgiManager) {
        HRESULT hr = encoder->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(dxgiManager));
        if (FAILED(hr)) {
            Logger::W("WmfVideoEncoder", "MFT rejected D3D Manager, hr = 0x" + std::to_string(hr));
        }
    }

    // 1. Output Media Type
    Microsoft::WRL::ComPtr<IMFMediaType> outMediaType;
    MFCreateMediaType(&outMediaType);
    outMediaType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    outMediaType->SetGUID(MF_MT_SUBTYPE, targetSubtype);
    outMediaType->SetUINT32(MF_MT_AVG_BITRATE, bitrateKbps * 1000);
    MFSetAttributeSize(outMediaType.Get(), MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(outMediaType.Get(), MF_MT_FRAME_RATE, fps, 1);
    MFSetAttributeRatio(outMediaType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    outMediaType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);

    if (targetSubtype == MFVideoFormat_HEVC) {
        outMediaType->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH265VProfile_Main_420_8);
    } else {
        outMediaType->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main);
    }

    HRESULT hr = encoder->SetOutputType(0, outMediaType.Get(), 0);
    if (FAILED(hr)) {
        outMediaType->DeleteItem(MF_MT_MPEG2_PROFILE);
        hr = encoder->SetOutputType(0, outMediaType.Get(), 0);
        if (FAILED(hr)) {
            DWORD typeIdx = 0;
            Microsoft::WRL::ComPtr<IMFMediaType> availType;
            bool ok = false;
            while (SUCCEEDED(encoder->GetOutputAvailableType(0, typeIdx++, &availType))) {
                GUID sub = GUID_NULL;
                availType->GetGUID(MF_MT_SUBTYPE, &sub);
                if (sub == targetSubtype) {
                    availType->SetUINT32(MF_MT_AVG_BITRATE, bitrateKbps * 1000);
                    MFSetAttributeSize(availType.Get(), MF_MT_FRAME_SIZE, width, height);
                    MFSetAttributeRatio(availType.Get(), MF_MT_FRAME_RATE, fps, 1);
                    MFSetAttributeRatio(availType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
                    availType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
                    if (SUCCEEDED(encoder->SetOutputType(0, availType.Get(), 0))) {
                        ok = true;
                        break;
                    }
                }
                availType = nullptr;
            }
            if (!ok) {
                Logger::W("WmfVideoEncoder", "Failed to set OutputType on MFT encoder.");
                return false;
            }
        }
    }

    // 2. Input Media Type (NV12)
    Microsoft::WRL::ComPtr<IMFMediaType> inMediaType;
    MFCreateMediaType(&inMediaType);
    inMediaType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    inMediaType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    MFSetAttributeSize(inMediaType.Get(), MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(inMediaType.Get(), MF_MT_FRAME_RATE, fps, 1);
    MFSetAttributeRatio(inMediaType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    inMediaType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);

    hr = encoder->SetInputType(0, inMediaType.Get(), 0);
    if (FAILED(hr)) {
        DWORD inTypeIdx = 0;
        Microsoft::WRL::ComPtr<IMFMediaType> availInType;
        bool inOk = false;
        while (SUCCEEDED(encoder->GetInputAvailableType(0, inTypeIdx++, &availInType))) {
            GUID sub = GUID_NULL;
            availInType->GetGUID(MF_MT_SUBTYPE, &sub);
            if (sub == MFVideoFormat_NV12) {
                MFSetAttributeSize(availInType.Get(), MF_MT_FRAME_SIZE, width, height);
                MFSetAttributeRatio(availInType.Get(), MF_MT_FRAME_RATE, fps, 1);
                MFSetAttributeRatio(availInType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
                availInType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
                if (SUCCEEDED(encoder->SetInputType(0, availInType.Get(), 0))) {
                    inOk = true;
                    break;
                }
            }
            availInType = nullptr;
        }
        if (!inOk) {
            Logger::W("WmfVideoEncoder", "Failed to set InputType on MFT encoder.");
            return false;
        }
    }

    // 3. Low-latency tuning
    if (SUCCEEDED(encoder->QueryInterface(IID_PPV_ARGS(&outCodecApi))) && outCodecApi) {
        VARIANT val;
        VariantInit(&val);

        val.vt = VT_UI4;
        val.ulVal = eAVEncCommonRateControlMode_CBR;
        outCodecApi->SetValue(&CODECAPI_AVEncCommonRateControlMode, &val);

        val.vt = VT_BOOL;
        val.boolVal = VARIANT_TRUE;
        outCodecApi->SetValue(&CODECAPI_AVEncCommonLowLatency, &val);

        val.vt = VT_UI4;
        val.ulVal = fps > 0 ? fps : 60;
        outCodecApi->SetValue(&CODECAPI_AVEncMPVGOPSize, &val);

        val.vt = VT_UI4;
        val.ulVal = 0;
        outCodecApi->SetValue(&CODECAPI_AVEncMPVDefaultBPictureCount, &val);
    }

    encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return true;
}

bool WmfVideoEncoder::Initialize(
    int width,
    int height,
    int fps,
    int bitrateKbps,
    VideoCodecType codecType
) {
    Shutdown();

    m_width = (width / 2) * 2;
    m_height = (height / 2) * 2;
    m_fps = fps > 0 ? fps : 60;
    m_bitrateKbps = bitrateKbps > 0 ? bitrateKbps : 16000;
    m_codecType = codecType;

    Logger::I("WmfVideoEncoder", "Initializing encoder: " + std::to_string(m_width) + "x" + std::to_string(m_height) +
              " @" + std::to_string(m_fps) + "fps, " + std::to_string(m_bitrateKbps) + " Kbps, Requested=" +
              (m_codecType == VideoCodecType::H265_HEVC ? "HEVC" : "H.264"));

    if (!InitColorConverter()) {
        Logger::E("WmfVideoEncoder", "Failed to initialize D3D11 color converter.");
        return false;
    }

    auto TryEnumerateAndActivate = [this](GUID subtype, const std::string& name) -> bool {
        MFT_REGISTER_TYPE_INFO outputTypeInfo = { MFMediaType_Video, subtype };
        IMFActivate** ppActivate = nullptr;
        UINT32 count = 0;

        HRESULT hr = MFTEnumEx(
            MFT_CATEGORY_VIDEO_ENCODER,
            MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
            nullptr,
            &outputTypeInfo,
            &ppActivate,
            &count
        );

        if (FAILED(hr) || count == 0) {
            Logger::W("WmfVideoEncoder", "No hardware " + name + " encoder found, querying all encoders...");
            hr = MFTEnumEx(
                MFT_CATEGORY_VIDEO_ENCODER,
                MFT_ENUM_FLAG_ALL | MFT_ENUM_FLAG_SORTANDFILTER,
                nullptr,
                &outputTypeInfo,
                &ppActivate,
                &count
            );
        }

        Logger::I("WmfVideoEncoder", "Found " + std::to_string(count) + " candidates for " + name);

        for (UINT32 i = 0; i < count; ++i) {
            WCHAR friendlyName[256] = {};
            ppActivate[i]->GetString(MFT_FRIENDLY_NAME_Attribute, friendlyName, 256, nullptr);

            char szName[256] = {};
            WideCharToMultiByte(CP_UTF8, 0, friendlyName, -1, szName, sizeof(szName), nullptr, nullptr);
            Logger::I("WmfVideoEncoder", "Trying encoder [" + std::to_string(i) + "]: " + std::string(szName));

            Microsoft::WRL::ComPtr<IMFTransform> encoder;
            hr = ppActivate[i]->ActivateObject(IID_PPV_ARGS(&encoder));
            if (SUCCEEDED(hr) && encoder) {
                if (TrySetupEncoder(encoder.Get(), m_dxgiManager.Get(), subtype, m_width, m_height, m_fps, m_bitrateKbps, m_codecApi)) {
                    m_encoder = encoder;
                    Logger::I("WmfVideoEncoder", "Successfully activated encoder: " + std::string(szName));
                    for (UINT32 j = 0; j < count; ++j) ppActivate[j]->Release();
                    CoTaskMemFree(ppActivate);
                    return true;
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

    if (TryEnumerateAndActivate(preferredSubtype, preferredName)) {
        m_isInitialized = true;
        m_forceKeyframe = true;
        m_frameIndex = 0;
        if (SUCCEEDED(m_encoder.As(&m_eventGenerator)) && m_eventGenerator) {
            m_isEventThreadRunning = true;
            m_eventThread = std::thread(&WmfVideoEncoder::EventThreadProc, this);
            Logger::I("WmfVideoEncoder", "Asynchronous MFT Event Generator thread started.");
        }
        return true;
    }

    // Fallback to H.264 if HEVC failed
    if (m_codecType == VideoCodecType::H265_HEVC) {
        Logger::W("WmfVideoEncoder", "HEVC encoder unavailable, attempting fallback to H.264...");
        if (TryEnumerateAndActivate(MFVideoFormat_H264, "H.264")) {
            m_codecType = VideoCodecType::H264;
            m_isInitialized = true;
            m_forceKeyframe = true;
            m_frameIndex = 0;
            if (SUCCEEDED(m_encoder.As(&m_eventGenerator)) && m_eventGenerator) {
                m_isEventThreadRunning = true;
                m_eventThread = std::thread(&WmfVideoEncoder::EventThreadProc, this);
                Logger::I("WmfVideoEncoder", "Asynchronous MFT Event Generator thread started.");
            }
            Logger::I("WmfVideoEncoder", "Fallback to H.264 successful.");
            return true;
        }
    }

    Logger::E("WmfVideoEncoder", "Failed to initialize any hardware/software video encoder MFT.");
    return false;
}

void WmfVideoEncoder::RequestKeyFrame() {
    m_forceKeyframe = true;
}

void WmfVideoEncoder::EventThreadProc() {
    Logger::I("WmfVideoEncoder", "Asynchronous MFT Event thread running.");
    while (m_isEventThreadRunning && m_eventGenerator) {
        Microsoft::WRL::ComPtr<IMFMediaEvent> pEvent;
        HRESULT hr = m_eventGenerator->GetEvent(0, &pEvent);
        if (FAILED(hr) || !pEvent) {
            if (!m_isEventThreadRunning) break;
            continue;
        }

        MediaEventType eventType = MEUnknown;
        pEvent->GetType(&eventType);

        if (eventType == METransformHaveOutput) {
            int64_t nowMs = Protocol::GetCurrentMillis();
            DrainOutput(nowMs);
        }
    }
    Logger::I("WmfVideoEncoder", "Asynchronous MFT Event thread stopped.");
}

bool WmfVideoEncoder::EncodeFrame(ID3D11Texture2D* bgraTexture, int64_t timestampNs) {
    if (!m_isInitialized || !m_encoder || !bgraTexture) return false;

    if (!ConvertBgraToNv12(bgraTexture)) {
        return false;
    }

    bool forceThisFrame = m_forceKeyframe || (m_frameIndex <= 15) || (m_frameIndex % (m_fps > 0 ? m_fps : 60) == 0);
    if (forceThisFrame && m_codecApi) {
        VARIANT val;
        VariantInit(&val);
        val.vt = VT_UI4;
        val.ulVal = 1;
        m_codecApi->SetValue(&CODECAPI_AVEncVideoForceKeyFrame, &val);
        m_forceKeyframe = false;
    }

    Microsoft::WRL::ComPtr<IMFMediaBuffer> mediaBuffer;
    HRESULT hr = MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), m_nv12Texture.Get(), 0, FALSE, &mediaBuffer);
    if (FAILED(hr)) return false;

    Microsoft::WRL::ComPtr<IMFSample> sample;
    hr = MFCreateSample(&sample);
    if (FAILED(hr)) return false;

    sample->AddBuffer(mediaBuffer.Get());

    LONGLONG sampleTimeHns = (m_frameIndex++ * 10000000LL) / m_fps;
    LONGLONG sampleDurationHns = 10000000LL / m_fps;
    sample->SetSampleTime(sampleTimeHns);
    sample->SetSampleDuration(sampleDurationHns);

    hr = m_encoder->ProcessInput(0, sample.Get(), 0);
    if (FAILED(hr) && hr != MF_E_NOTACCEPTING) {
        static int s_inErr = 0;
        if (s_inErr++ < 3) {
            Logger::W("WmfVideoEncoder", "ProcessInput failed, hr = 0x" + std::to_string(hr));
        }
    }

    if (!m_eventGenerator) {
        DrainOutput(timestampNs / 1000000);
    }
    return true;
}

void WmfVideoEncoder::DrainOutput(int64_t timestampMs) {
    if (!m_encoder) return;

    MFT_OUTPUT_STREAM_INFO streamInfo = {};
    HRESULT hrInfo = m_encoder->GetOutputStreamInfo(0, &streamInfo);
    if (FAILED(hrInfo)) return;

    while (true) {
        MFT_OUTPUT_DATA_BUFFER outputBuffer = {};
        outputBuffer.dwStreamID = 0;

        Microsoft::WRL::ComPtr<IMFSample> outSample;
        Microsoft::WRL::ComPtr<IMFMediaBuffer> outMediaBuffer;

        bool mftProvidesSamples = (streamInfo.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
        if (!mftProvidesSamples) {
            MFCreateSample(&outSample);
            DWORD bufSize = streamInfo.cbSize > 0 ? streamInfo.cbSize : static_cast<DWORD>(m_width * m_height);
            MFCreateMemoryBuffer(bufSize, &outMediaBuffer);
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
                UINT32 isKeyframeAttr = 0;
                pSample->GetUINT32(MFSampleExtension_CleanPoint, &isKeyframeAttr);
                bool isKeyframe = (isKeyframeAttr != 0);
                bool isHevc = (m_codecType == VideoCodecType::H265_HEVC);
                bool isCodecConfig = false;

                if (!isHevc) {
                    for (DWORD i = 0; i + 3 < currentLen; ++i) {
                        if ((i + 4 <= currentLen && pData[i] == 0 && pData[i+1] == 0 && pData[i+2] == 0 && pData[i+3] == 1) ||
                            (pData[i] == 0 && pData[i+1] == 0 && pData[i+2] == 1)) {
                            int scLen = (pData[i+2] == 1) ? 3 : 4;
                            if (i + scLen < currentLen) {
                                uint8_t nal = pData[i + scLen] & 0x1F;
                                if (nal == 7 || nal == 8) isCodecConfig = true;
                                if (nal == 5) isKeyframe = true;
                            }
                        }
                    }
                } else {
                    for (DWORD i = 0; i + 3 < currentLen; ++i) {
                        if ((i + 4 <= currentLen && pData[i] == 0 && pData[i+1] == 0 && pData[i+2] == 0 && pData[i+3] == 1) ||
                            (pData[i] == 0 && pData[i+1] == 0 && pData[i+2] == 1)) {
                            int scLen = (pData[i+2] == 1) ? 3 : 4;
                            if (i + scLen < currentLen) {
                                uint8_t nal = (pData[i + scLen] >> 1) & 0x3F;
                                if (nal == 32 || nal == 33 || nal == 34) isCodecConfig = true;
                                if (nal == 19 || nal == 20 || nal == 21) isKeyframe = true;
                            }
                        }
                    }
                }

                if (isKeyframe) {
                    static std::atomic<int> s_encodedFrames{ 0 };
                    int fNum = ++s_encodedFrames;
                    Logger::I("WmfVideoEncoder", "Encoded Keyframe #" + std::to_string(fNum) + " (" + std::to_string(currentLen) + " bytes, isHevc=" + std::to_string(isHevc) + ")");
                }

                if (m_encodedCallback) {
                    m_encodedCallback(pData, currentLen, timestampMs, isKeyframe, isCodecConfig, isHevc);
                }

                buffer->Unlock();
            }
        }

        if (mftProvidesSamples && outputBuffer.pSample) {
            outputBuffer.pSample->Release();
            outputBuffer.pSample = nullptr;
        }

        if (outputBuffer.pEvents) {
            outputBuffer.pEvents->Release();
            outputBuffer.pEvents = nullptr;
        }
    }
}
