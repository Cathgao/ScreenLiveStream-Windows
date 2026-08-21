#pragma once
#include <winsock2.h>
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mfidl.h>
#include <wrl/client.h>
#include <string>

namespace D3D11Helper {

using Microsoft::WRL::ComPtr;

struct DeviceResources {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Multithread> multithread;
    ComPtr<IMFDXGIDeviceManager> dxgiManager;
    UINT resetToken = 0;
};

bool CreateDevice(DeviceResources& outResources);

ComPtr<ID3D11Texture2D> CreateTexture(
    ID3D11Device* device,
    UINT width,
    UINT height,
    DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM,
    UINT bindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE,
    D3D11_USAGE usage = D3D11_USAGE_DEFAULT
);

} // namespace D3D11Helper
