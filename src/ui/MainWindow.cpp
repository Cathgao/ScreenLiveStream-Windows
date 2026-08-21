#include "MainWindow.h"
#include "Logger.h"
#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <sstream>
#include <iomanip>
#include <algorithm>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "uxtheme.lib")

#define ID_TIMER_STATS 1001
#define ID_BTN_MODE_SENDER 2001
#define ID_BTN_MODE_RECEIVER 2002
#define ID_COMBO_TARGET 2003
#define ID_BTN_REFRESH_TARGETS 2004
#define ID_COMBO_DEVICES 2005
#define ID_EDIT_IP 2006
#define ID_EDIT_PORT 2007
#define ID_COMBO_CODEC 2008
#define ID_COMBO_BITRATE 2009
#define ID_COMBO_FPS 2010
#define ID_COMBO_PROTOCOL 2011
#define ID_CHK_CURSOR 2012
#define ID_CHK_AUDIO 2013
#define ID_BTN_ACTION 2014
#define ID_BTN_REFRESH_DEVICES 2015

#define WM_USER_DEVICES_UPDATED (WM_USER + 1)
#define WM_USER_ADAPT_WINDOW (WM_USER + 2)

static std::wstring Utf8ToWide(const std::string& str) {
    if (str.empty()) return L"";
    int sizeNeeded = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), nullptr, 0);
    std::wstring wstr(sizeNeeded, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), &wstr[0], sizeNeeded);
    return wstr;
}

static void AddRoundedRectToPath(Gdiplus::GraphicsPath& path, Gdiplus::RectF rect, float radius) {
    float diameter = radius * 2.0f;
    if (diameter > rect.Width) diameter = rect.Width;
    if (diameter > rect.Height) diameter = rect.Height;
    path.AddArc(rect.X, rect.Y, diameter, diameter, 180.0f, 90.0f);
    path.AddArc(rect.GetRight() - diameter, rect.Y, diameter, diameter, 270.0f, 90.0f);
    path.AddArc(rect.GetRight() - diameter, rect.GetBottom() - diameter, diameter, diameter, 0.0f, 90.0f);
    path.AddArc(rect.X, rect.GetBottom() - diameter, diameter, diameter, 90.0f, 90.0f);
    path.CloseFigure();
}

MainWindow::MainWindow() {
    MFStartup(MF_VERSION);
    D3D11Helper::CreateDevice(m_d3dResources);

    Gdiplus::GdiplusStartupInput gdiplusStartupInput;
    Gdiplus::GdiplusStartup(&m_gdiplusToken, &gdiplusStartupInput, nullptr);

    InitThemeResources();
    m_lanDiscovery = std::make_unique<LanDiscovery>();
}

MainWindow::~MainWindow() {
    StopSender();
    StopReceiver();
    m_lanDiscovery = nullptr;

    CleanupThemeResources();
    if (m_gdiplusToken) {
        Gdiplus::GdiplusShutdown(m_gdiplusToken);
        m_gdiplusToken = 0;
    }

    MFShutdown();
}

void MainWindow::InitThemeResources() {
    m_hFontNormal = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");

    m_hBrushBg = CreateSolidBrush(RGB(18, 18, 20));       // #121214
    m_hBrushCard = CreateSolidBrush(RGB(30, 30, 36));     // #1E1E24
    m_hBrushInput = CreateSolidBrush(RGB(24, 24, 29));    // #18181D
}

void MainWindow::CleanupThemeResources() {
    if (m_hFontNormal) { DeleteObject(m_hFontNormal); m_hFontNormal = nullptr; }

    if (m_hBrushBg) { DeleteObject(m_hBrushBg); m_hBrushBg = nullptr; }
    if (m_hBrushCard) { DeleteObject(m_hBrushCard); m_hBrushCard = nullptr; }
    if (m_hBrushInput) { DeleteObject(m_hBrushInput); m_hBrushInput = nullptr; }
}

bool MainWindow::Create(HINSTANCE hInstance, int nCmdShow) {
    m_hInstance = hInstance;

    INITCOMMONCONTROLSEX icex = {};
    icex.dwSize = sizeof(icex);
    icex.dwICC = ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES;
    InitCommonControlsEx(&icex);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = MainWindow::WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"ScreenLiveStreamMainWindow";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = m_hBrushBg;
    RegisterClassExW(&wc);

    WNDCLASSEXW wcr = {};
    wcr.cbSize = sizeof(wcr);
    wcr.lpfnWndProc = MainWindow::ReceiverWndProc;
    wcr.hInstance = hInstance;
    wcr.lpszClassName = L"ScreenLiveStreamReceiverView";
    wcr.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcr.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassExW(&wcr);

    int winW = 556;
    int winH = 615;
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);

    m_hwnd = CreateWindowExW(
        0,
        L"ScreenLiveStreamMainWindow",
        L"ScreenLiveStream Windows",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN,
        (screenW - winW) / 2,
        (screenH - winH) / 2,
        winW,
        winH,
        nullptr,
        nullptr,
        hInstance,
        this
    );

    if (!m_hwnd) return false;

    // Enable Windows 11 / 10 Dark Mode Titlebar
    BOOL darkMode = TRUE;
    DwmSetWindowAttribute(m_hwnd, 20, &darkMode, sizeof(darkMode)); // DWMWA_USE_IMMERSIVE_DARK_MODE

    InitControls();
    RefreshCaptureTargets();

    m_lanDiscovery->SetOnDevicesUpdated([this](const std::vector<DiscoveredDevice>& devices) {
        {
            std::lock_guard<std::mutex> lock(m_devicesMutex);
            m_cachedDevices = devices;
        }
        if (m_hwnd) {
            PostMessage(m_hwnd, WM_USER_DEVICES_UPDATED, 0, 0);
        }
    });
    m_lanDiscovery->StartScanning();

    ShowWindow(m_hwnd, nCmdShow);
    UpdateWindow(m_hwnd);

    SetTimer(m_hwnd, ID_TIMER_STATS, 1000, nullptr);
    return true;
}

