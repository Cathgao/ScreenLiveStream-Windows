#include "D3D11Helper.h"
#include "Logger.h"

namespace D3D11Helper {

bool CreateDevice(DeviceResources& outResources) {
    UINT creationFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
#ifdef _DEBUG
    // creationFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0
    };
    D3D_FEATURE_LEVEL featureLevel;

    HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        creationFlags,
        featureLevels,
        ARRAYSIZE(featureLevels),
        D3D11_SDK_VERSION,
        &outResources.device,
        &featureLevel,
        &outResources.context
    );

    if (FAILED(hr)) {
        Logger::E("D3D11", "Failed to create D3D11 Device with hardware driver, hr = " + std::to_string(hr));
        return false;
    }

    // Enable multithread protection on D3D11 device context
    hr = outResources.context.As(&outResources.multithread);
    if (SUCCEEDED(hr) && outResources.multithread) {
        outResources.multithread->SetMultithreadProtected(TRUE);
    }

    // Create WMF DXGI Device Manager
    hr = MFCreateDXGIDeviceManager(&outResources.resetToken, &outResources.dxgiManager);
    if (SUCCEEDED(hr) && outResources.dxgiManager) {
        hr = outResources.dxgiManager->ResetDevice(static_cast<IUnknown*>(outResources.device.Get()), outResources.resetToken);
        if (FAILED(hr)) {
            Logger::W("D3D11", "Failed to reset DXGIDeviceManager with D3D11 Device");
        }
    } else {
        Logger::W("D3D11", "Failed to create MFCreateDXGIDeviceManager");
    }

    Logger::I("D3D11", "D3D11 Device created successfully with Feature Level: " + std::to_string(featureLevel));
    return true;
}

ComPtr<ID3D11Texture2D> CreateTexture(
    ID3D11Device* device,
    UINT width,
    UINT height,
    DXGI_FORMAT format,
    UINT bindFlags,
    D3D11_USAGE usage
) {
    if (!device || width == 0 || height == 0) return nullptr;

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Usage = usage;
    desc.BindFlags = bindFlags;
    desc.CPUAccessFlags = (usage == D3D11_USAGE_DYNAMIC || usage == D3D11_USAGE_STAGING) ? D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE : 0;
    desc.MiscFlags = 0;

    ComPtr<ID3D11Texture2D> texture;
    HRESULT hr = device->CreateTexture2D(&desc, nullptr, &texture);
    if (FAILED(hr)) {
        Logger::E("D3D11", "Failed to create Texture2D, hr = " + std::to_string(hr));
        return nullptr;
    }
    return texture;
}

} // namespace D3D11Helper
