#include "LanDiscovery.h"
#include "Protocol.h"
#include "Logger.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <sstream>
#include <algorithm>

#pragma comment(lib, "iphlpapi.lib")

struct NetInterfaceInfo {
    std::string ip;
    std::string broadcastIp;
};

static std::vector<NetInterfaceInfo> GetLocalInterfaces() {
    std::vector<NetInterfaceInfo> interfaces;
    ULONG outBufLen = 15000;
    std::vector<BYTE> buffer(outBufLen);
    PIP_ADAPTER_ADDRESSES pAddresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());

    ULONG flags = GAA_FLAG_INCLUDE_PREFIX | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    DWORD dwRetVal = GetAdaptersAddresses(AF_INET, flags, nullptr, pAddresses, &outBufLen);
    if (dwRetVal == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(outBufLen);
        pAddresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
        dwRetVal = GetAdaptersAddresses(AF_INET, flags, nullptr, pAddresses, &outBufLen);
    }

    if (dwRetVal != NO_ERROR) {
        return interfaces;
    }

    for (PIP_ADAPTER_ADDRESSES pCurr = pAddresses; pCurr != nullptr; pCurr = pCurr->Next) {
        if (pCurr->OperStatus != IfOperStatusUp) continue;
        if (pCurr->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;

        for (PIP_ADAPTER_UNICAST_ADDRESS pUnicast = pCurr->FirstUnicastAddress; pUnicast != nullptr; pUnicast = pUnicast->Next) {
            if (pUnicast->Address.lpSockaddr->sa_family == AF_INET) {
                auto* saIn = reinterpret_cast<sockaddr_in*>(pUnicast->Address.lpSockaddr);
                uint32_t ip = ntohl(saIn->sin_addr.s_addr);
                UINT8 prefixLen = pUnicast->OnLinkPrefixLength;
                if (prefixLen == 0 || prefixLen > 32) prefixLen = 24;

                uint32_t mask = (prefixLen == 0) ? 0 : (~0u << (32 - prefixLen));
                uint32_t bcast = (ip & mask) | (~mask);

                char ipStr[INET_ADDRSTRLEN] = {};
                inet_ntop(AF_INET, &saIn->sin_addr, ipStr, sizeof(ipStr));

                struct in_addr bcastAddr;
                bcastAddr.s_addr = htonl(bcast);
                char bcastStr[INET_ADDRSTRLEN] = {};
                inet_ntop(AF_INET, &bcastAddr, bcastStr, sizeof(bcastStr));

                NetInterfaceInfo info;
                info.ip = ipStr;
                info.broadcastIp = bcastStr;
                interfaces.push_back(info);
            }
        }
    }

    return interfaces;
}

static inline std::string TrimString(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, (last - first + 1));
}

LanDiscovery::LanDiscovery() {}

LanDiscovery::~LanDiscovery() {
    StopScanning();
    StopAnnouncing();
}

void LanDiscovery::StartScanning() {
    StopScanning();
    m_isScanning = true;
    m_forceRescan = true;
    m_scanThread = std::thread(&LanDiscovery::ScanThreadProc, this);
    Logger::I("LanDiscovery", "Started LAN Discovery scanner.");
}

void LanDiscovery::StopScanning() {
    if (m_isScanning.exchange(false)) {
        if (m_scanThread.joinable()) {
            m_scanThread.join();
        }
    }
}

void LanDiscovery::Rescan() {
    {
        std::lock_guard<std::mutex> lock(m_devicesMutex);
        m_devices.clear();
    }
    m_forceRescan = true;
}

void LanDiscovery::StartAnnouncing(uint16_t streamPort, const std::string& protocol, const std::string& deviceName) {
    StopAnnouncing();
    m_streamPort = streamPort;
    m_protocol = protocol;
    m_deviceName = deviceName;
    m_isAnnouncing = true;
    m_announceThread = std::thread(&LanDiscovery::AnnounceThreadProc, this);
    Logger::I("LanDiscovery", "Started LAN Discovery announcer (" + m_deviceName + ", port " + std::to_string(m_streamPort) + ", " + m_protocol + ")");
}

void LanDiscovery::StopAnnouncing() {
    if (m_isAnnouncing.exchange(false)) {
        if (m_announceThread.joinable()) {
            m_announceThread.join();
        }
    }
}

