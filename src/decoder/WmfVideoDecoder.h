#pragma once
#include <winsock2.h>
#include <windows.h>
#include <d3d11.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wrl/client.h>
#include <vector>
#include <functional>
#include <cstdint>
#include "../encoder/WmfVideoEncoder.h"

class WmfVideoDecoder {
public:
    using FrameDecodedCallback = std::function<void(ID3D11Texture2D* texture, int64_t timestampMs, int width, int height)>;

    WmfVideoDecoder(ID3D11Device* d3d11Device, IMFDXGIDeviceManager* dxgiManager);
    ~WmfVideoDecoder();

    bool Initialize(VideoCodecType codecType = VideoCodecType::H265_HEVC);
    void Shutdown();

    bool DecodeNalu(const uint8_t* data, size_t size, int64_t timestampMs);
    void SetDecodedCallback(FrameDecodedCallback cb) { m_decodedCallback = cb; }

    bool IsInitialized() const { return m_isInitialized; }
    VideoCodecType GetCodecType() const { return m_codecType; }

private:
    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
    Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> m_dxgiManager;

    Microsoft::WRL::ComPtr<IMFTransform> m_decoder;
    VideoCodecType m_codecType = VideoCodecType::H265_HEVC;
    bool m_isInitialized = false;
    int m_videoWidth = 0;
    int m_videoHeight = 0;

    FrameDecodedCallback m_decodedCallback;

    void DrainOutput(int64_t timestampMs);
    bool ConfigureOutputType();
};
