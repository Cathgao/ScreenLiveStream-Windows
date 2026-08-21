#pragma once
#include <d3d11.h>
#include <dxgi1_5.h>
#include <wrl/client.h>
#include <windows.h>
#include <string>
#include <atomic>
#include <mutex>

class D3D11Renderer {
public:
    D3D11Renderer(ID3D11Device* device, ID3D11DeviceContext* context);
    ~D3D11Renderer();

    bool Initialize(HWND hwnd, int width, int height);
    void Shutdown();
    void Resize(int width, int height);

    void RenderFrame(ID3D11Texture2D* frameTexture, int videoWidth, int videoHeight, const std::wstring& statsText = L"");

private:
    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> m_swapChain;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_renderTargetView;

    // Video processor for NV12 / BGRA to SwapChain BackBuffer
    Microsoft::WRL::ComPtr<ID3D11VideoDevice> m_videoDevice;
    Microsoft::WRL::ComPtr<ID3D11VideoContext> m_videoContext;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor> m_videoProcessor;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> m_videoProcessorEnum;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> m_inputView;
    ID3D11Texture2D* m_lastFrameTexture = nullptr;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> m_outputView;

    HWND m_hwnd = nullptr;
    int m_windowWidth = 0;
    int m_windowHeight = 0;
    int m_lastVideoWidth = 0;
    int m_lastVideoHeight = 0;
    bool m_isInitialized = false;

    std::mutex m_renderMutex;

    bool CreateSwapChain(int width, int height);
    bool SetupVideoProcessor(int inWidth, int inHeight, DXGI_FORMAT inFormat);
};