void MainWindow::InitControls() {
    auto CreateCardLabel = [&](const wchar_t* text, int x, int y, int w, int h) {
        HWND lbl = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT, x, y, w, h, m_hwnd, nullptr, m_hInstance, nullptr);
        SendMessage(lbl, WM_SETFONT, (WPARAM)m_hFontNormal, TRUE);
        return lbl;
    };

    // Role Switcher Buttons (Owner-draw, Y: 18)
    m_btnModeSender = CreateWindowExW(0, L"BUTTON", L"电脑发送端 (投给手机/Quest)", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 24, 18, 248, 34, m_hwnd, (HMENU)ID_BTN_MODE_SENDER, m_hInstance, nullptr);
    m_btnModeReceiver = CreateWindowExW(0, L"BUTTON", L"电脑接收端 (接收手机/Quest)", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 276, 18, 248, 34, m_hwnd, (HMENU)ID_BTN_MODE_RECEIVER, m_hInstance, nullptr);

    // Card 1: 采集与目标设备
    m_lblTarget = CreateCardLabel(L"采集目标:", 34, 100, 75, 22);
    m_comboTarget = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL, 112, 96, 332, 240, m_hwnd, (HMENU)ID_COMBO_TARGET, m_hInstance, nullptr);
    SendMessage(m_comboTarget, WM_SETFONT, (WPARAM)m_hFontNormal, TRUE);
    SetWindowTheme(m_comboTarget, L"DarkMode_Explorer", nullptr);

    m_btnRefreshTargets = CreateWindowExW(0, L"BUTTON", L"刷新", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 450, 95, 70, 28, m_hwnd, (HMENU)ID_BTN_REFRESH_TARGETS, m_hInstance, nullptr);

    m_lblDevices = CreateCardLabel(L"局域网设备:", 34, 136, 75, 22);
    m_comboDevices = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL, 112, 132, 332, 240, m_hwnd, (HMENU)ID_COMBO_DEVICES, m_hInstance, nullptr);
    SendMessage(m_comboDevices, WM_SETFONT, (WPARAM)m_hFontNormal, TRUE);
    SetWindowTheme(m_comboDevices, L"DarkMode_Explorer", nullptr);

    m_btnRefreshDevices = CreateWindowExW(0, L"BUTTON", L"搜索", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 450, 131, 70, 28, m_hwnd, (HMENU)ID_BTN_REFRESH_DEVICES, m_hInstance, nullptr);

    m_lblIp = CreateCardLabel(L"目标地址:", 34, 172, 75, 22);
    m_editIp = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"192.168.1.100", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 112, 169, 260, 25, m_hwnd, (HMENU)ID_EDIT_IP, m_hInstance, nullptr);
    m_lblPort = CreateCardLabel(L"端口:", 380, 172, 40, 22);
    m_editPort = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"8888", WS_CHILD | WS_VISIBLE | ES_NUMBER, 420, 169, 100, 25, m_hwnd, (HMENU)ID_EDIT_PORT, m_hInstance, nullptr);
    SendMessage(m_editIp, WM_SETFONT, (WPARAM)m_hFontNormal, TRUE);
    SendMessage(m_editPort, WM_SETFONT, (WPARAM)m_hFontNormal, TRUE);
    SetWindowTheme(m_editIp, L"DarkMode_Explorer", nullptr);
    SetWindowTheme(m_editPort, L"DarkMode_Explorer", nullptr);

    // Card 2: 编码与网络参数
    m_lblCodec = CreateCardLabel(L"视频编码:", 34, 254, 75, 22);
    m_comboCodec = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 112, 250, 140, 140, m_hwnd, (HMENU)ID_COMBO_CODEC, m_hInstance, nullptr);
    SendMessage(m_comboCodec, CB_ADDSTRING, 0, (LPARAM)L"H.265 / HEVC");
    SendMessage(m_comboCodec, CB_ADDSTRING, 0, (LPARAM)L"H.264 / AVC");
    SendMessage(m_comboCodec, CB_SETCURSEL, 0, 0);
    SendMessage(m_comboCodec, WM_SETFONT, (WPARAM)m_hFontNormal, TRUE);
    SetWindowTheme(m_comboCodec, L"DarkMode_Explorer", nullptr);

    m_lblBitrate = CreateCardLabel(L"目标码率:", 274, 254, 75, 22);
    m_comboBitrate = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 354, 250, 166, 160, m_hwnd, (HMENU)ID_COMBO_BITRATE, m_hInstance, nullptr);
    SendMessage(m_comboBitrate, CB_ADDSTRING, 0, (LPARAM)L"8 Mbps (流畅)");
    SendMessage(m_comboBitrate, CB_ADDSTRING, 0, (LPARAM)L"16 Mbps (高清)");
    SendMessage(m_comboBitrate, CB_ADDSTRING, 0, (LPARAM)L"25 Mbps (超清)");
    SendMessage(m_comboBitrate, CB_ADDSTRING, 0, (LPARAM)L"40 Mbps (极清)");
    SendMessage(m_comboBitrate, CB_SETCURSEL, 1, 0);
    SendMessage(m_comboBitrate, WM_SETFONT, (WPARAM)m_hFontNormal, TRUE);
    SetWindowTheme(m_comboBitrate, L"DarkMode_Explorer", nullptr);

    m_lblFps = CreateCardLabel(L"目标帧率:", 34, 288, 75, 22);
    m_comboFps = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 112, 284, 140, 140, m_hwnd, (HMENU)ID_COMBO_FPS, m_hInstance, nullptr);
    SendMessage(m_comboFps, CB_ADDSTRING, 0, (LPARAM)L"60 FPS");
    SendMessage(m_comboFps, CB_ADDSTRING, 0, (LPARAM)L"90 FPS");
    SendMessage(m_comboFps, CB_ADDSTRING, 0, (LPARAM)L"120 FPS");
    SendMessage(m_comboFps, CB_ADDSTRING, 0, (LPARAM)L"144 FPS");
    SendMessage(m_comboFps, CB_SETCURSEL, 0, 0);
    SendMessage(m_comboFps, WM_SETFONT, (WPARAM)m_hFontNormal, TRUE);
    SetWindowTheme(m_comboFps, L"DarkMode_Explorer", nullptr);

    m_lblProtocol = CreateCardLabel(L"传输协议:", 274, 288, 75, 22);
    m_comboProtocol = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 354, 284, 166, 140, m_hwnd, (HMENU)ID_COMBO_PROTOCOL, m_hInstance, nullptr);
    SendMessage(m_comboProtocol, CB_ADDSTRING, 0, (LPARAM)L"UDP (低延迟推荐)");
    SendMessage(m_comboProtocol, CB_ADDSTRING, 0, (LPARAM)L"TCP (稳定投屏)");
    SendMessage(m_comboProtocol, CB_SETCURSEL, 0, 0);
    SendMessage(m_comboProtocol, WM_SETFONT, (WPARAM)m_hFontNormal, TRUE);
    SetWindowTheme(m_comboProtocol, L"DarkMode_Explorer", nullptr);

    // Toggles
    m_chkCursor = CreateWindowExW(0, L"BUTTON", L"捕获鼠标光标", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 34, 320, 210, 24, m_hwnd, (HMENU)ID_CHK_CURSOR, m_hInstance, nullptr);
    m_chkAudio = CreateWindowExW(0, L"BUTTON", L"捕获系统声音 (WASAPI)", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 274, 320, 246, 24, m_hwnd, (HMENU)ID_CHK_AUDIO, m_hInstance, nullptr);
    SendMessage(m_chkCursor, BM_SETCHECK, BST_CHECKED, 0);
    SendMessage(m_chkAudio, BM_SETCHECK, BST_CHECKED, 0);
    SendMessage(m_chkCursor, WM_SETFONT, (WPARAM)m_hFontNormal, TRUE);
    SendMessage(m_chkAudio, WM_SETFONT, (WPARAM)m_hFontNormal, TRUE);

    // Action Hero Button (Owner-draw)
    m_btnAction = CreateWindowExW(0, L"BUTTON", L"启动画面投屏", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 20, 504, 504, 46, m_hwnd, (HMENU)ID_BTN_ACTION, m_hInstance, nullptr);
}

void MainWindow::RefreshCaptureTargets() {
    SendMessage(m_comboTarget, CB_RESETCONTENT, 0, 0);
    m_captureTargets.clear();

    auto monitors = WgcCapture::EnumerateMonitors();
    for (size_t i = 0; i < monitors.size(); ++i) {
        std::wstring name = L"[显示器] " + monitors[i].title + L" (" + std::to_wstring(monitors[i].width) + L"x" + std::to_wstring(monitors[i].height) + L")";
        SendMessage(m_comboTarget, CB_ADDSTRING, 0, (LPARAM)name.c_str());
        m_captureTargets.push_back(monitors[i]);
    }

    auto windows = WgcCapture::EnumerateWindows();
    for (size_t i = 0; i < windows.size(); ++i) {
        std::wstring name = L"[窗口] " + windows[i].title + L" (" + std::to_wstring(windows[i].width) + L"x" + std::to_wstring(windows[i].height) + L")";
        SendMessage(m_comboTarget, CB_ADDSTRING, 0, (LPARAM)name.c_str());
        m_captureTargets.push_back(windows[i]);
    }

    if (!m_captureTargets.empty()) {
        SendMessage(m_comboTarget, CB_SETCURSEL, 0, 0);
    }
}

void MainWindow::RefreshDiscoveredDevices() {
    std::vector<DiscoveredDevice> devicesCopy;
    {
        std::lock_guard<std::mutex> lock(m_devicesMutex);
        devicesCopy = m_cachedDevices;
    }

    SendMessage(m_comboDevices, CB_RESETCONTENT, 0, 0);

    if (devicesCopy.empty()) {
        SendMessage(m_comboDevices, CB_ADDSTRING, 0, (LPARAM)L"未发现设备 (点击右侧搜索或手动输入)");
        SendMessage(m_comboDevices, CB_SETCURSEL, 0, 0);
        return;
    }

    for (const auto& dev : devicesCopy) {
        std::wstring nameW = Utf8ToWide(dev.deviceName);
        std::wstring ipW = Utf8ToWide(dev.ip);
        std::wstring protoW = Utf8ToWide(dev.protocol);
        std::wstring label = nameW + L" (" + ipW + L":" + std::to_wstring(dev.port) + L", " + protoW + L")";
        SendMessage(m_comboDevices, CB_ADDSTRING, 0, (LPARAM)label.c_str());
    }
    SendMessage(m_comboDevices, CB_SETCURSEL, 0, 0);

    // Auto-fill IP & Port if user hasn't typed a custom IP or if only 1 device found
    wchar_t currentIp[64] = {};
    GetWindowTextW(m_editIp, currentIp, 64);
    if (wcslen(currentIp) == 0 || wcscmp(currentIp, L"192.168.1.100") == 0 || devicesCopy.size() == 1) {
        const auto& firstDev = devicesCopy[0];
        std::wstring ipW = Utf8ToWide(firstDev.ip);
        SetWindowTextW(m_editIp, ipW.c_str());
        SetWindowTextW(m_editPort, std::to_wstring(firstDev.port).c_str());
        SendMessage(m_comboProtocol, CB_SETCURSEL, (firstDev.protocol == "TCP" ? 1 : 0), 0);
    }
}

