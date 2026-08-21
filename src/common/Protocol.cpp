#include "Protocol.h"
#include <cstring>
#include <winsock2.h>

namespace Protocol {

static inline void WriteBigEndian16(uint8_t* dst, uint16_t val) {
    dst[0] = static_cast<uint8_t>((val >> 8) & 0xFF);
    dst[1] = static_cast<uint8_t>(val & 0xFF);
}

static inline void WriteBigEndian32(uint8_t* dst, uint32_t val) {
    dst[0] = static_cast<uint8_t>((val >> 24) & 0xFF);
    dst[1] = static_cast<uint8_t>((val >> 16) & 0xFF);
    dst[2] = static_cast<uint8_t>((val >> 8) & 0xFF);
    dst[3] = static_cast<uint8_t>(val & 0xFF);
}

static inline void WriteBigEndian64(uint8_t* dst, uint64_t val) {
    dst[0] = static_cast<uint8_t>((val >> 56) & 0xFF);
    dst[1] = static_cast<uint8_t>((val >> 48) & 0xFF);
    dst[2] = static_cast<uint8_t>((val >> 40) & 0xFF);
    dst[3] = static_cast<uint8_t>((val >> 32) & 0xFF);
    dst[4] = static_cast<uint8_t>((val >> 24) & 0xFF);
    dst[5] = static_cast<uint8_t>((val >> 16) & 0xFF);
    dst[6] = static_cast<uint8_t>((val >> 8) & 0xFF);
    dst[7] = static_cast<uint8_t>(val & 0xFF);
}

static inline uint16_t ReadBigEndian16(const uint8_t* src) {
    return static_cast<uint16_t>((src[0] << 8) | src[1]);
}

static inline uint32_t ReadBigEndian32(const uint8_t* src) {
    return (static_cast<uint32_t>(src[0]) << 24) |
           (static_cast<uint32_t>(src[1]) << 16) |
           (static_cast<uint32_t>(src[2]) << 8)  |
           (static_cast<uint32_t>(src[3]));
}

static inline uint64_t ReadBigEndian64(const uint8_t* src) {
    return (static_cast<uint64_t>(src[0]) << 56) |
           (static_cast<uint64_t>(src[1]) << 48) |
           (static_cast<uint64_t>(src[2]) << 40) |
           (static_cast<uint64_t>(src[3]) << 32) |
           (static_cast<uint64_t>(src[4]) << 24) |
           (static_cast<uint64_t>(src[5]) << 16) |
           (static_cast<uint64_t>(src[6]) << 8)  |
           (static_cast<uint64_t>(src[7]));
}

int64_t GetCurrentNanos() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
}

int64_t GetCurrentMillis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
}

std::vector<uint8_t> BuildStreamStopPacket() {
    std::vector<uint8_t> packet(HEADER_SIZE, 0);
    packet[0] = MAGIC_0;
    packet[1] = MAGIC_1;
    packet[2] = VERSION;
    packet[3] = FLAG_STREAM_STOP;
    return packet;
}

bool IsStreamStopPacket(const uint8_t* data, size_t length) {
    if (length < HEADER_SIZE || !data) return false;
    if (data[0] != MAGIC_0 || data[1] != MAGIC_1) return false;
    return data[3] == FLAG_STREAM_STOP;
}

std::vector<uint8_t> BuildPingPacket(uint32_t probeSeq, int64_t sendTimeNanos) {
    std::vector<uint8_t> packet(HEADER_SIZE + 8, 0);
    packet[0] = MAGIC_0;
    packet[1] = MAGIC_1;
    packet[2] = VERSION;
    packet[3] = FLAG_PING;
    WriteBigEndian32(&packet[4], probeSeq);
    WriteBigEndian64(&packet[8], 0);
    WriteBigEndian16(&packet[16], 0);
    WriteBigEndian16(&packet[18], 1);
    WriteBigEndian16(&packet[20], 8);
    WriteBigEndian64(&packet[HEADER_SIZE], static_cast<uint64_t>(sendTimeNanos));
    return packet;
}

std::vector<uint8_t> BuildPingReplyPacket(uint32_t probeSeq, int64_t originalSendTimeNanos) {
    std::vector<uint8_t> packet(HEADER_SIZE + 8, 0);
    packet[0] = MAGIC_0;
    packet[1] = MAGIC_1;
    packet[2] = VERSION;
    packet[3] = FLAG_PING_REPLY;
    WriteBigEndian32(&packet[4], probeSeq);
    WriteBigEndian64(&packet[8], 0);
    WriteBigEndian16(&packet[16], 0);
    WriteBigEndian16(&packet[18], 1);
    WriteBigEndian16(&packet[20], 8);
    WriteBigEndian64(&packet[HEADER_SIZE], static_cast<uint64_t>(originalSendTimeNanos));
    return packet;
}

std::vector<uint8_t> BuildPingStatsPacket(int32_t rttMs, int32_t lossPercentX100) {
    std::vector<uint8_t> packet(HEADER_SIZE + 8, 0);
    packet[0] = MAGIC_0;
    packet[1] = MAGIC_1;
    packet[2] = VERSION;
    packet[3] = FLAG_PING_STATS;
    WriteBigEndian32(&packet[4], 0);
    WriteBigEndian64(&packet[8], 0);
    WriteBigEndian16(&packet[16], 0);
    WriteBigEndian16(&packet[18], 1);
    WriteBigEndian16(&packet[20], 8);
    WriteBigEndian32(&packet[HEADER_SIZE], static_cast<uint32_t>(rttMs));
    WriteBigEndian32(&packet[HEADER_SIZE + 4], static_cast<uint32_t>(lossPercentX100));
    return packet;
}

std::optional<ParsedPacket> ParsePacket(const uint8_t* data, size_t length) {
    if (length < HEADER_SIZE || !data) return std::nullopt;

    if (data[0] != MAGIC_0 || data[1] != MAGIC_1) {
        return std::nullopt;
    }

    ParsedPacket pkt;
    pkt.header.magic0 = data[0];
    pkt.header.magic1 = data[1];
    pkt.header.version = data[2];
    pkt.header.flags = data[3];
    pkt.header.frameSeq = ReadBigEndian32(&data[4]);
    pkt.header.timestampMs = ReadBigEndian64(&data[8]);
    pkt.header.packetIndex = ReadBigEndian16(&data[16]);
    pkt.header.totalPackets = ReadBigEndian16(&data[18]);
    pkt.header.payloadSize = ReadBigEndian16(&data[20]);

    if (length >= HEADER_SIZE + pkt.header.payloadSize) {
        pkt.payload = &data[HEADER_SIZE];
        pkt.payloadSize = pkt.header.payloadSize;
    } else {
        pkt.payload = &data[HEADER_SIZE];
        pkt.payloadSize = length - HEADER_SIZE;
    }

    return pkt;
}

} // namespace Protocol
