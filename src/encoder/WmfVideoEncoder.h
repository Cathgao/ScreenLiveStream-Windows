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

    WmfVideoEncoder(ID3D11Device* d3d11Device, IMFDXGIDeviceManager* dxgiManager);
    ~WmfVideoEncoder();

    bool Initialize(
        int width,
        int height,
        int fps,
        int bitrateKbps,
        VideoCodecType codecType = VideoCodecType::H265_HEVC
    );

    void Shutdown();

    bool EncodeFrame(ID3D11Texture2D* bgraTexture, int64_t timestampNs);

    void RequestKeyFrame();
    void SetEncodedCallback(EncodedCallback cb) { m_encodedCallback = cb; }

    bool IsInitialized() const { return m_isInitialized; }
    VideoCodecType GetCodecType() const { return m_codecType; }

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
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_nv12Texture;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> m_inputView;
    ID3D11Texture2D* m_lastBgraTexture = nullptr;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> m_outputView;

    int m_width = 0;
    int m_height = 0;
    int m_fps = 60;
    int m_bitrateKbps = 16000;
    VideoCodecType m_codecType = VideoCodecType::H265_HEVC;
    bool m_isInitialized = false;
    bool m_forceKeyframe = true;
    uint32_t m_frameIndex = 0;

    EncodedCallback m_encodedCallback;
    std::vector<uint8_t> m_cachedConfigData;
    std::mutex m_drainMutex;

    Microsoft::WRL::ComPtr<IMFMediaEventGenerator> m_eventGenerator;
    std::thread m_eventThread;
    std::atomic<bool> m_isEventThreadRunning{ false };
    void EventThreadProc();

    bool InitColorConverter();
    bool ConvertBgraToNv12(ID3D11Texture2D* bgraTexture);
    void DrainOutput(int64_t timestampMs);
};