void MainWindow::UpdateUiMode() {
    if (m_isSenderMode) {
        // Adjust window size for Sender Mode
        SetWindowPos(m_hwnd, nullptr, 0, 0, 556, 615, SWP_NOMOVE | SWP_NOZORDER);

        // Show Sender-only controls
        ShowWindow(m_lblTarget, SW_SHOW);
        ShowWindow(m_comboTarget, SW_SHOW);
        ShowWindow(m_btnRefreshTargets, SW_SHOW);
        ShowWindow(m_lblDevices, SW_SHOW);
        ShowWindow(m_comboDevices, SW_SHOW);
        ShowWindow(m_btnRefreshDevices, SW_SHOW);
        ShowWindow(m_lblIp, SW_SHOW);
        ShowWindow(m_editIp, SW_SHOW);
        ShowWindow(m_lblCodec, SW_SHOW);
        ShowWindow(m_comboCodec, SW_SHOW);
        ShowWindow(m_lblBitrate, SW_SHOW);
        ShowWindow(m_comboBitrate, SW_SHOW);
        ShowWindow(m_lblFps, SW_SHOW);
        ShowWindow(m_comboFps, SW_SHOW);
        ShowWindow(m_chkCursor, SW_SHOW);
        ShowWindow(m_chkAudio, SW_SHOW);

        // Position Sender controls
        SetWindowTextW(m_lblIp, L"目标地址:");
        SetWindowPos(m_lblPort, nullptr, 380, 172, 40, 22, SWP_NOZORDER | SWP_SHOWWINDOW);
        SetWindowTextW(m_lblPort, L"端口:");
        SetWindowPos(m_editPort, nullptr, 420, 169, 100, 25, SWP_NOZORDER | SWP_SHOWWINDOW);
        SetWindowPos(m_lblProtocol, nullptr, 274, 288, 75, 22, SWP_NOZORDER | SWP_SHOWWINDOW);
        SetWindowPos(m_comboProtocol, nullptr, 354, 284, 166, 140, SWP_NOZORDER | SWP_SHOWWINDOW);
        SetWindowPos(m_btnAction, nullptr, 20, 504, 504, 46, SWP_NOZORDER | SWP_SHOWWINDOW);

        EnableWindow(m_comboTarget, !m_isStreaming);
        EnableWindow(m_btnRefreshTargets, !m_isStreaming);
        EnableWindow(m_comboDevices, !m_isStreaming);
        EnableWindow(m_btnRefreshDevices, !m_isStreaming);
        EnableWindow(m_editIp, !m_isStreaming);
        EnableWindow(m_editPort, !m_isStreaming);
        EnableWindow(m_comboCodec, !m_isStreaming);
        EnableWindow(m_comboBitrate, !m_isStreaming);
        EnableWindow(m_comboFps, !m_isStreaming);
        EnableWindow(m_comboProtocol, !m_isStreaming);
        EnableWindow(m_chkCursor, !m_isStreaming);
        EnableWindow(m_chkAudio, !m_isStreaming);
    } else {
        // Adjust window size for Receiver Mode (Compact & Clean)
        SetWindowPos(m_hwnd, nullptr, 0, 0, 556, 520, SWP_NOMOVE | SWP_NOZORDER);

        // Hide all Sender-only controls
        ShowWindow(m_lblTarget, SW_HIDE);
        ShowWindow(m_comboTarget, SW_HIDE);
        ShowWindow(m_btnRefreshTargets, SW_HIDE);
        ShowWindow(m_lblDevices, SW_HIDE);
        ShowWindow(m_comboDevices, SW_HIDE);
        ShowWindow(m_btnRefreshDevices, SW_HIDE);
        ShowWindow(m_lblIp, SW_HIDE);
        ShowWindow(m_editIp, SW_HIDE);
        ShowWindow(m_lblCodec, SW_HIDE);
        ShowWindow(m_comboCodec, SW_HIDE);
        ShowWindow(m_lblBitrate, SW_HIDE);
        ShowWindow(m_comboBitrate, SW_HIDE);
        ShowWindow(m_lblFps, SW_HIDE);
        ShowWindow(m_comboFps, SW_HIDE);
        ShowWindow(m_chkCursor, SW_HIDE);
        ShowWindow(m_chkAudio, SW_HIDE);

        // Position Receiver controls inside Card 1
        SetWindowPos(m_lblPort, nullptr, 34, 100, 75, 22, SWP_NOZORDER | SWP_SHOWWINDOW);
        SetWindowTextW(m_lblPort, L"监听端口:");
        SetWindowPos(m_editPort, nullptr, 112, 96, 140, 25, SWP_NOZORDER | SWP_SHOWWINDOW);
        SetWindowPos(m_lblProtocol, nullptr, 274, 100, 75, 22, SWP_NOZORDER | SWP_SHOWWINDOW);
        SetWindowPos(m_comboProtocol, nullptr, 354, 96, 166, 140, SWP_NOZORDER | SWP_SHOWWINDOW);
        SetWindowPos(m_btnAction, nullptr, 20, 412, 504, 46, SWP_NOZORDER | SWP_SHOWWINDOW);

        EnableWindow(m_editPort, !m_isReceiving);
        EnableWindow(m_comboProtocol, !m_isReceiving);
    }

    if (m_hwnd) {
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }
    if (m_btnAction) {
        InvalidateRect(m_btnAction, nullptr, TRUE);
        UpdateWindow(m_btnAction);
    }
    if (m_btnModeSender) {
        InvalidateRect(m_btnModeSender, nullptr, TRUE);
    }
    if (m_btnModeReceiver) {
        InvalidateRect(m_btnModeReceiver, nullptr, TRUE);
    }
}

void MainWindow::UpdateStatusText() {
    if (m_hwnd) {
        RECT rc = m_isSenderMode ? RECT{ 20, 364, 530, 490 } : RECT{ 20, 210, 530, 400 };
        InvalidateRect(m_hwnd, &rc, FALSE);
    }
}

