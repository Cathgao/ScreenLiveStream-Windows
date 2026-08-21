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
    m_isInitialized = false;

    std::lock_guard<std::mutex> lock(m_encoderMutex);

    if (m_encoder) {
        m_encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        m_encoder->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
        m_encoder = nullptr;
    }
    m_codecApi = nullptr;
    for (int i = 0; i < NV12_RING_SIZE; ++i) {
        m_outputViews[i] = nullptr;
        m_nv12Textures[i] = nullptr;
    }
    m_inputViews.clear();
    m_videoProcessor = nullptr;
    m_videoProcessorEnum = nullptr;
    m_videoContext = nullptr;
    m_videoDevice = nullptr;
    m_cachedConfigData.clear();
    m_encodedCallback = nullptr;
    m_nv12RingIndex = 0;
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

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outViewDesc = {};
    outViewDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    outViewDesc.Texture2D.MipSlice = 0;

    for (int i = 0; i < NV12_RING_SIZE; ++i) {
        hr = m_device->CreateTexture2D(&nv12Desc, nullptr, &m_nv12Textures[i]);
        if (FAILED(hr)) {
            nv12Desc.BindFlags = D3D11_BIND_RENDER_TARGET;
            hr = m_device->CreateTexture2D(&nv12Desc, nullptr, &m_nv12Textures[i]);
            if (FAILED(hr)) {
                Logger::E("WmfVideoEncoder", "Failed to create NV12 texture [" + std::to_string(i) + "], hr = 0x" + std::to_string(hr));
                return false;
            }
        }

        hr = m_videoDevice->CreateVideoProcessorOutputView(m_nv12Textures[i].Get(), m_videoProcessorEnum.Get(), &outViewDesc, &m_outputViews[i]);
        if (FAILED(hr)) {
            Logger::E("WmfVideoEncoder", "Failed to create VideoProcessorOutputView [" + std::to_string(i) + "], hr = 0x" + std::to_string(hr));
            return false;
        }
    }

    m_inputViews.clear();
    m_nv12RingIndex = 0;
    return true;
}

bool WmfVideoEncoder::ConvertBgraToNv12(ID3D11Texture2D* bgraTexture, ID3D11Texture2D** outNv12Texture) {
    if (!bgraTexture || !m_videoDevice || !m_videoContext || !m_videoProcessor || !outNv12Texture) return false;

    int ringIdx = (m_nv12RingIndex++) % NV12_RING_SIZE;
    if (!m_outputViews[ringIdx] || !m_nv12Textures[ringIdx]) return false;

    auto it = m_inputViews.find(bgraTexture);
    if (it == m_inputViews.end() || !it->second) {
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
        m_inputViews[bgraTexture] = inputView;
        it = m_inputViews.find(bgraTexture);
    }

    D3D11_VIDEO_PROCESSOR_STREAM stream = {};
    stream.Enable = TRUE;
    stream.pInputSurface = it->second.Get();

    HRESULT hr = m_videoContext->VideoProcessorBlt(m_videoProcessor.Get(), m_outputViews[ringIdx].Get(), 0, 1, &stream);
    if (FAILED(hr)) {
        static int s_err2 = 0;
        if (s_err2++ < 3) {
            Logger::E("WmfVideoEncoder", "VideoProcessorBlt failed, hr = 0x" + std::to_string(hr));
        }
        return false;
    }

    *outNv12Texture = m_nv12Textures[ringIdx].Get();
    return true;
}

