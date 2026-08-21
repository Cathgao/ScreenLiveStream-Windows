#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include "Protocol.h"
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>

class UdpStreamer {
public:
    UdpStreamer();
    ~UdpStreamer();

    bool Start(const std::string& targetIp, uint16_t targetPort);
    void Stop();
    bool IsRunning() const { return m_isRunning; }

    void SendFrame(
        const uint8_t* data,
        size_t size,
        int64_t timestampMs,
        bool isKeyframe,
        bool isCodecConfig,
        bool isHevc,
        bool isAudio = false
    );

    int GetRttMs() const { return m_lastRttMs; }

private:
    std::atomic<bool> m_isRunning{ false };
    SOCKET m_sock = INVALID_SOCKET;
    sockaddr_in m_targetAddr = {};
    std::mutex m_sendMutex;
    uint32_t m_videoSeq = 0;
    uint32_t m_audioSeq = 0;
    uint32_t m_pingSeq = 0;
    std::atomic<int> m_lastRttMs{ 0 };

    std::thread m_recvThread;
    void RecvThreadProc();
};