void LanDiscovery::UpdateOrAddDevice(const std::string& ip, uint16_t port, const std::string& name, const std::string& proto) {
    std::vector<DiscoveredDevice> currentList;
    {
        std::lock_guard<std::mutex> lock(m_devicesMutex);
        int64_t now = Protocol::GetCurrentMillis();

        bool found = false;
        for (auto& d : m_devices) {
            if (d.ip == ip) {
                d.port = port;
                d.deviceName = name;
                d.protocol = proto;
                d.lastSeenMs = now;
                found = true;
                break;
            }
        }

        if (!found) {
            DiscoveredDevice dev;
            dev.ip = ip;
            dev.port = port;
            dev.deviceName = name;
            dev.protocol = proto;
            dev.lastSeenMs = now;
            m_devices.push_back(dev);
            Logger::I("LanDiscovery", "Discovered new device: " + name + " (" + ip + ":" + std::to_string(port) + ", " + proto + ")");
        }

        // Filter out expired devices (> 10s)
        m_devices.erase(
            std::remove_if(m_devices.begin(), m_devices.end(), [now](const DiscoveredDevice& d) {
                return (now - d.lastSeenMs) > 10000;
            }),
            m_devices.end()
        );

        currentList = m_devices;
    }

    if (m_onDevicesUpdated) {
        m_onDevicesUpdated(currentList);
    }
}

void LanDiscovery::ScanThreadProc() {
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) return;

    BOOL broadcast = TRUE;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, (const char*)&broadcast, sizeof(broadcast));

    BOOL reuse = TRUE;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));

    DWORD timeout = 400; // 400ms timeout
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));

    // Try binding to DISCOVERY_PORT with SO_REUSEADDR so we receive both direct ACKs and broadcast beacons
    sockaddr_in bindAddr = {};
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_port = htons(Protocol::DISCOVERY_PORT);
    bindAddr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sock, (sockaddr*)&bindAddr, sizeof(bindAddr)) == SOCKET_ERROR) {
        // Fallback to ephemeral port if 9998 is unavailable
        bindAddr.sin_port = htons(0);
        bind(sock, (sockaddr*)&bindAddr, sizeof(bindAddr));
    }

    sockaddr_in globalBroadcastAddr = {};
    globalBroadcastAddr.sin_family = AF_INET;
    globalBroadcastAddr.sin_port = htons(Protocol::DISCOVERY_PORT);
    globalBroadcastAddr.sin_addr.s_addr = htonl(INADDR_BROADCAST);

    auto lastPingTime = std::chrono::steady_clock::now() - std::chrono::seconds(5);
    char buf[2048];

    while (m_isScanning) {
        auto now = std::chrono::steady_clock::now();
        bool shouldPing = m_forceRescan.exchange(false) ||
                          std::chrono::duration_cast<std::chrono::milliseconds>(now - lastPingTime).count() >= 1500;

        if (shouldPing) {
            lastPingTime = now;
            auto localIfs = GetLocalInterfaces();

            // 1. Send to global broadcast 255.255.255.255:9998
            sendto(sock, Protocol::DISCOVERY_PING.c_str(), (int)Protocol::DISCOVERY_PING.length(), 0,
                   (sockaddr*)&globalBroadcastAddr, sizeof(globalBroadcastAddr));

            // 2. Send to each active interface subnet broadcast (e.g. 192.168.1.255)
            for (const auto& iface : localIfs) {
                if (!iface.broadcastIp.empty()) {
                    sockaddr_in subnetBcast = {};
                    subnetBcast.sin_family = AF_INET;
                    subnetBcast.sin_port = htons(Protocol::DISCOVERY_PORT);
                    inet_pton(AF_INET, iface.broadcastIp.c_str(), &subnetBcast.sin_addr);
                    sendto(sock, Protocol::DISCOVERY_PING.c_str(), (int)Protocol::DISCOVERY_PING.length(), 0,
                           (sockaddr*)&subnetBcast, sizeof(subnetBcast));
                }
            }
        }

        sockaddr_in fromAddr = {};
        int fromLen = sizeof(fromAddr);
        int bytes = recvfrom(sock, buf, sizeof(buf) - 1, 0, (sockaddr*)&fromAddr, &fromLen);

        if (bytes > 0) {
            buf[bytes] = '\0';
            std::string msg(buf, bytes);

            char ipStr[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &fromAddr.sin_addr, ipStr, sizeof(ipStr));

            // Filter out self-discovery packets
            auto localIfs = GetLocalInterfaces();
            bool isSelf = false;
            for (const auto& iface : localIfs) {
                if (iface.ip == ipStr) {
                    isSelf = true;
                    break;
                }
            }
            if (isSelf) continue;

            std::string prefixAck = Protocol::DISCOVERY_ACK_PREFIX;
            std::string prefixBeacon = Protocol::DISCOVERY_BEACON_PREFIX;

            std::string content;
            if (msg.rfind(prefixAck, 0) == 0) {
                content = msg.substr(prefixAck.length());
            } else if (msg.rfind(prefixBeacon, 0) == 0) {
                content = msg.substr(prefixBeacon.length());
            }

            if (!content.empty()) {
                try {
                    std::stringstream ss(content);
                    std::string devName, portStr, proto;
                    if (std::getline(ss, devName, ':') && std::getline(ss, portStr, ':')) {
                        std::getline(ss, proto); // Optional 3rd parameter
                        devName = TrimString(devName);
                        portStr = TrimString(portStr);
                        proto = TrimString(proto);

                        if (proto.empty()) {
                            proto = "UDP";
                        }
                        std::transform(proto.begin(), proto.end(), proto.begin(), [](unsigned char c) {
                            return static_cast<char>(std::toupper(c));
                        });

                        if (!portStr.empty()) {
                            uint16_t port = static_cast<uint16_t>(std::stoi(portStr));
                            if (devName.empty()) devName = "Android Device";
                            UpdateOrAddDevice(ipStr, port, devName, proto);
                        }
                    }
                } catch (...) {}
            }
        }
    }

    closesocket(sock);
}

