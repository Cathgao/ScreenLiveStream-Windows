#include "WgcCapture.h"
#include "Logger.h"
#include "Protocol.h"

#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <dwmapi.h>
#include <dxgi.h>

using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;

extern "C" {
    HRESULT __stdcall CreateDirect3D11DeviceFromDXGIDevice(
        ::IDXGIDevice* dxgiDevice,
        ::IInspectable** graphicsDevice
    );

    HRESULT __stdcall CreateDirect3D11SurfaceFromDXGISurface(
        ::IDXGISurface* dgxiSurface,
        ::IInspectable** graphicsSurface
    );
}

WgcCapture::WgcCapture(ID3D11Device* d3d11Device)
    : m_d3d11Device(d3d11Device) {
    if (m_d3d11Device) {
        m_d3d11Device->GetImmediateContext(&m_d3d11Context);

        Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
        if (SUCCEEDED(m_d3d11Device.As(&dxgiDevice))) {
            winrt::com_ptr<::IInspectable> inspectable;
            if (SUCCEEDED(CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.Get(), inspectable.put()))) {
                m_winrtDevice = inspectable.as<IDirect3DDevice>();
            }
        }
    }
}

WgcCapture::~WgcCapture() {
    StopCapture();
}

std::vector<CaptureTarget> WgcCapture::EnumerateMonitors() {
    std::vector<CaptureTarget> targets;

    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR hmon, HDC, LPRECT, LPARAM lParam) -> BOOL {
        auto* pTargets = reinterpret_cast<std::vector<CaptureTarget>*>(lParam);

        MONITORINFOEXW mi = {};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(hmon, &mi)) {
            CaptureTarget target;
            target.type = CaptureTarget::Type::Monitor;
            target.hmon = hmon;
            target.width = mi.rcMonitor.right - mi.rcMonitor.left;
            target.height = mi.rcMonitor.bottom - mi.rcMonitor.top;
            target.title = mi.szDevice;
            pTargets->push_back(target);
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&targets));

    return targets;
}

std::vector<CaptureTarget> WgcCapture::EnumerateWindows() {
    std::vector<CaptureTarget> targets;

    EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
        auto* pTargets = reinterpret_cast<std::vector<CaptureTarget>*>(lParam);

        if (!IsWindowVisible(hwnd)) return TRUE;
        if (IsIconic(hwnd)) return TRUE;

        // Skip windows without title
        int len = GetWindowTextLengthW(hwnd);
        if (len == 0) return TRUE;

        std::wstring title(len + 1, L'\0');
        GetWindowTextW(hwnd, title.data(), len + 1);
        title.resize(len);

        // Filter out Program Manager / Tooltips
        if (title == L"Program Manager" || title == L"Settings" || title == L"Windows Shell Experience Host") {
            return TRUE;
        }

        // Check if window is cloaked by DWM
        int cloaked = 0;
        DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
        if (cloaked) return TRUE;

        RECT rc;
        GetWindowRect(hwnd, &rc);
        int w = rc.right - rc.left;
        int h = rc.bottom - rc.top;
        if (w <= 100 || h <= 100) return TRUE;

        CaptureTarget target;
        target.type = CaptureTarget::Type::Window;
        target.hwnd = hwnd;
        target.title = title;
        target.width = w;
        target.height = h;
        pTargets->push_back(target);

        return TRUE;
    }, reinterpret_cast<LPARAM>(&targets));

    return targets;
}