bool MainWindow::StartSender() {
    int targetIdx = (int)SendMessage(m_comboTarget, CB_GETCURSEL, 0, 0);
    if (targetIdx < 0 || targetIdx >= (int)m_captureTargets.size()) {
        MessageBoxW(m_hwnd, L"请先选择采集目标！", L"提示", MB_OK | MB_ICONWARNING);
        return false;
    }

    wchar_t ipBuf[64] = {};
    wchar_t portBuf[16] = {};
    GetWindowTextW(m_editIp, ipBuf, 64);
    GetWindowTextW(m_editPort, portBuf, 16);

    char ipStr[64] = {};
    WideCharToMultiByte(CP_UTF8, 0, ipBuf, -1, ipStr, sizeof(ipStr), nullptr, nullptr);
    uint16_t port = static_cast<uint16_t>(_wtoi(portBuf));

    int codecIdx = (int)SendMessage(m_comboCodec, CB_GETCURSEL, 0, 0);
    VideoCodecType codecType = (codecIdx == 0) ? VideoCodecType::H265_HEVC : VideoCodecType::H264;

    int bitrateIdx = (int)SendMessage(m_comboBitrate, CB_GETCURSEL, 0, 0);
    int bitrates[] = { 8000, 16000, 25000, 40000 };
    int bitrateKbps = bitrates[bitrateIdx >= 0 && bitrateIdx < 4 ? bitrateIdx : 1];

    int fpsIdx = (int)SendMessage(m_comboFps, CB_GETCURSEL, 0, 0);
    int fpsList[] = { 60, 90, 120, 144 };
    int fps = fpsList[fpsIdx >= 0 && fpsIdx < 4 ? fpsIdx : 0];

    int protoIdx = (int)SendMessage(m_comboProtocol, CB_GETCURSEL, 0, 0);
    bool isUdp = (protoIdx == 0);

    bool captureCursor = (SendMessage(m_chkCursor, BM_GETCHECK, 0, 0) == BST_CHECKED);
    bool captureAudio = (SendMessage(m_chkAudio, BM_GETCHECK, 0, 0) == BST_CHECKED);

    Logger::I("MainWindow", "Starting Sender Pipeline -> " + std::string(ipStr) + ":" + std::to_string(port));

    // 1. Init Network Streamer
    if (isUdp) {
        m_udpStreamer = std::make_unique<UdpStreamer>();
        if (!m_udpStreamer->Start(ipStr, port)) {
            MessageBoxW(m_hwnd, L"无法启动 UDP 发送器！", L"错误", MB_OK | MB_ICONERROR);
            return false;
        }
    } else {
        m_tcpStreamer = std::make_unique<TcpStreamer>();
        if (!m_tcpStreamer->Start(ipStr, port)) {
            MessageBoxW(m_hwnd, L"无法连接到接收端 TCP 端口！", L"错误", MB_OK | MB_ICONERROR);
            return false;
        }
    }

    // 2. Init Audio Encoder & Capture
    if (captureAudio) {
        m_audioEncoder = std::make_unique<WmfAudioEncoder>();
        m_audioEncoder->Initialize(48000, 2, 128000);
        m_audioEncoder->SetEncodedCallback([this](const uint8_t* data, size_t size, int64_t timestampMs) {
            if (!m_isStreaming) return;
            if (m_udpStreamer && m_udpStreamer->IsRunning()) {
                m_udpStreamer->SendFrame(data, size, timestampMs, false, false, false, true);
            } else if (m_tcpStreamer && m_tcpStreamer->IsConnected()) {
                m_tcpStreamer->SendFrame(data, size, timestampMs, false, false, false, true);
            }
        });

        m_wasapiCapture = std::make_unique<WasapiCapture>();
        m_wasapiCapture->SetAudioCallback([this](const uint8_t* pcm, size_t bytes, int64_t tsNs) {
            if (!m_isStreaming) return;
            if (m_audioEncoder && m_audioEncoder->IsInitialized()) {
                m_audioEncoder->EncodePcm(pcm, bytes, tsNs);
            }
        });
        m_wasapiCapture->Start();
    }

    // 3. Init Video Encoder
    const auto& target = m_captureTargets[targetIdx];
    int initW = target.width > 0 ? target.width : 1920;
    int initH = target.height > 0 ? target.height : 1080;
    m_statWidth = initW;
    m_statHeight = initH;
    m_fpsCounter = 0;
    m_statFps = 0;
    m_statBitrateKbps = 0;
    m_statRttMs = 0;

    m_videoEncoder = std::make_unique<WmfVideoEncoder>(m_d3dResources.device.Get(), m_d3dResources.dxgiManager.Get());
    if (!m_videoEncoder->Initialize(initW, initH, fps, bitrateKbps, codecType)) {
        MessageBoxW(m_hwnd, L"硬件视频编码器初始化失败！", L"错误", MB_OK | MB_ICONERROR);
        StopSender();
        return false;
    }

    m_videoEncoder->SetEncodedCallback([this](const uint8_t* data, size_t size, int64_t timestampMs, bool isKeyframe, bool isCodecConfig, bool isHevc) {
        if (!m_isStreaming) return;
        m_fpsCounter.fetch_add(1);
        if (m_udpStreamer && m_udpStreamer->IsRunning()) {
            m_udpStreamer->SendFrame(data, size, timestampMs, isKeyframe, isCodecConfig, isHevc, false);
        } else if (m_tcpStreamer && m_tcpStreamer->IsConnected()) {
            m_tcpStreamer->SendFrame(data, size, timestampMs, isKeyframe, isCodecConfig, isHevc, false);
        }
    });

    // 4. Init WGC Capture
    m_wgcCapture = std::make_unique<WgcCapture>(m_d3dResources.device.Get());
    m_wgcCapture->SetFrameCallback([this](ID3D11Texture2D* texture, int64_t tsNs, int w, int h) {
        if (!m_isStreaming) return;
        m_statWidth = w;
        m_statHeight = h;
        if (m_videoEncoder && m_videoEncoder->IsInitialized()) {
            m_videoEncoder->EncodeFrame(texture, tsNs);
        }
    });

    if (!m_wgcCapture->StartCapture(target, captureCursor)) {
        MessageBoxW(m_hwnd, L"WGC 屏幕采集启动失败！", L"错误", MB_OK | MB_ICONERROR);
        StopSender();
        return false;
    }

    m_isStreaming = true;
    UpdateUiMode();
    UpdateStatusText();
    return true;
}

void MainWindow::StopSender() {
    m_isStreaming = false;
    m_statFps = 0;
    m_statBitrateKbps = 0;
    m_statRttMs = 0;
    m_fpsCounter = 0;
    if (m_wgcCapture) {
        m_wgcCapture->StopCapture();
        m_wgcCapture = nullptr;
    }
    if (m_wasapiCapture) {
        m_wasapiCapture->Stop();
        m_wasapiCapture = nullptr;
    }
    if (m_videoEncoder) {
        m_videoEncoder->Shutdown();
        m_videoEncoder = nullptr;
    }
    if (m_audioEncoder) {
        m_audioEncoder->Shutdown();
        m_audioEncoder = nullptr;
    }
    if (m_udpStreamer) {
        m_udpStreamer->Stop();
        m_udpStreamer = nullptr;
    }
    if (m_tcpStreamer) {
        m_tcpStreamer->Stop();
        m_tcpStreamer = nullptr;
    }
    UpdateUiMode();
    UpdateStatusText();
}

void MainWindow::CreateReceiverWindow() {
    if (m_hwndReceiverView) return;

    int winW = 1280;
    int winH = 720;
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);

    m_postedAdaptW = 0;
    m_postedAdaptH = 0;
    m_appliedAdaptedW = 0;
    m_appliedAdaptedH = 0;

    m_hwndReceiverView = CreateWindowExW(
        0,
        L"ScreenLiveStreamReceiverView",
        L"ScreenLiveStream 投屏播放窗口",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        (screenW - winW) / 2,
        (screenH - winH) / 2,
        winW,
        winH,
        m_hwnd,
        nullptr,
        m_hInstance,
        this
    );

    m_d3dRenderer = std::make_unique<D3D11Renderer>(m_d3dResources.device.Get(), m_d3dResources.context.Get());
    m_d3dRenderer->Initialize(m_hwndReceiverView, winW, winH);
}

void MainWindow::DestroyReceiverWindow() {
    if (m_d3dRenderer) {
        m_d3dRenderer->Shutdown();
        m_d3dRenderer = nullptr;
    }
    if (m_hwndReceiverView) {
        DestroyWindow(m_hwndReceiverView);
        m_hwndReceiverView = nullptr;
    }
}

void MainWindow::AutoAdaptReceiverWindow(int videoW, int videoH) {
    if (!m_hwndReceiverView || videoW <= 0 || videoH <= 0) return;
    if (m_appliedAdaptedW == videoW && m_appliedAdaptedH == videoH) return;

    m_appliedAdaptedW = videoW;
    m_appliedAdaptedH = videoH;

    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);

    int targetClientW = 0;
    int targetClientH = 0;

    if (videoW < videoH) {
        targetClientH = (std::min)(840, screenH - 120);
        targetClientW = static_cast<int>(targetClientH * (static_cast<float>(videoW) / static_cast<float>(videoH)));
    } else {
        targetClientW = (std::min)(1280, screenW - 100);
        targetClientH = static_cast<int>(targetClientW * (static_cast<float>(videoH) / static_cast<float>(videoW)));
    }

    RECT rc = { 0, 0, targetClientW, targetClientH };
    AdjustWindowRectEx(&rc, GetWindowLong(m_hwndReceiverView, GWL_STYLE), FALSE, GetWindowLong(m_hwndReceiverView, GWL_EXSTYLE));

    int winW = rc.right - rc.left;
    int winH = rc.bottom - rc.top;
    int posX = (screenW - winW) / 2;
    int posY = (screenH - winH) / 2;

    SetWindowPos(m_hwndReceiverView, nullptr, posX, posY, winW, winH, SWP_NOZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    if (m_d3dRenderer) {
        m_d3dRenderer->Resize(targetClientW, targetClientH);
    }
    Logger::I("MainWindow", "Auto-adapted receiver window size: " + std::to_string(targetClientW) + "x" + std::to_string(targetClientH));
}