void LanDiscovery::AnnounceThreadProc() {
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) return;

    BOOL broadcast = TRUE;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, (const char*)&broadcast, sizeof(broadcast));

    BOOL reuse = TRUE;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));

    DWORD timeout = 500;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));

    sockaddr_in bindAddr = {};
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_port = htons(Protocol::DISCOVERY_PORT);
    bindAddr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sock, (sockaddr*)&bindAddr, sizeof(bindAddr)) == SOCKET_ERROR) {
        Logger::E("LanDiscovery", "Announcer failed to bind port " + std::to_string(Protocol::DISCOVERY_PORT));
        closesocket(sock);
        return;
    }

    sockaddr_in globalBroadcastAddr = {};
    globalBroadcastAddr.sin_family = AF_INET;
    globalBroadcastAddr.sin_port = htons(Protocol::DISCOVERY_PORT);
    globalBroadcastAddr.sin_addr.s_addr = htonl(INADDR_BROADCAST);

    auto lastBeaconTime = std::chrono::steady_clock::now() - std::chrono::seconds(5);
    char buf[2048];

    std::string ackMsg = Protocol::DISCOVERY_ACK_PREFIX + m_deviceName + ":" + std::to_string(m_streamPort) + ":" + m_protocol;
    std::string beaconMsg = Protocol::DISCOVERY_BEACON_PREFIX + m_deviceName + ":" + std::to_string(m_streamPort) + ":" + m_protocol;

    while (m_isAnnouncing) {
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastBeaconTime).count() >= 2000) {
            lastBeaconTime = now;
            auto localIfs = GetLocalInterfaces();

            // Send beacon to global broadcast and all subnet broadcasts
            sendto(sock, beaconMsg.c_str(), (int)beaconMsg.length(), 0, (sockaddr*)&globalBroadcastAddr, sizeof(globalBroadcastAddr));
            for (const auto& iface : localIfs) {
                if (!iface.broadcastIp.empty()) {
                    sockaddr_in subnetBcast = {};
                    subnetBcast.sin_family = AF_INET;
                    subnetBcast.sin_port = htons(Protocol::DISCOVERY_PORT);
                    inet_pton(AF_INET, iface.broadcastIp.c_str(), &subnetBcast.sin_addr);
                    sendto(sock, beaconMsg.c_str(), (int)beaconMsg.length(), 0, (sockaddr*)&subnetBcast, sizeof(subnetBcast));
                }
            }
        }

        sockaddr_in fromAddr = {};
        int fromLen = sizeof(fromAddr);
        int bytes = recvfrom(sock, buf, sizeof(buf) - 1, 0, (sockaddr*)&fromAddr, &fromLen);

        if (bytes > 0) {
            buf[bytes] = '\0';
            std::string msg(buf, bytes);
            if (msg == Protocol::DISCOVERY_PING) {
                // Reply with ACK directly to sender
                sendto(sock, ackMsg.c_str(), (int)ackMsg.length(), 0, (sockaddr*)&fromAddr, fromLen);
            }
        }
    }

    closesocket(sock);
}

