#pragma once
#include <cstdint>
#include <vector>
#include <string>
#include <optional>
#include <chrono>

namespace Protocol {

constexpr uint8_t MAGIC_0 = 0x51; // 'Q'
constexpr uint8_t MAGIC_1 = 0x43; // 'C'
constexpr uint8_t VERSION = 0x01;

constexpr size_t HEADER_SIZE = 22;
constexpr size_t MAX_PAYLOAD_SIZE = 1300;

constexpr uint8_t FLAG_KEYFRAME     = 0x01;
constexpr uint8_t FLAG_CODEC_CONFIG = 0x02;
constexpr uint8_t FLAG_CODEC_HEVC   = 0x04;
constexpr uint8_t FLAG_AUDIO        = 0x08;
constexpr uint8_t FLAG_PING         = 0x10;
constexpr uint8_t FLAG_PING_REPLY   = 0x20;
constexpr uint8_t FLAG_PING_STATS   = 0x40;
constexpr uint8_t FLAG_STREAM_STOP  = 0x82;

constexpr uint16_t DEFAULT_STREAM_PORT = 8888;
constexpr uint16_t DISCOVERY_PORT = 9998;
const std::string DISCOVERY_PING = "QUEST_CAST_DISCOVER_PING";
const std::string DISCOVERY_ACK_PREFIX = "QUEST_CAST_RECEIVER_ACK:";
const std::string DISCOVERY_BEACON_PREFIX = "QUEST_CAST_BEACON:";

struct PacketHeader {
    uint8_t magic0 = MAGIC_0;
    uint8_t magic1 = MAGIC_1;
    uint8_t version = VERSION;
    uint8_t flags = 0;
    uint32_t frameSeq = 0;
    uint64_t timestampMs = 0;
    uint16_t packetIndex = 0;
    uint16_t totalPackets = 0;
    uint16_t payloadSize = 0;

    bool isKeyframe() const { return (flags & FLAG_KEYFRAME) != 0; }
    bool isCodecConfig() const { return (flags & FLAG_CODEC_CONFIG) != 0; }
    bool isHevc() const { return (flags & FLAG_CODEC_HEVC) != 0; }
    bool isAudio() const { return (flags & FLAG_AUDIO) != 0; }
    bool isPing() const { return (flags & FLAG_PING) != 0; }
    bool isPingReply() const { return (flags & FLAG_PING_REPLY) != 0; }
    bool isPingStats() const { return (flags & FLAG_PING_STATS) != 0; }
    bool isStreamStop() const { return flags == FLAG_STREAM_STOP; }
};

struct ParsedPacket {
    PacketHeader header;
    const uint8_t* payload = nullptr;
    size_t payloadSize = 0;
};

// Packet builders and parsers
std::vector<uint8_t> BuildPacket(
    const uint8_t* payload,
    size_t payloadSize,
    uint32_t frameSeq,
    uint64_t timestampMs,
    uint16_t packetIndex,
    uint16_t totalPackets,
    bool isKeyframe,
    bool isCodecConfig,
    bool isHevc,
    bool isAudio = false
);

std::vector<uint8_t> BuildStreamStopPacket();
bool IsStreamStopPacket(const uint8_t* data, size_t length);
std::vector<uint8_t> BuildPingPacket(uint32_t probeSeq, int64_t sendTimeNanos);
std::vector<uint8_t> BuildPingReplyPacket(uint32_t probeSeq, int64_t originalSendTimeNanos);
std::vector<uint8_t> BuildPingStatsPacket(int32_t rttMs, int32_t lossPercentX100);

std::optional<ParsedPacket> ParsePacket(const uint8_t* data, size_t length);

int64_t GetCurrentNanos();
int64_t GetCurrentMillis();

} // namespace Protocol