void MainWindow::ReceiverDecodeLoop() {
    Logger::I("MainWindow", "Receiver Decode Loop thread started (FFmpeg Engine).");
    while (m_isDecoding) {
        ReceiverVideoPacket pkt;
        bool isLatestInBatch = false;
        {
            std::unique_lock<std::mutex> lock(m_frameQueueMutex);
            m_frameQueueCv.wait(lock, [this] {
                return !m_isDecoding || !m_frameQueue.empty();
            });

            if (!m_isDecoding) break;

            pkt = std::move(m_frameQueue.front());
            m_frameQueue.pop_front();

            // Decode all packets for P-frame reference continuity, but only present the latest frame in batch
            isLatestInBatch = m_frameQueue.empty();
        }

        if (m_videoDecoder && !pkt.data.empty()) {
            VideoCodecType targetCodec = pkt.isHevc ? VideoCodecType::H265_HEVC : VideoCodecType::H264;
            if (m_videoDecoder->GetCodecType() != targetCodec) {
                m_videoDecoder->Initialize(targetCodec);
            }
            m_shouldRenderFrame.store(isLatestInBatch, std::memory_order_relaxed);
            m_videoDecoder->DecodeNalu(pkt.data.data(), pkt.data.size(), pkt.timestampMs);
        }
    }
    Logger::I("MainWindow", "Receiver Decode Loop thread terminated.");
}

bool MainWindow::StartReceiver() {
    wchar_t portBuf[16] = {};
    GetWindowTextW(m_editPort, portBuf, 16);
    uint16_t port = static_cast<uint16_t>(_wtoi(portBuf));
    if (port == 0) port = 8888;

    int protoIdx = (int)SendMessage(m_comboProtocol, CB_GETCURSEL, 0, 0);
    bool isUdp = (protoIdx == 0);

    CreateReceiverWindow();

    // 1. Audio Decoder & Player
    m_wasapiPlayer = std::make_unique<WasapiPlayer>();
    m_wasapiPlayer->Start(48000, 2);

    m_audioDecoder = std::make_unique<WmfAudioDecoder>();
    m_audioDecoder->Initialize(48000, 2);
    m_audioDecoder->SetDecodedCallback([this](const uint8_t* pcm, size_t bytes, int64_t) {
        if (m_wasapiPlayer && m_wasapiPlayer->IsPlaying()) {
            m_wasapiPlayer->PushPcm(pcm, bytes);
        }
    });

    // 2. FFmpeg Video Decoder (D3D11VA Hardware Accelerated)
    m_fpsCounter = 0;
    m_statFps = 0;
    m_statBitrateKbps = 0;
    m_statRttMs = 0;
    m_videoDecoder = std::make_unique<FfmpegVideoDecoder>(m_d3dResources.device.Get());
    m_videoDecoder->Initialize(VideoCodecType::H265_HEVC);
    m_videoDecoder->SetDecodedCallback([this](ID3D11Texture2D* tex, int64_t, int w, int h) {
        m_statWidth = w;
        m_statHeight = h;
        m_fpsCounter.fetch_add(1);

        if (m_hwnd && (m_postedAdaptW.load() != w || m_postedAdaptH.load() != h)) {
            m_postedAdaptW = w;
            m_postedAdaptH = h;
            PostMessage(m_hwnd, WM_USER_ADAPT_WINDOW, w, h);
        }

        if (m_d3dRenderer && m_shouldRenderFrame.load(std::memory_order_relaxed)) {
            m_d3dRenderer->RenderFrame(tex, w, h);
        }
    });

    // 3. Start Asynchronous Video Decoder Worker Thread
    m_isDecoding = true;
    m_receiverDecodeThread = std::thread(&MainWindow::ReceiverDecodeLoop, this);

    // 4. Network Receiver
    if (isUdp) {
        m_udpReceiver = std::make_unique<UdpReceiver>();
        m_udpReceiver->SetVideoCallback([this](const uint8_t* data, size_t size, int64_t ts, bool isKeyframe, bool isCodecConfig, bool isHevc) {
            if (!m_isReceiving || !m_isDecoding || !data || size == 0) return;

            ReceiverVideoPacket pkt;
            pkt.data.assign(data, data + size);
            pkt.timestampMs = ts;
            pkt.isKeyframe = isKeyframe;
            pkt.isCodecConfig = isCodecConfig;
            pkt.isHevc = isHevc;

            {
                std::lock_guard<std::mutex> lock(m_frameQueueMutex);
                if (isKeyframe && m_frameQueue.size() > 5) {
                    m_frameQueue.clear();
                }
                m_frameQueue.push_back(std::move(pkt));
            }
            m_frameQueueCv.notify_one();
        });
        m_udpReceiver->SetAudioCallback([this](const uint8_t* data, size_t size, int64_t ts) {
            if (m_isReceiving && m_audioDecoder) {
                m_audioDecoder->DecodeAac(data, size, ts);
            }
        });
        m_udpReceiver->SetStatsCallback([this](int rtt, int loss) {
            m_statRttMs = rtt;
            m_statLossBps = loss;
        });
        m_udpReceiver->Start(port);
    } else {
        m_tcpReceiver = std::make_unique<TcpReceiver>();
        m_tcpReceiver->SetVideoCallback([this](const uint8_t* data, size_t size, int64_t ts, bool isKeyframe, bool isCodecConfig, bool isHevc) {
            if (!m_isReceiving || !m_isDecoding || !data || size == 0) return;

            ReceiverVideoPacket pkt;
            pkt.data.assign(data, data + size);
            pkt.timestampMs = ts;
            pkt.isKeyframe = isKeyframe;
            pkt.isCodecConfig = isCodecConfig;
            pkt.isHevc = isHevc;

            {
                std::lock_guard<std::mutex> lock(m_frameQueueMutex);
                if (isKeyframe && m_frameQueue.size() > 5) {
                    m_frameQueue.clear();
                }
                m_frameQueue.push_back(std::move(pkt));
            }
            m_frameQueueCv.notify_one();
        });
        m_tcpReceiver->SetAudioCallback([this](const uint8_t* data, size_t size, int64_t ts) {
            if (m_isReceiving && m_audioDecoder) {
                m_audioDecoder->DecodeAac(data, size, ts);
            }
        });
        m_tcpReceiver->Start(port);
    }

    // 5. Announce on LAN
    m_lanDiscovery->StartAnnouncing(port, isUdp ? "UDP" : "TCP", "Windows-PC");

    m_isReceiving = true;
    UpdateUiMode();
    UpdateStatusText();
    return true;
}

void MainWindow::StopReceiver() {
    m_isReceiving = false;
    m_lanDiscovery->StopAnnouncing();

    if (m_udpReceiver) {
        m_udpReceiver->Stop();
        m_udpReceiver = nullptr;
    }
    if (m_tcpReceiver) {
        m_tcpReceiver->Stop();
        m_tcpReceiver = nullptr;
    }

    // Stop and join decode thread
    m_isDecoding = false;
    m_frameQueueCv.notify_all();
    if (m_receiverDecodeThread.joinable()) {
        m_receiverDecodeThread.join();
    }
    {
        std::lock_guard<std::mutex> lock(m_frameQueueMutex);
        m_frameQueue.clear();
    }

    if (m_videoDecoder) {
        m_videoDecoder->Shutdown();
        m_videoDecoder = nullptr;
    }
    if (m_audioDecoder) {
        m_audioDecoder->Shutdown();
        m_audioDecoder = nullptr;
    }
    if (m_wasapiPlayer) {
        m_wasapiPlayer->Stop();
        m_wasapiPlayer = nullptr;
    }
    m_statFps = 0;
    m_statBitrateKbps = 0;
    m_statRttMs = 0;
    m_fpsCounter = 0;
    DestroyReceiverWindow();
    UpdateUiMode();
    UpdateStatusText();
}

int MainWindow::Run() {
    MSG msg = {};
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return static_cast<int>(msg.wParam);
}

