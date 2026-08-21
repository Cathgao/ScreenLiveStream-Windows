#pragma once
#include <winsock2.h>
#include <windows.h>
#include <d3d11.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <strmif.h>
#include <codecapi.h>
#include <wrl/client.h>
#include <vector>
#include <set>
#include <unordered_map>
#include <functional>
#include <cstdint>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>

enum class VideoCodecType {
    H264,
    H265_HEVC
};

enum class RateControlMode : uint32_t {
    CBR = 0,                // eAVEncCommonRateControlMode_CBR (恒定码率)
    PeakConstrainedVBR = 1, // eAVEncCommonRateControlMode_PeakConstrainedVBR (受限 VBR)
    UnconstrainedVBR = 2,   // eAVEncCommonRateControlMode_UnconstrainedVBR (动态码率 VBR)
    Quality = 3,            // eAVEncCommonRateControlMode_Quality (恒定质量 CQF)
    LowDelayVBR = 4,        // eAVEncCommonRateControlMode_LowDelayVBR (低延迟 VBR)
    GlobalVBR = 5,          // eAVEncCommonRateControlMode_GlobalVBR (全局 VBR)
    GlobalLowDelayVBR = 6   // eAVEncCommonRateControlMode_GlobalLowDelayVBR (全局低延迟 VBR)
};

struct RateControlModeInfo {
    RateControlMode mode;
    std::wstring displayName;
};

class WmfVideoEncoder {
public:
    using EncodedCallback = std::function<void(
        const uint8_t* data,
        size_t size,
        int64_t timestampMs,
        bool isKeyframe,
        bool isCodecConfig,
        bool isHevc
    )>;

    static std::vector<RateControlModeInfo> QuerySupportedRateControlModes(
        IMFDXGIDeviceManager* dxgiManager,
        VideoCodecType codecType
    );

    WmfVideoEncoder(ID3D11Device* d3d11Device, IMFDXGIDeviceManager* dxgiManager);
    ~WmfVideoEncoder();

    bool Initialize(
        int width,
        int height,
        int fps,
        int bitrateKbps,
        VideoCodecType codecType = VideoCodecType::H265_HEVC,
        RateControlMode rateControlMode = RateControlMode::UnconstrainedVBR
    );

    void Shutdown();

    bool EncodeFrame(ID3D11Texture2D* bgraTexture, int64_t timestampNs);

    void RequestKeyFrame();
    void SetEncodedCallback(EncodedCallback cb) { m_encodedCallback = cb; }

    bool IsInitialized() const { return m_isInitialized; }
    VideoCodecType GetCodecType() const { return m_codecType; }
    RateControlMode GetRateControlMode() const { return m_rateControlMode; }

private:
    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
    Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> m_dxgiManager;

    Microsoft::WRL::ComPtr<IMFTransform> m_encoder;
    Microsoft::WRL::ComPtr<ICodecAPI> m_codecApi;

    Microsoft::WRL::ComPtr<ID3D11VideoDevice> m_videoDevice;
    Microsoft::WRL::ComPtr<ID3D11VideoContext> m_videoContext;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor> m_videoProcessor;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> m_videoProcessorEnum;
    static constexpr int NV12_RING_SIZE = 4;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_nv12Textures[NV12_RING_SIZE];
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> m_outputViews[NV12_RING_SIZE];
    std::unordered_map<ID3D11Texture2D*, Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView>> m_inputViews;
    uint32_t m_nv12RingIndex = 0;

    int m_width = 0;
    int m_height = 0;
    int m_fps = 60;
    int m_bitrateKbps = 16000;
    VideoCodecType m_codecType = VideoCodecType::H265_HEVC;
    RateControlMode m_rateControlMode = RateControlMode::UnconstrainedVBR;
    std::atomic<bool> m_isInitialized{ false };
    bool m_forceKeyframe = true;
    uint32_t m_frameIndex = 0;

    EncodedCallback m_encodedCallback;
    std::vector<uint8_t> m_cachedConfigData;
    std::mutex m_encoderMutex;

    bool InitColorConverter();
    bool ConvertBgraToNv12(ID3D11Texture2D* bgraTexture, ID3D11Texture2D** outNv12Texture);
    void DrainOutput(int64_t timestampMs);
};
