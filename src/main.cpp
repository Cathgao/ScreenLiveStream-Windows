#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winrt/Windows.Foundation.h>
#include "MainWindow.h"
#include "Logger.h"

int WINAPI wWinMain(
    _In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPWSTR lpCmdLine,
    _In_ int nShowCmd
) {
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);

    // Initialize File Logger
    Logger::Init("questcast.log");
    Logger::I("Main", "=== ScreenLiveStream Windows Starting ===");

    // Initialize WinRT and COM for the main UI thread
    winrt::init_apartment(winrt::apartment_type::single_threaded);

    // Initialize Winsock
    WSADATA wsaData;
    int res = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (res != 0) {
        Logger::E("Main", "WSAStartup failed");
        MessageBoxW(nullptr, L"WSAStartup failed", L"Error", MB_OK | MB_ICONERROR);
        winrt::uninit_apartment();
        return 1;
    }

    Logger::I("Main", "Winsock initialized successfully.");

    int ret = 0;
    try {
        MainWindow mainWin;
        if (!mainWin.Create(hInstance, nShowCmd)) {
            Logger::E("Main", "Failed to create main window");
            MessageBoxW(nullptr, L"Failed to create main window", L"Error", MB_OK | MB_ICONERROR);
            WSACleanup();
            winrt::uninit_apartment();
            return 1;
        }

        ret = mainWin.Run();
    } catch (const winrt::hresult_error& ex) {
        std::wstring msg = L"WinRT Exception: " + std::wstring(ex.message());
        Logger::E("Main", "Fatal WinRT exception: " + winrt::to_string(ex.message()));
        MessageBoxW(nullptr, msg.c_str(), L"Fatal Error", MB_OK | MB_ICONERROR);
    } catch (const std::exception& e) {
        std::string msg = std::string("Standard Exception: ") + e.what();
        Logger::E("Main", msg);
        MessageBoxA(nullptr, msg.c_str(), "Fatal Error", MB_OK | MB_ICONERROR);
    } catch (...) {
        Logger::E("Main", "Unknown fatal exception caught.");
        MessageBoxW(nullptr, L"Unknown fatal error occurred.", L"Fatal Error", MB_OK | MB_ICONERROR);
    }

    Logger::I("Main", "Application exiting cleanly.");
    WSACleanup();
    winrt::uninit_apartment();
    return ret;
}
