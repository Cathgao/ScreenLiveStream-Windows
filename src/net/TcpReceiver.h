#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include "Protocol.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <thread>
#include <atomic>
#include <functional>

class TcpReceiver {
public:
    using VideoFrameCallback = std::function<void(const uint8_t* data, size_t size, int64_t timestampMs, bool isKeyframe, bool isCodecConfig, bool isHevc)>;
    using AudioFrameCallback = std::function<void(const uint8_t* data, size_t size, int64_t timestampMs)>;

    TcpReceiver();
    ~TcpReceiver();

    bool Start(uint16_t listenPort = 8888);
    void Stop();
    bool IsRunning() const { return m_isRunning; }

    void SetVideoCallback(VideoFrameCallback cb) { m_videoCallback = cb; }
    void SetAudioCallback(AudioFrameCallback cb) { m_audioCallback = cb; }

    uint64_t GetAndResetReceivedBytes() {
        return m_receivedBytes.exchange(0);
    }

private:
    std::atomic<bool> m_isRunning{ false };
    uint16_t m_listenPort = 8888;
    SOCKET m_listenSock = INVALID_SOCKET;
    SOCKET m_clientSock = INVALID_SOCKET;
    std::thread m_listenThread;
    std::atomic<uint64_t> m_receivedBytes{ 0 };

    VideoFrameCallback m_videoCallback;
    AudioFrameCallback m_audioCallback;

    void ListenThreadProc();
    void HandleClient(SOCKET clientSock);
};
