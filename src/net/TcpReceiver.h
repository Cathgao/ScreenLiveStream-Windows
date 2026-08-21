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

private:
    std::atomic<bool> m_isRunning{ false };
    uint16_t m_listenPort = 8888;
    SOCKET m_listenSock = INVALID_SOCKET;
    SOCKET m_clientSock = INVALID_SOCKET;
    std::thread m_listenThread;

    VideoFrameCallback m_videoCallback;
    AudioFrameCallback m_audioCallback;

    struct FrameAssembly {
        uint32_t frameSeq = 0;
        uint64_t timestampMs = 0;
        uint16_t totalPackets = 0;
        uint16_t receivedPackets = 0;
        bool isKeyframe = false;
        bool isCodecConfig = false;
        bool isHevc = false;
        bool isAudio = false;
        std::vector<std::vector<uint8_t>> chunks;
    };

    std::unordered_map<uint32_t, FrameAssembly> m_assemblies;

    void ListenThreadProc();
    void HandleClient(SOCKET clientSock);
    void ProcessParsedPacket(const Protocol::ParsedPacket& pkt);
};