std::vector<RateControlModeInfo> WmfVideoEncoder::QuerySupportedRateControlModes(
    IMFDXGIDeviceManager* dxgiManager,
    VideoCodecType codecType
) {
    UNREFERENCED_PARAMETER(dxgiManager);
    std::vector<RateControlModeInfo> result;
    std::set<uint32_t> foundModes;

    GUID targetSubtype = (codecType == VideoCodecType::H265_HEVC) ? MFVideoFormat_HEVC : MFVideoFormat_H264;
    MFT_REGISTER_TYPE_INFO outputTypeInfo = { MFMediaType_Video, targetSubtype };
    IMFActivate** ppActivate = nullptr;
    UINT32 count = 0;

    HRESULT hr = MFTEnumEx(
        MFT_CATEGORY_VIDEO_ENCODER,
        MFT_ENUM_FLAG_ALL | MFT_ENUM_FLAG_SORTANDFILTER,
        nullptr,
        &outputTypeInfo,
        &ppActivate,
        &count
    );

    if (SUCCEEDED(hr) && count > 0 && ppActivate) {
        for (UINT32 i = 0; i < count; ++i) {
            try {
                Microsoft::WRL::ComPtr<IMFTransform> encoder;
                if (SUCCEEDED(ppActivate[i]->ActivateObject(IID_PPV_ARGS(&encoder))) && encoder) {
                    Microsoft::WRL::ComPtr<ICodecAPI> codecApi;
                    if (SUCCEEDED(encoder->QueryInterface(IID_PPV_ARGS(&codecApi))) && codecApi) {
                        if (codecApi->IsSupported(&CODECAPI_AVEncCommonRateControlMode) == S_OK) {
                            // 1. Try GetParameterValues
                            VARIANT* pValues = nullptr;
                            ULONG numValues = 0;
                            if (SUCCEEDED(codecApi->GetParameterValues(&CODECAPI_AVEncCommonRateControlMode, &pValues, &numValues)) && pValues && numValues > 0) {
                                for (ULONG v = 0; v < numValues; ++v) {
                                    uint32_t val = (pValues[v].vt == VT_UI4) ? pValues[v].ulVal : (uint32_t)pValues[v].lVal;
                                    foundModes.insert(val);
                                    VariantClear(&pValues[v]);
                                }
                                CoTaskMemFree(pValues);
                            }

                            // 2. Try GetParameterRange
                            VARIANT vMin, vMax, vDelta;
                            VariantInit(&vMin); VariantInit(&vMax); VariantInit(&vDelta);
                            if (SUCCEEDED(codecApi->GetParameterRange(&CODECAPI_AVEncCommonRateControlMode, &vMin, &vMax, &vDelta))) {
                                ULONG minVal = (vMin.vt == VT_UI4) ? vMin.ulVal : (ULONG)vMin.lVal;
                                ULONG maxVal = (vMax.vt == VT_UI4) ? vMax.ulVal : (ULONG)vMax.lVal;
                                ULONG step = (vDelta.vt == VT_UI4) ? vDelta.ulVal : (ULONG)vDelta.lVal;
                                if (step == 0) step = 1;
                                for (ULONG val = minVal; val <= maxVal && val <= 6; val += step) {
                                    foundModes.insert(val);
                                }
                                VariantClear(&vMin); VariantClear(&vMax); VariantClear(&vDelta);
                            }
                        }
                    }
                    encoder = nullptr;
                }
            } catch (...) {}
            ppActivate[i]->Release();
        }
        CoTaskMemFree(ppActivate);
    }

    // Baseline streaming modes: ensure CBR, LowDelayVBR, UnconstrainedVBR, Quality CQF are always supported
    foundModes.insert(static_cast<uint32_t>(RateControlMode::CBR));
    foundModes.insert(static_cast<uint32_t>(RateControlMode::LowDelayVBR));
    foundModes.insert(static_cast<uint32_t>(RateControlMode::UnconstrainedVBR));
    foundModes.insert(static_cast<uint32_t>(RateControlMode::Quality));

    auto GetModeName = [](RateControlMode mode) -> std::wstring {
        switch (mode) {
            case RateControlMode::UnconstrainedVBR: return L"VBR (动态码率 - 推荐)";
            case RateControlMode::CBR: return L"CBR (恒定码率)";
            case RateControlMode::LowDelayVBR: return L"低延迟 VBR (Low-Delay)";
            case RateControlMode::PeakConstrainedVBR: return L"受限 VBR (Peak Constrained)";
            case RateControlMode::Quality: return L"恒定质量 CQF (Quality)";
            case RateControlMode::GlobalLowDelayVBR: return L"全局低延迟 VBR";
            case RateControlMode::GlobalVBR: return L"全局 VBR";
            default: return L"自定义模式";
        }
    };

    static const RateControlMode priorityOrder[] = {
        RateControlMode::UnconstrainedVBR,
        RateControlMode::CBR,
        RateControlMode::LowDelayVBR,
        RateControlMode::PeakConstrainedVBR,
        RateControlMode::Quality,
        RateControlMode::GlobalLowDelayVBR,
        RateControlMode::GlobalVBR
    };

    if (!foundModes.empty()) {
        for (auto m : priorityOrder) {
            if (foundModes.count(static_cast<uint32_t>(m))) {
                result.push_back({ m, GetModeName(m) });
            }
        }
    }

    if (result.empty()) {
        result.push_back({ RateControlMode::UnconstrainedVBR, GetModeName(RateControlMode::UnconstrainedVBR) });
        result.push_back({ RateControlMode::CBR, GetModeName(RateControlMode::CBR) });
        result.push_back({ RateControlMode::LowDelayVBR, GetModeName(RateControlMode::LowDelayVBR) });
        result.push_back({ RateControlMode::Quality, GetModeName(RateControlMode::Quality) });
    }

    std::string modeNames;
    for (size_t k = 0; k < result.size(); ++k) {
        char szBuf[128] = {};
        WideCharToMultiByte(CP_UTF8, 0, result[k].displayName.c_str(), -1, szBuf, sizeof(szBuf), nullptr, nullptr);
        if (k > 0) modeNames += ", ";
        modeNames += szBuf;
    }

    Logger::I("WmfVideoEncoder", "Hardware rate control modes scanned for " +
              std::string(codecType == VideoCodecType::H265_HEVC ? "HEVC" : "H.264") +
              ": [" + modeNames + "]");
    return result;
}

