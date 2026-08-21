#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <string>
#include <vector>
#include <deque>
#include <memory>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <thread>

#include "../common/D3D11Helper.h"
#include "../capture/WgcCapture.h"
#include "../capture/WasapiCapture.h"
#include "../encoder/WmfVideoEncoder.h"
#include "../encoder/WmfAudioEncoder.h"
#include "../decoder/FfmpegVideoDecoder.h"
#include "../decoder/WmfAudioDecoder.h"
#include "../render/D3D11Renderer.h"
#include "../render/WasapiPlayer.h"
#include "../net/LanDiscovery.h"
#include "../net/TcpStreamer.h"
#include "../net/TcpReceiver.h"
#include "../net/UdpStreamer.h"
#include "../net/UdpReceiver.h"

#include <gdiplus.h>

class MainWindow {
public:
    MainWindow();
    ~MainWindow();

    bool Create(HINSTANCE hInstance, int nCmdShow);
    int Run();

private:
    HINSTANCE m_hInstance = nullptr;
    HWND m_hwnd = nullptr;
    HWND m_hwndReceiverView = nullptr;
    ULONG_PTR m_gdiplusToken = 0;

    D3D11Helper::DeviceResources m_d3dResources;

    // Sender Pipeline Components
    std::unique_ptr<WgcCapture> m_wgcCapture;
    std::unique_ptr<WasapiCapture> m_wasapiCapture;
    std::unique_ptr<WmfVideoEncoder> m_videoEncoder;
    std::unique_ptr<WmfAudioEncoder> m_audioEncoder;
    std::unique_ptr<TcpStreamer> m_tcpStreamer;
    std::unique_ptr<UdpStreamer> m_udpStreamer;

    // Receiver Pipeline Components
    std::unique_ptr<FfmpegVideoDecoder> m_videoDecoder;
    std::unique_ptr<WmfAudioDecoder> m_audioDecoder;
    std::unique_ptr<D3D11Renderer> m_d3dRenderer;
    std::unique_ptr<WasapiPlayer> m_wasapiPlayer;
    std::unique_ptr<TcpReceiver> m_tcpReceiver;
    std::unique_ptr<UdpReceiver> m_udpReceiver;

    // Asynchronous Video Decoder Worker Thread & Queue
    struct ReceiverVideoPacket {
        std::vector<uint8_t> data;
        int64_t timestampMs = 0;
        bool isKeyframe = false;
        bool isCodecConfig = false;
        bool isHevc = false;
    };

    std::thread m_receiverDecodeThread;
    std::mutex m_frameQueueMutex;
    std::condition_variable m_frameQueueCv;
    std::deque<ReceiverVideoPacket> m_frameQueue;
    std::atomic<bool> m_isDecoding{ false };

    void ReceiverDecodeLoop();

    // LAN Discovery
    std::unique_ptr<LanDiscovery> m_lanDiscovery;
    std::mutex m_devicesMutex;
    std::vector<DiscoveredDevice> m_cachedDevices;
    std::vector<CaptureTarget> m_captureTargets;

    // State
    std::atomic<bool> m_isStreaming{ false };
    std::atomic<bool> m_isReceiving{ false };
    bool m_isSenderMode = true;

    // Stats & Adaptation
    std::atomic<int> m_fpsCounter{ 0 };
    std::atomic<int> m_statFps{ 0 };
    std::atomic<int> m_statBitrateKbps{ 0 };
    std::atomic<int> m_statRttMs{ 0 };
    std::atomic<int> m_statLossBps{ 0 };
    std::atomic<int> m_statWidth{ 0 };
    std::atomic<int> m_statHeight{ 0 };
    std::atomic<int> m_postedAdaptW{ 0 };
    std::atomic<int> m_postedAdaptH{ 0 };
    int m_appliedAdaptedW = 0;
    int m_appliedAdaptedH = 0;

    // UI Control Handles
    HWND m_btnModeSender = nullptr;
    HWND m_btnModeReceiver = nullptr;
    HWND m_comboTarget = nullptr;
    HWND m_btnRefreshTargets = nullptr;
    HWND m_comboDevices = nullptr;
    HWND m_btnRefreshDevices = nullptr;
    HWND m_editIp = nullptr;
    HWND m_editPort = nullptr;
    HWND m_comboCodec = nullptr;
    HWND m_comboBitrate = nullptr;
    HWND m_comboFps = nullptr;
    HWND m_comboProtocol = nullptr;
    HWND m_chkCursor = nullptr;
    HWND m_chkAudio = nullptr;
    HWND m_btnAction = nullptr;

    // UI Label Handles
    HWND m_lblTarget = nullptr;
    HWND m_lblDevices = nullptr;
    HWND m_lblIp = nullptr;
    HWND m_lblPort = nullptr;
    HWND m_lblCodec = nullptr;
    HWND m_lblBitrate = nullptr;
    HWND m_lblFps = nullptr;
    HWND m_lblProtocol = nullptr;

    // UI Theme Resources
    HFONT m_hFontTitle = nullptr;
    HFONT m_hFontHeader = nullptr;
    HFONT m_hFontNormal = nullptr;
    HFONT m_hFontBold = nullptr;
    HFONT m_hFontSmall = nullptr;
    HBRUSH m_hBrushBg = nullptr;
    HBRUSH m_hBrushCard = nullptr;
    HBRUSH m_hBrushInput = nullptr;

    void InitThemeResources();
    void CleanupThemeResources();
    void InitControls();
    void RefreshCaptureTargets();
    void RefreshDiscoveredDevices();
    void UpdateUiMode();
    void UpdateStatusText();

    bool StartSender();
    void StopSender();

    bool StartReceiver();
    void StopReceiver();

    void CreateReceiverWindow();
    void DestroyReceiverWindow();
    void AutoAdaptReceiverWindow(int videoW, int videoH);

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK ReceiverWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
};

