#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <atomic>
#include <shared_mutex>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

struct CaptureTarget {
    enum class Type { Monitor, Window };
    Type type;
    HWND hwnd = nullptr;
    HMONITOR hmon = nullptr;
    std::wstring title;
    int width = 0;
    int height = 0;
};

class WgcCapture {
public:
    using FrameCallback = std::function<void(ID3D11Texture2D* texture, int64_t timestampNs, int width, int height)>;

    WgcCapture(ID3D11Device* d3d11Device);
    ~WgcCapture();

    static std::vector<CaptureTarget> EnumerateMonitors();
    static std::vector<CaptureTarget> EnumerateWindows();

    bool StartCapture(const CaptureTarget& target, bool captureCursor = true);
    void StopCapture();

    bool IsCapturing() const { return m_isCapturing; }
    void SetFrameCallback(FrameCallback cb) { m_frameCallback = cb; }

    int GetCaptureWidth() const { return m_captureWidth; }
    int GetCaptureHeight() const { return m_captureHeight; }

private:
    Microsoft::WRL::ComPtr<ID3D11Device> m_d3d11Device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_d3d11Context;
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice m_winrtDevice{ nullptr };

    winrt::Windows::Graphics::Capture::GraphicsCaptureItem m_item{ nullptr };
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool m_framePool{ nullptr };
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession m_session{ nullptr };
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool::FrameArrived_revoker m_frameArrivedRevoker;

    std::atomic<bool> m_isCapturing{ false };
    int m_captureWidth = 0;
    int m_captureHeight = 0;
    FrameCallback m_frameCallback;
    std::shared_mutex m_frameMutex;

    void OnFrameArrived(winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool const& sender, winrt::Windows::Foundation::IInspectable const& args);
};