static bool TrySetupEncoder(
    IMFTransform* encoder,
    IMFDXGIDeviceManager* dxgiManager,
    ID3D11Texture2D* nv12Texture,
    GUID targetSubtype,
    int width,
    int height,
    int fps,
    int bitrateKbps,
    RateControlMode rateControlMode,
    Microsoft::WRL::ComPtr<ICodecAPI>& outCodecApi
) {
    if (!encoder) return false;

    Microsoft::WRL::ComPtr<IMFAttributes> attributes;
    bool isAsync = false;
    if (SUCCEEDED(encoder->GetAttributes(&attributes)) && attributes) {
        UINT32 asyncVal = 0;
        attributes->GetUINT32(MF_TRANSFORM_ASYNC, &asyncVal);
        isAsync = (asyncVal != 0);
        if (isAsync) {
            attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
        }
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

    // 3. Low-latency & Real-Time High-FPS tuning
    if (SUCCEEDED(encoder->QueryInterface(IID_PPV_ARGS(&outCodecApi))) && outCodecApi) {
        VARIANT val;
        VariantInit(&val);

        val.vt = VT_UI4;
        val.ulVal = static_cast<ULONG>(rateControlMode);
        HRESULT hrMode = outCodecApi->SetValue(&CODECAPI_AVEncCommonRateControlMode, &val);
        if (FAILED(hrMode)) {
            Logger::W("WmfVideoEncoder", "Failed to set RateControlMode = " + std::to_string((uint32_t)rateControlMode) + ", fallback to CBR");
            val.ulVal = eAVEncCommonRateControlMode_CBR;
            outCodecApi->SetValue(&CODECAPI_AVEncCommonRateControlMode, &val);
        }

        if (rateControlMode == RateControlMode::Quality) {
            val.vt = VT_UI4;
            val.ulVal = 75; // Quality factor (1..100)
            outCodecApi->SetValue(&CODECAPI_AVEncCommonQuality, &val);
        }

        val.vt = VT_BOOL;
        val.boolVal = VARIANT_TRUE;
        outCodecApi->SetValue(&CODECAPI_AVEncCommonLowLatency, &val);

        val.vt = VT_BOOL;
        val.boolVal = VARIANT_TRUE;
        outCodecApi->SetValue(&CODECAPI_AVEncCommonRealTime, &val);

        val.vt = VT_UI4;
        val.ulVal = 100; // 100 = QualityVsSpeed Fastest
        outCodecApi->SetValue(&CODECAPI_AVEncCommonQualityVsSpeed, &val);

        val.vt = VT_UI4;
        val.ulVal = fps > 0 ? fps : 60;
        outCodecApi->SetValue(&CODECAPI_AVEncMPVGOPSize, &val);

        val.vt = VT_UI4;
        val.ulVal = 0;
        outCodecApi->SetValue(&CODECAPI_AVEncMPVDefaultBPictureCount, &val);
    }

    encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    // 4. Verification: Test encode 1 frame to verify encoder accepts input and does not reject with MF_E_NOTACCEPTING
    if (nv12Texture) {
        Microsoft::WRL::ComPtr<IMFMediaBuffer> testBuffer;
        if (SUCCEEDED(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), nv12Texture, 0, FALSE, &testBuffer)) && testBuffer) {
            Microsoft::WRL::ComPtr<IMFSample> testSample;
            if (SUCCEEDED(MFCreateSample(&testSample)) && testSample) {
                testSample->AddBuffer(testBuffer.Get());
                testSample->SetSampleTime(0);
                testSample->SetSampleDuration(10000000LL / (fps > 0 ? fps : 60));

                hr = encoder->ProcessInput(0, testSample.Get(), 0);
                if (FAILED(hr)) {
                    Logger::W("WmfVideoEncoder", "Candidate rejected test ProcessInput, hr = 0x" + std::to_string(hr));
                    return false;
                }
                encoder->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
                encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
                encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
            }
        }
    }

    return true;
}

