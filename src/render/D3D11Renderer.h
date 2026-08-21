#pragma once
#include <d3d11.h>
#include <dxgi1_5.h>
#include <wrl/client.h>
#include <windows.h>
#include <string>
#include <atomic>
#include <mutex>
#include <chrono>
#include <unordered_map>

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
    std::unordered_map<ID3D11Texture2D*, Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView>> m_inputViewCache;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> m_outputView;

    HWND m_hwnd = nullptr;
    int m_windowWidth = 0;
    int m_windowHeight = 0;
    int m_lastVideoWidth = 0;
    int m_lastVideoHeight = 0;
    bool m_isInitialized = false;
    bool m_allowTearing = false;

    std::mutex m_renderMutex;

    // 1-second Periodic Stats Tracking
    std::chrono::steady_clock::time_point m_lastStatsTime;
    uint32_t m_statsFramesRendered = 0;
    double m_statsTotalRenderMs = 0.0;
    double m_statsMaxRenderMs = 0.0;

    bool CreateSwapChain(int width, int height);
    bool SetupVideoProcessor(int inWidth, int inHeight, DXGI_FORMAT inFormat);
    void LogPeriodicStats();
};