bool WgcCapture::StartCapture(const CaptureTarget& target, bool captureCursor) {
    StopCapture();

    if (!m_winrtDevice || !m_d3d11Device) {
        Logger::E("WGC", "Cannot start capture: D3D11/WinRT device not initialized.");
        return false;
    }

    try {
        auto interop = winrt::get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();

        if (target.type == CaptureTarget::Type::Monitor && target.hmon) {
            winrt::check_hresult(interop->CreateForMonitor(
                target.hmon,
                winrt::guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>(),
                winrt::put_abi(m_item)
            ));
        } else if (target.type == CaptureTarget::Type::Window && target.hwnd) {
            winrt::check_hresult(interop->CreateForWindow(
                target.hwnd,
                winrt::guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>(),
                winrt::put_abi(m_item)
            ));
        } else {
            Logger::E("WGC", "Invalid capture target.");
            return false;
        }

        auto itemSize = m_item.Size();
        m_captureWidth = itemSize.Width;
        m_captureHeight = itemSize.Height;

        Logger::I("WGC", "Target resolution: " + std::to_string(m_captureWidth) + "x" + std::to_string(m_captureHeight));

        // Create Direct3D11CaptureFramePool with 4 buffers for high-framerate 120+ FPS capture
        m_framePool = Direct3D11CaptureFramePool::CreateFreeThreaded(
            m_winrtDevice,
            DirectXPixelFormat::B8G8R8A8UIntNormalized,
            4,
            itemSize
        );

        m_frameArrivedRevoker = m_framePool.FrameArrived(
            winrt::auto_revoke,
            { this, &WgcCapture::OnFrameArrived }
        );

        m_session = m_framePool.CreateCaptureSession(m_item);
        m_session.IsCursorCaptureEnabled(captureCursor);

        // Win11 22H2+ support: turn off yellow border if supported
        try {
            auto session3 = m_session.as<winrt::Windows::Graphics::Capture::IGraphicsCaptureSession3>();
            if (session3) {
                session3.IsBorderRequired(false);
            }
        } catch (...) {
            // Older Windows 10 versions do not have IGraphicsCaptureSession3
        }

        // Win11 23H2+ / high-framerate support: unlock 120+ FPS capture interval
        try {
            auto session5 = m_session.as<winrt::Windows::Graphics::Capture::IGraphicsCaptureSession5>();
            if (session5) {
                session5.MinUpdateInterval(winrt::Windows::Foundation::TimeSpan{ 0 });
                Logger::I("WGC", "Successfully unlocked high-framerate MinUpdateInterval=0");
            }
        } catch (...) {
            try {
                auto sessionAbi = reinterpret_cast<IUnknown*>(winrt::get_abi(m_session));
                Microsoft::WRL::ComPtr<ABI::Windows::Graphics::Capture::IGraphicsCaptureSession5> session5Abi;
                if (sessionAbi && SUCCEEDED(sessionAbi->QueryInterface(IID_PPV_ARGS(&session5Abi))) && session5Abi) {
                    ABI::Windows::Foundation::TimeSpan zeroTs = { 0 };
                    session5Abi->put_MinUpdateInterval(zeroTs);
                    Logger::I("WGC", "Successfully unlocked high-framerate via ABI MinUpdateInterval=0");
                }
            } catch (...) {}
        }

        m_session.StartCapture();
        m_isCapturing = true;
        Logger::I("WGC", "Capture started successfully.");
        return true;
    } catch (const winrt::hresult_error& ex) {
        Logger::E("WGC", "Failed to start capture: " + winrt::to_string(ex.message()));
        return false;
    } catch (const std::exception& e) {
        Logger::E("WGC", std::string("Failed to start capture: ") + e.what());
        return false;
    }
}

void WgcCapture::StopCapture() {
    if (!m_isCapturing.exchange(false)) return;

    try {
        if (m_frameArrivedRevoker) {
            m_frameArrivedRevoker.revoke();
        }
        if (m_session) {
            m_session.Close();
            m_session = nullptr;
        }
        if (m_framePool) {
            m_framePool.Close();
            m_framePool = nullptr;
        }
        m_item = nullptr;
    } catch (...) {}

    // Exclusive lock m_frameMutex to ensure any in-flight OnFrameArrived has completely finished
    {
        std::unique_lock<std::shared_mutex> lock(m_frameMutex);
        m_frameCallback = nullptr;
    }

    Logger::I("WGC", "Capture stopped.");
}

void WgcCapture::OnFrameArrived(Direct3D11CaptureFramePool const& sender, winrt::Windows::Foundation::IInspectable const&) {
    std::shared_lock<std::shared_mutex> lock(m_frameMutex);
    if (!m_isCapturing) return;

    try {
        while (auto frame = sender.TryGetNextFrame()) {
            if (!m_isCapturing) break;

            static std::atomic<int> s_wgcFrames{ 0 };
            int f = ++s_wgcFrames;
            if (f <= 3 || f % 240 == 0) {
                Logger::I("WGC", "Captured frame #" + std::to_string(f));
            }

            auto surface = frame.Surface();
            if (!surface) continue;
            auto access = surface.as<Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
            if (!access) continue;

            Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
            if (SUCCEEDED(access->GetInterface(__uuidof(ID3D11Texture2D), (void**)&texture)) && texture) {
                auto size = frame.ContentSize();
                if (size.Width != m_captureWidth || size.Height != m_captureHeight) {
                    m_captureWidth = size.Width;
                    m_captureHeight = size.Height;
                    sender.Recreate(m_winrtDevice, DirectXPixelFormat::B8G8R8A8UIntNormalized, 4, size);
                }

                int64_t timestampNs = Protocol::GetCurrentNanos();
                if (m_frameCallback && m_isCapturing) {
                    m_frameCallback(texture.Get(), timestampNs, m_captureWidth, m_captureHeight);
                }
            }
        }
    } catch (const winrt::hresult_error& ex) {
        // Expected when session or frame pool closes
        Logger::I("WGC", "WinRT frame arrival closed/ignored: " + winrt::to_string(ex.message()));
    } catch (const std::exception& e) {
        Logger::E("WGC", "Exception in OnFrameArrived: " + std::string(e.what()));
    } catch (...) {
        Logger::E("WGC", "Unknown exception in OnFrameArrived");
    }
}