bool WmfVideoEncoder::Initialize(
    int width,
    int height,
    int fps,
    int bitrateKbps,
    VideoCodecType codecType,
    RateControlMode rateControlMode
) {
    Shutdown();

    m_width = (width / 2) * 2;
    m_height = (height / 2) * 2;
    m_fps = fps > 0 ? fps : 60;
    m_bitrateKbps = bitrateKbps > 0 ? bitrateKbps : 16000;
    m_codecType = codecType;
    m_rateControlMode = rateControlMode;

    Logger::I("WmfVideoEncoder", "Initializing encoder: " + std::to_string(m_width) + "x" + std::to_string(m_height) +
              " @" + std::to_string(m_fps) + "fps, " + std::to_string(m_bitrateKbps) + " Kbps, Requested=" +
              (m_codecType == VideoCodecType::H265_HEVC ? "HEVC" : "H.264") +
              ", RateControlMode=" + std::to_string((uint32_t)m_rateControlMode));

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
            MFT_ENUM_FLAG_ALL | MFT_ENUM_FLAG_SORTANDFILTER,
            nullptr,
            &outputTypeInfo,
            &ppActivate,
            &count
        );

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
                if (TrySetupEncoder(encoder.Get(), m_dxgiManager.Get(), m_nv12Textures[0].Get(), subtype, m_width, m_height, m_fps, m_bitrateKbps, m_rateControlMode, m_codecApi)) {
                    m_encoder = encoder;
                    Logger::I("WmfVideoEncoder", "Successfully activated verified encoder: " + std::string(szName));
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

bool WmfVideoEncoder::EncodeFrame(ID3D11Texture2D* bgraTexture, int64_t timestampNs) {
    if (!m_isInitialized || !bgraTexture) return false;

    std::lock_guard<std::mutex> lock(m_encoderMutex);
    if (!m_isInitialized || !m_encoder || !bgraTexture) return false;

    ID3D11Texture2D* targetNv12 = nullptr;
    if (!ConvertBgraToNv12(bgraTexture, &targetNv12) || !targetNv12) {
        return false;
    }

    bool forceThisFrame = m_forceKeyframe || (m_frameIndex <= 5) || (m_frameIndex % (m_fps > 0 ? m_fps : 60) == 0);
    if (forceThisFrame && m_codecApi) {
        VARIANT val;
        VariantInit(&val);
        val.vt = VT_UI4;
        val.ulVal = 1;
        m_codecApi->SetValue(&CODECAPI_AVEncVideoForceKeyFrame, &val);
        m_forceKeyframe = false;
    }

    Microsoft::WRL::ComPtr<IMFMediaBuffer> mediaBuffer;
    HRESULT hr = MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), targetNv12, 0, FALSE, &mediaBuffer);
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
    if (FAILED(hr)) {
        static int s_inErr = 0;
        if (s_inErr++ < 3) {
            Logger::W("WmfVideoEncoder", "ProcessInput failed, hr = 0x" + std::to_string(hr));
        }
    }

    DrainOutput(timestampNs / 1000000);
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
                bool isHevc = (m_codecType == VideoCodecType::H265_HEVC);
                UINT32 isCleanPoint = 0;
                pSample->GetUINT32(MFSampleExtension_CleanPoint, &isCleanPoint);
                bool hasKeyNal = false;
                bool hasSliceNal = false;

                // NAL unit scanning
                struct NalSlice {
                    size_t start;      // start of start code
                    size_t header;     // start of NAL header byte
                    size_t end;        // end of NAL
                    uint8_t nalType;
                    bool isParamSet;
                    bool isKeyframe;
                    bool isSlice;
                };

                std::vector<NalSlice> nals;
                size_t i = 0;
                int currentNalStart = -1;
                int currentNalHeader = -1;
                uint8_t currentNalType = 0;
                bool currentIsParam = false;
                bool currentIsKey = false;
                bool currentIsSlice = false;

                auto finishNal = [&](size_t endPos) {
                    if (currentNalStart >= 0 && endPos > static_cast<size_t>(currentNalStart)) {
                        nals.push_back({
                            static_cast<size_t>(currentNalStart),
                            static_cast<size_t>(currentNalHeader),
                            endPos,
                            currentNalType,
                            currentIsParam,
                            currentIsKey,
                            currentIsSlice
                        });
                    }
                };

                while (i + 2 < currentLen) {
                    int scLen = 0;
                    if (pData[i] == 0 && pData[i + 1] == 0) {
                        if (pData[i + 2] == 1) {
                            scLen = 3;
                        } else if (i + 3 < currentLen && pData[i + 2] == 0 && pData[i + 3] == 1) {
                            scLen = 4;
                        }
                    }

                    if (scLen > 0) {
                        finishNal(i);
                        currentNalStart = static_cast<int>(i);
                        currentNalHeader = static_cast<int>(i + scLen);
                        currentNalType = 0;
                        currentIsParam = false;
                        currentIsKey = false;
                        currentIsSlice = false;

                        if (currentNalHeader < static_cast<int>(currentLen)) {
                            if (!isHevc) {
                                currentNalType = pData[currentNalHeader] & 0x1F;
                                currentIsParam = (currentNalType == 7 || currentNalType == 8); // SPS (7), PPS (8)
                                currentIsKey = (currentNalType == 5); // IDR (5)
                                currentIsSlice = (currentNalType == 1 || currentNalType == 5);
                            } else {
                                currentNalType = (pData[currentNalHeader] >> 1) & 0x3F;
                                currentIsParam = (currentNalType == 32 || currentNalType == 33 || currentNalType == 34); // VPS (32), SPS (33), PPS (34)
                                currentIsKey = (currentNalType == 19 || currentNalType == 20 || currentNalType == 21); // IDR/CRA
                                currentIsSlice = (currentNalType >= 0 && currentNalType <= 21);
                            }
                            if (currentIsKey) hasKeyNal = true;
                            if (currentIsSlice) hasSliceNal = true;
                        }
                        i += scLen;
                    } else {
                        i++;
                    }
                }
                finishNal(currentLen);

                // Collect Parameter Sets if present
                std::vector<uint8_t> newParamSets;
                for (const auto& nal : nals) {
                    if (nal.isParamSet && nal.end > nal.start) {
                        newParamSets.insert(newParamSets.end(), pData + nal.start, pData + nal.end);
                    }
                }

                if (!newParamSets.empty()) {
                    m_cachedConfigData = std::move(newParamSets);
                }

                bool isKeyframe = hasKeyNal || (isCleanPoint != 0);

                if (m_encodedCallback) {
                    // 1. If this is a keyframe (or parameter sets are present):
                    // Send separate CodecConfig packet (csd-0/csd-1 for receiver MediaCodec initialization)
                    if (isKeyframe && !m_cachedConfigData.empty()) {
                        m_encodedCallback(
                            m_cachedConfigData.data(),
                            m_cachedConfigData.size(),
                            timestampMs,
                            false, /* isKeyframe */
                            true,  /* isCodecConfig */
                            isHevc
                        );
                    }

                    // 2. If the buffer has video slice data or is a full frame:
                    if (hasSliceNal || !nals.empty()) {
                        if (isKeyframe) {
                            static std::atomic<int> s_encodedFrames{ 0 };
                            int fNum = ++s_encodedFrames;
                            Logger::I("WmfVideoEncoder", "Encoded Keyframe #" + std::to_string(fNum) + " (" + std::to_string(currentLen) + " bytes, isHevc=" + std::to_string(isHevc) + ")");
                        }

                        // Send Keyframe / Delta frame
                        m_encodedCallback(
                            pData,
                            currentLen,
                            timestampMs,
                            isKeyframe,
                            false, /* isCodecConfig */
                            isHevc
                        );
                    } else if (newParamSets.empty() && m_cachedConfigData.empty()) {
                        // Fallback for raw packet without recognizable NAL
                        m_encodedCallback(
                            pData,
                            currentLen,
                            timestampMs,
                            isKeyframe,
                            false,
                            isHevc
                        );
                    }
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