LRESULT CALLBACK MainWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    MainWindow* pThis = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCT*>(lParam);
        pThis = reinterpret_cast<MainWindow*>(cs->lpCreateParams);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pThis));
    } else {
        pThis = reinterpret_cast<MainWindow*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    }

    if (!pThis) return DefWindowProc(hwnd, msg, wParam, lParam);

    switch (msg) {
        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);

            RECT clientRc;
            GetClientRect(hwnd, &clientRc);
            int width = clientRc.right - clientRc.left;
            int height = clientRc.bottom - clientRc.top;

            HDC memDC = CreateCompatibleDC(hdc);
            HBITMAP memBitmap = CreateCompatibleBitmap(hdc, width, height);
            HBITMAP oldBitmap = (HBITMAP)SelectObject(memDC, memBitmap);

            {
                Gdiplus::Graphics g(memDC);
                g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);

                // 1. Fill Dark Base Background (#121214)
                Gdiplus::SolidBrush bgBrush(Gdiplus::Color(255, 18, 18, 20));
                g.FillRectangle(&bgBrush, 0, 0, width, height);

                Gdiplus::Font headerFont(L"Microsoft YaHei UI", 9.5f, Gdiplus::FontStyleBold, Gdiplus::UnitPoint);
                Gdiplus::Font smallFont(L"Microsoft YaHei UI", 8.5f, Gdiplus::FontStyleRegular, Gdiplus::UnitPoint);

                Gdiplus::SolidBrush textWhite(Gdiplus::Color(255, 244, 244, 245));
                Gdiplus::SolidBrush textMuted(Gdiplus::Color(255, 161, 161, 170));
                Gdiplus::SolidBrush cardBg(Gdiplus::Color(255, 30, 30, 36)); // #1E1E24
                Gdiplus::Pen cardBorderPen(Gdiplus::Color(255, 46, 46, 56), 1.0f); // #2E2E38

                // 2. Role Switcher Capsule Background (X: 20, Y: 14, W: 504, H: 42)
                Gdiplus::GraphicsPath roleBoxPath;
                Gdiplus::RectF roleBoxRc(20.0f, 14.0f, 504.0f, 42.0f);
                AddRoundedRectToPath(roleBoxPath, roleBoxRc, 8.0f);
                Gdiplus::SolidBrush roleBoxBg(Gdiplus::Color(255, 24, 24, 29)); // #18181D
                g.FillPath(&roleBoxBg, &roleBoxPath);
                g.DrawPath(&cardBorderPen, &roleBoxPath);

                if (pThis->m_isSenderMode) {
                    // 3. Card 1: 采集与目标设备 (X: 20, Y: 66, W: 504, H: 146)
                    Gdiplus::GraphicsPath card1Path;
                    Gdiplus::RectF card1Rc(20.0f, 66.0f, 504.0f, 146.0f);
                    AddRoundedRectToPath(card1Path, card1Rc, 8.0f);
                    g.FillPath(&cardBg, &card1Path);
                    g.DrawPath(&cardBorderPen, &card1Path);
                    g.DrawString(L"采集与目标设备", -1, &headerFont, Gdiplus::PointF(34.0f, 74.0f), &textWhite);

                    // 4. Card 2: 编码与网络设置 (X: 20, Y: 220, W: 504, H: 136)
                    Gdiplus::GraphicsPath card2Path;
                    Gdiplus::RectF card2Rc(20.0f, 220.0f, 504.0f, 136.0f);
                    AddRoundedRectToPath(card2Path, card2Rc, 8.0f);
                    g.FillPath(&cardBg, &card2Path);
                    g.DrawPath(&cardBorderPen, &card2Path);
                    g.DrawString(L"编码与网络设置", -1, &headerFont, Gdiplus::PointF(34.0f, 228.0f), &textWhite);

                    // 5. Card 3: 实时运行状态 (X: 20, Y: 364, W: 504, H: 122)
                    Gdiplus::GraphicsPath card3Path;
                    Gdiplus::RectF card3Rc(20.0f, 364.0f, 504.0f, 122.0f);
                    AddRoundedRectToPath(card3Path, card3Rc, 8.0f);
                    g.FillPath(&cardBg, &card3Path);
                    g.DrawPath(&cardBorderPen, &card3Path);
                    g.DrawString(L"实时运行状态", -1, &headerFont, Gdiplus::PointF(34.0f, 372.0f), &textWhite);

                    // Status Indicator Dot (Native Circle)
                    bool isRunning = pThis->m_isStreaming.load();
                    Gdiplus::Color dotColor = isRunning ? Gdiplus::Color(255, 16, 185, 129) : Gdiplus::Color(255, 113, 113, 122);
                    Gdiplus::SolidBrush dotBrush(dotColor);
                    g.FillEllipse(&dotBrush, 36.0f, 401.0f, 8.0f, 8.0f);

                    // Status Text
                    std::wstring statusStr = isRunning ? L"正在推流中 (Zero-Copy GPU -> WMF MFT 硬件编码)" : L"就绪 (Ready) - 选择采集目标后点击下方按钮启动";
                    g.DrawString(statusStr.c_str(), -1, &smallFont, Gdiplus::PointF(50.0f, 398.0f), isRunning ? &textWhite : &textMuted);

                    // 4 Metric Badges
                    auto DrawMetricBadge = [&](float bx, float by, float bw, float bh, const std::wstring& label, const std::wstring& val) {
                        Gdiplus::GraphicsPath bpath;
                        Gdiplus::RectF brc(bx, by, bw, bh);
                        AddRoundedRectToPath(bpath, brc, 4.0f);
                        Gdiplus::SolidBrush bbg(Gdiplus::Color(255, 24, 24, 29)); // #18181D
                        g.FillPath(&bbg, &bpath);
                        g.DrawPath(&cardBorderPen, &bpath);

                        std::wstring combined = label + L": " + val;
                        g.DrawString(combined.c_str(), -1, &smallFont, Gdiplus::PointF(bx + 8.0f, by + 5.0f), &textWhite);
                    };

                    std::wstring resStr = (pThis->m_statWidth > 0 && pThis->m_statHeight > 0) ? (std::to_wstring(pThis->m_statWidth) + L"x" + std::to_wstring(pThis->m_statHeight)) : L"1920x1080";
                    std::wstring fpsStr = std::to_wstring(pThis->m_statFps.load()) + L" FPS";
                    std::wstring rttStr = std::to_wstring(pThis->m_statRttMs.load()) + L" ms";
                    std::wstringstream brStream;
                    brStream << std::fixed << std::setprecision(1) << (pThis->m_statBitrateKbps / 1000.0f) << L" Mbps";
                    std::wstring brStr = brStream.str();

                    float badgeW = 114.0f;
                    float badgeH = 26.0f;
                    float badgeY = 444.0f;
                    DrawMetricBadge(34.0f, badgeY, badgeW, badgeH, L"分辨率", resStr);
                    DrawMetricBadge(156.0f, badgeY, badgeW, badgeH, L"实时帧率", fpsStr);
                    DrawMetricBadge(278.0f, badgeY, badgeW, badgeH, L"往返延迟", rttStr);
                    DrawMetricBadge(400.0f, badgeY, badgeW, badgeH, L"当前码率", brStr);
                } else {
                    // In Receiver Mode
                    // 3. Card 1: 接收服务设置 (X: 20, Y: 66, W: 504, H: 136)
                    Gdiplus::GraphicsPath card1Path;
                    Gdiplus::RectF card1Rc(20.0f, 66.0f, 504.0f, 136.0f);
                    AddRoundedRectToPath(card1Path, card1Rc, 8.0f);
                    g.FillPath(&cardBg, &card1Path);
                    g.DrawPath(&cardBorderPen, &card1Path);
                    g.DrawString(L"接收服务设置", -1, &headerFont, Gdiplus::PointF(34.0f, 74.0f), &textWhite);

                    // Guide tips in Card 1
                    g.DrawString(L"• 启动后将在局域网自动广播本机 (Windows-PC) 接收服务", -1, &smallFont, Gdiplus::PointF(34.0f, 132.0f), &textMuted);
                    g.DrawString(L"• 手机 / Quest 开启投屏 App 即可在设备列表中发现并一键连接", -1, &smallFont, Gdiplus::PointF(34.0f, 154.0f), &textMuted);
                    g.DrawString(L"• 视频格式与码率由移动端决定，本机自动调用 D3D11VA 硬解加速", -1, &smallFont, Gdiplus::PointF(34.0f, 176.0f), &textMuted);

                    // 4. Card 2: 实时接收状态 (X: 20, Y: 210, W: 504, H: 186)
                    Gdiplus::GraphicsPath card2Path;
                    Gdiplus::RectF card2Rc(20.0f, 210.0f, 504.0f, 186.0f);
                    AddRoundedRectToPath(card2Path, card2Rc, 8.0f);
                    g.FillPath(&cardBg, &card2Path);
                    g.DrawPath(&cardBorderPen, &card2Path);
                    g.DrawString(L"实时接收状态", -1, &headerFont, Gdiplus::PointF(34.0f, 218.0f), &textWhite);

                    bool isRunning = pThis->m_isReceiving.load();
                    Gdiplus::Color dotColor = isRunning ? Gdiplus::Color(255, 16, 185, 129) : Gdiplus::Color(255, 113, 113, 122);
                    Gdiplus::SolidBrush dotBrush(dotColor);
                    g.FillEllipse(&dotBrush, 36.0f, 247.0f, 8.0f, 8.0f);

                    std::wstring statusStr = isRunning ? L"正在接收中 (FFmpeg D3D11VA 硬件加速解码)" : L"接收就绪 (Ready) - 启动接收端后，在移动端连接本机即可";
                    g.DrawString(statusStr.c_str(), -1, &smallFont, Gdiplus::PointF(50.0f, 244.0f), isRunning ? &textWhite : &textMuted);

                    // 4 Metric Badges
                    auto DrawMetricBadge = [&](float bx, float by, float bw, float bh, const std::wstring& label, const std::wstring& val) {
                        Gdiplus::GraphicsPath bpath;
                        Gdiplus::RectF brc(bx, by, bw, bh);
                        AddRoundedRectToPath(bpath, brc, 4.0f);
                        Gdiplus::SolidBrush bbg(Gdiplus::Color(255, 24, 24, 29)); // #18181D
                        g.FillPath(&bbg, &bpath);
                        g.DrawPath(&cardBorderPen, &bpath);

                        std::wstring combined = label + L": " + val;
                        g.DrawString(combined.c_str(), -1, &smallFont, Gdiplus::PointF(bx + 8.0f, by + 5.0f), &textWhite);
                    };

                    std::wstring resStr = (pThis->m_statWidth > 0 && pThis->m_statHeight > 0) ? (std::to_wstring(pThis->m_statWidth) + L"x" + std::to_wstring(pThis->m_statHeight)) : L"1920x1080";
                    std::wstring fpsStr = std::to_wstring(pThis->m_statFps.load()) + L" FPS";
                    std::wstring rttStr = std::to_wstring(pThis->m_statRttMs.load()) + L" ms";
                    std::wstringstream brStream;
                    brStream << std::fixed << std::setprecision(1) << (pThis->m_statBitrateKbps / 1000.0f) << L" Mbps";
                    std::wstring brStr = brStream.str();

                    float badgeW = 114.0f;
                    float badgeH = 26.0f;
                    float badgeY = 280.0f;
                    DrawMetricBadge(34.0f, badgeY, badgeW, badgeH, L"分辨率", resStr);
                    DrawMetricBadge(156.0f, badgeY, badgeW, badgeH, L"实时帧率", fpsStr);
                    DrawMetricBadge(278.0f, badgeY, badgeW, badgeH, L"往返延迟", rttStr);
                    DrawMetricBadge(400.0f, badgeY, badgeW, badgeH, L"当前码率", brStr);

                    // Pipeline info
                    std::wstring pipeInfo = isRunning ? L"渲染管线: D3D11VA Texture -> SwapChain 直接呈现 | 音频: WASAPI 低延迟播放" : L"等待移动端连接推流中...";
                    g.DrawString(pipeInfo.c_str(), -1, &smallFont, Gdiplus::PointF(34.0f, 320.0f), isRunning ? &textWhite : &textMuted);
                }
            }

            BitBlt(hdc, 0, 0, width, height, memDC, 0, 0, SRCCOPY);
            SelectObject(memDC, oldBitmap);
            DeleteObject(memBitmap);
            DeleteDC(memDC);

            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_DRAWITEM: {
            auto* pDraw = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            if (!pDraw) break;

            Gdiplus::Graphics g(pDraw->hDC);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);

            int w = pDraw->rcItem.right - pDraw->rcItem.left;
            int h = pDraw->rcItem.bottom - pDraw->rcItem.top;
            Gdiplus::RectF rf(0.0f, 0.0f, (float)w, (float)h);

            bool isSelectedState = (pDraw->itemState & ODS_SELECTED);
            bool isDisabledState = (pDraw->itemState & ODS_DISABLED);

            if (pDraw->CtlID == ID_BTN_MODE_SENDER || pDraw->CtlID == ID_BTN_MODE_RECEIVER) {
                bool isCurrentMode = (pDraw->CtlID == ID_BTN_MODE_SENDER) ? pThis->m_isSenderMode : !pThis->m_isSenderMode;
                Gdiplus::GraphicsPath path;
                AddRoundedRectToPath(path, rf, 6.0f);

                if (isCurrentMode) {
                    Gdiplus::LinearGradientBrush pillGrad(rf, Gdiplus::Color(255, 99, 102, 241), Gdiplus::Color(255, 79, 70, 229), Gdiplus::LinearGradientModeVertical);
                    g.FillPath(&pillGrad, &path);
                    Gdiplus::Pen borderPen(Gdiplus::Color(255, 129, 140, 248), 1.0f);
                    g.DrawPath(&borderPen, &path);

                    Gdiplus::Font font(L"Microsoft YaHei UI", 9.0f, Gdiplus::FontStyleBold, Gdiplus::UnitPoint);
                    Gdiplus::SolidBrush textBrush(Gdiplus::Color(255, 255, 255, 255));
                    Gdiplus::StringFormat sf;
                    sf.SetAlignment(Gdiplus::StringAlignmentCenter);
                    sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);

                    const wchar_t* title = (pDraw->CtlID == ID_BTN_MODE_SENDER) ? L"电脑发送端 (投给手机/Quest)" : L"电脑接收端 (接收手机/Quest)";
                    g.DrawString(title, -1, &font, rf, &sf, &textBrush);
                } else {
                    Gdiplus::SolidBrush inactiveBg(Gdiplus::Color(255, 24, 24, 29));
                    g.FillPath(&inactiveBg, &path);

                    Gdiplus::Font font(L"Microsoft YaHei UI", 9.0f, Gdiplus::FontStyleRegular, Gdiplus::UnitPoint);
                    Gdiplus::SolidBrush textBrush(Gdiplus::Color(255, 161, 161, 170));
                    Gdiplus::StringFormat sf;
                    sf.SetAlignment(Gdiplus::StringAlignmentCenter);
                    sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);

                    const wchar_t* title = (pDraw->CtlID == ID_BTN_MODE_SENDER) ? L"电脑发送端" : L"电脑接收端";
                    g.DrawString(title, -1, &font, rf, &sf, &textBrush);
                }
                return TRUE;
            }

            if (pDraw->CtlID == ID_BTN_REFRESH_TARGETS || pDraw->CtlID == ID_BTN_REFRESH_DEVICES) {
                Gdiplus::GraphicsPath path;
                AddRoundedRectToPath(path, rf, 4.0f);

                Gdiplus::Color btnBg = isSelectedState ? Gdiplus::Color(255, 30, 30, 36) : Gdiplus::Color(255, 39, 39, 48);
                Gdiplus::SolidBrush brush(btnBg);
                g.FillPath(&brush, &path);
                Gdiplus::Pen border(Gdiplus::Color(255, 63, 63, 78), 1.0f);
                g.DrawPath(&border, &path);

                Gdiplus::Font font(L"Microsoft YaHei UI", 9.0f, Gdiplus::FontStyleRegular, Gdiplus::UnitPoint);
                Gdiplus::SolidBrush textBrush(Gdiplus::Color(255, 244, 244, 245));
                Gdiplus::StringFormat sf;
                sf.SetAlignment(Gdiplus::StringAlignmentCenter);
                sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);

                const wchar_t* txt = (pDraw->CtlID == ID_BTN_REFRESH_TARGETS) ? L"刷新" : L"搜索";
                g.DrawString(txt, -1, &font, rf, &sf, &textBrush);
                return TRUE;
            }

            if (pDraw->CtlID == ID_BTN_ACTION) {
                Gdiplus::GraphicsPath path;
                AddRoundedRectToPath(path, rf, 8.0f);

                bool isStop = (pThis->m_isStreaming || pThis->m_isReceiving);
                if (isDisabledState) {
                    Gdiplus::SolidBrush brush(Gdiplus::Color(255, 39, 39, 48));
                    g.FillPath(&brush, &path);
                    Gdiplus::Font font(L"Microsoft YaHei UI", 11.0f, Gdiplus::FontStyleBold, Gdiplus::UnitPoint);
                    Gdiplus::SolidBrush textBrush(Gdiplus::Color(255, 113, 113, 122));
                    Gdiplus::StringFormat sf;
                    sf.SetAlignment(Gdiplus::StringAlignmentCenter);
                    sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
                    g.DrawString(L"处理中...", -1, &font, rf, &sf, &textBrush);
                } else if (isStop) {
                    Gdiplus::LinearGradientBrush brush(rf, Gdiplus::Color(255, 239, 68, 68), Gdiplus::Color(255, 220, 38, 38), Gdiplus::LinearGradientModeVertical);
                    g.FillPath(&brush, &path);
                    Gdiplus::Pen border(Gdiplus::Color(255, 248, 113, 113), 1.0f);
                    g.DrawPath(&border, &path);

                    Gdiplus::Font font(L"Microsoft YaHei UI", 11.0f, Gdiplus::FontStyleBold, Gdiplus::UnitPoint);
                    Gdiplus::SolidBrush textBrush(Gdiplus::Color(255, 255, 255, 255));
                    Gdiplus::StringFormat sf;
                    sf.SetAlignment(Gdiplus::StringAlignmentCenter);
                    sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);

                    const wchar_t* txt = pThis->m_isSenderMode ? L"停止画面投屏" : L"停止接收端";
                    g.DrawString(txt, -1, &font, rf, &sf, &textBrush);
                } else {
                    Gdiplus::LinearGradientBrush brush(rf, Gdiplus::Color(255, 99, 102, 241), Gdiplus::Color(255, 79, 70, 229), Gdiplus::LinearGradientModeVertical);
                    g.FillPath(&brush, &path);
                    Gdiplus::Pen border(Gdiplus::Color(255, 129, 140, 248), 1.0f);
                    g.DrawPath(&border, &path);

                    Gdiplus::Font font(L"Microsoft YaHei UI", 11.0f, Gdiplus::FontStyleBold, Gdiplus::UnitPoint);
                    Gdiplus::SolidBrush textBrush(Gdiplus::Color(255, 255, 255, 255));
                    Gdiplus::StringFormat sf;
                    sf.SetAlignment(Gdiplus::StringAlignmentCenter);
                    sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);

                    const wchar_t* txt = pThis->m_isSenderMode ? L"启动画面投屏" : L"启动接收端";
                    g.DrawString(txt, -1, &font, rf, &sf, &textBrush);
                }
                return TRUE;
            }
            break;
        }

        case WM_CTLCOLORSTATIC: {
            HDC hdcStatic = (HDC)wParam;
            SetTextColor(hdcStatic, RGB(228, 228, 231));
            SetBkMode(hdcStatic, TRANSPARENT);
            return (LRESULT)pThis->m_hBrushCard;
        }

        case WM_CTLCOLOREDIT: {
            HDC hdcEdit = (HDC)wParam;
            SetTextColor(hdcEdit, RGB(250, 250, 250));
            SetBkColor(hdcEdit, RGB(24, 24, 29));
            return (LRESULT)pThis->m_hBrushInput;
        }

        case WM_CTLCOLORLISTBOX: {
            HDC hdcList = (HDC)wParam;
            SetTextColor(hdcList, RGB(250, 250, 250));
            SetBkColor(hdcList, RGB(24, 24, 29));
            return (LRESULT)pThis->m_hBrushInput;
        }

        case WM_CTLCOLORBTN: {
            return (LRESULT)pThis->m_hBrushBg;
        }

        case WM_COMMAND: {
            WORD id = LOWORD(wParam);
            WORD code = HIWORD(wParam);

            if (id == ID_BTN_MODE_SENDER && code == BN_CLICKED) {
                if (pThis->m_isReceiving) pThis->StopReceiver();
                pThis->m_isSenderMode = true;
                pThis->UpdateUiMode();
                pThis->UpdateStatusText();
            } else if (id == ID_BTN_MODE_RECEIVER && code == BN_CLICKED) {
                if (pThis->m_isStreaming) pThis->StopSender();
                pThis->m_isSenderMode = false;
                pThis->UpdateUiMode();
                pThis->UpdateStatusText();
            } else if (id == ID_BTN_REFRESH_TARGETS && code == BN_CLICKED) {
                pThis->RefreshCaptureTargets();
            } else if (id == ID_BTN_REFRESH_DEVICES && code == BN_CLICKED) {
                if (pThis->m_lanDiscovery) {
                    pThis->m_lanDiscovery->Rescan();
                }
                InvalidateRect(hwnd, nullptr, FALSE);
            } else if (id == ID_COMBO_DEVICES && code == CBN_SELCHANGE) {
                int sel = (int)SendMessage(pThis->m_comboDevices, CB_GETCURSEL, 0, 0);
                std::lock_guard<std::mutex> lock(pThis->m_devicesMutex);
                if (sel >= 0 && sel < (int)pThis->m_cachedDevices.size()) {
                    const auto& dev = pThis->m_cachedDevices[sel];
                    std::wstring ipW = Utf8ToWide(dev.ip);
                    SetWindowTextW(pThis->m_editIp, ipW.c_str());
                    SetWindowTextW(pThis->m_editPort, std::to_wstring(dev.port).c_str());
                    SendMessage(pThis->m_comboProtocol, CB_SETCURSEL, (dev.protocol == "TCP" ? 1 : 0), 0);
                }
            } else if (id == ID_BTN_ACTION && code == BN_CLICKED) {
                if (pThis->m_isSenderMode) {
                    if (pThis->m_isStreaming) {
                        pThis->StopSender();
                    } else {
                        pThis->StartSender();
                    }
                } else {
                    if (pThis->m_isReceiving) {
                        pThis->StopReceiver();
                    } else {
                        pThis->StartReceiver();
                    }
                }
            }
            break;
        }

        case WM_USER_DEVICES_UPDATED: {
            pThis->RefreshDiscoveredDevices();
            break;
        }

        case WM_USER_ADAPT_WINDOW: {
            int w = static_cast<int>(wParam);
            int h = static_cast<int>(lParam);
            pThis->AutoAdaptReceiverWindow(w, h);
            break;
        }

        case WM_TIMER: {
            if (wParam == ID_TIMER_STATS) {
                if (pThis->m_isStreaming) {
                    if (pThis->m_udpStreamer) {
                        pThis->m_statRttMs = pThis->m_udpStreamer->GetRttMs();
                        uint64_t bytes = pThis->m_udpStreamer->GetAndResetSentBytes();
                        pThis->m_statBitrateKbps = static_cast<int>((bytes * 8) / 1000);
                    } else if (pThis->m_tcpStreamer) {
                        uint64_t bytes = pThis->m_tcpStreamer->GetAndResetSentBytes();
                        pThis->m_statBitrateKbps = static_cast<int>((bytes * 8) / 1000);
                        pThis->m_statRttMs = 0;
                    }
                    pThis->m_statFps = pThis->m_fpsCounter.exchange(0);
                    pThis->UpdateStatusText();
                } else if (pThis->m_isReceiving) {
                    if (pThis->m_udpReceiver) {
                        uint64_t bytes = pThis->m_udpReceiver->GetAndResetReceivedBytes();
                        pThis->m_statBitrateKbps = static_cast<int>((bytes * 8) / 1000);
                    } else if (pThis->m_tcpReceiver) {
                        uint64_t bytes = pThis->m_tcpReceiver->GetAndResetReceivedBytes();
                        pThis->m_statBitrateKbps = static_cast<int>((bytes * 8) / 1000);
                        pThis->m_statRttMs = 0;
                    }
                    pThis->m_statFps = pThis->m_fpsCounter.exchange(0);
                    pThis->UpdateStatusText();
                } else {
                    pThis->m_statFps = 0;
                    pThis->m_statBitrateKbps = 0;
                    pThis->m_statRttMs = 0;
                    pThis->m_fpsCounter = 0;
                    pThis->UpdateStatusText();
                }
            }
            break;
        }

        case WM_DESTROY: {
            KillTimer(hwnd, ID_TIMER_STATS);
            PostQuitMessage(0);
            return 0;
        }
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK MainWindow::ReceiverWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    MainWindow* pThis = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCT*>(lParam);
        pThis = reinterpret_cast<MainWindow*>(cs->lpCreateParams);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pThis));
    } else {
        pThis = reinterpret_cast<MainWindow*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    }

    switch (msg) {
        case WM_SIZE: {
            if (pThis && pThis->m_d3dRenderer) {
                int w = LOWORD(lParam);
                int h = HIWORD(lParam);
                pThis->m_d3dRenderer->Resize(w, h);
            }
            break;
        }

        case WM_CLOSE: {
            if (pThis) {
                pThis->StopReceiver();
            }
            return 0;
        }
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}
