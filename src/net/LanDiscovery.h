#pragma once
#include <string>
#include <vector>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>
#include <cstdint>

struct DiscoveredDevice {
    std::string ip;
    uint16_t port = 8888;
    std::string deviceName;
    std::string protocol = "UDP"; // "UDP" or "TCP"
    int64_t lastSeenMs = 0;
};

class LanDiscovery {
public:
    using DeviceListCallback = std::function<void(const std::vector<DiscoveredDevice>& devices)>;

    LanDiscovery();
    ~LanDiscovery();

    // Start scanner (Sender mode: finds receivers on LAN)
    void StartScanning();
    void StopScanning();
    void Rescan();

    // Start announcer (Receiver mode: announces itself to senders)
    void StartAnnouncing(uint16_t streamPort, const std::string& protocol, const std::string& deviceName);
    void StopAnnouncing();

    void SetOnDevicesUpdated(DeviceListCallback cb) { m_onDevicesUpdated = cb; }

private:
    std::atomic<bool> m_isScanning{ false };
    std::atomic<bool> m_isAnnouncing{ false };
    std::atomic<bool> m_forceRescan{ false };
    std::thread m_scanThread;
    std::thread m_announceThread;

    std::mutex m_devicesMutex;
    std::vector<DiscoveredDevice> m_devices;
    DeviceListCallback m_onDevicesUpdated;

    uint16_t m_streamPort = 8888;
    std::string m_protocol = "UDP";
    std::string m_deviceName = "Windows-PC";

    void ScanThreadProc();
    void AnnounceThreadProc();
    void UpdateOrAddDevice(const std::string& ip, uint16_t port, const std::string& name, const std::string& proto);
};
