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

class TcpStreamer {
public:
    TcpStreamer();
    ~TcpStreamer();

    bool Start(const std::string& targetIp, uint16_t targetPort);
    void Stop();
    bool IsConnected() const { return m_isConnected; }

    void SendFrame(
        const uint8_t* data,
        size_t size,
        int64_t timestampMs,
        bool isKeyframe,
        bool isCodecConfig,
        bool isHevc,
        bool isAudio = false
    );

private:
    std::atomic<bool> m_isConnected{ false };
    SOCKET m_sock = INVALID_SOCKET;
    std::mutex m_sendMutex;
    uint32_t m_videoSeq = 0;
    uint32_t m_audioSeq = 0;
};
