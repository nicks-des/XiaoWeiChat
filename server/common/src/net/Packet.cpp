/**
 * @file Packet.cpp
 * @brief 传输帧编解码实现（大端序手工编解码，保证跨平台一致性）。
 */
#include "lingxi/net/Packet.h"

namespace lingxi::net {

namespace {

/**
 * @brief 无符号整数大端序写入。
 */
template <typename T>
void writeBigEndian(std::string& out, T value) {
    for (int shift = static_cast<int>(sizeof(T)) * 8 - 8; shift >= 0; shift -= 8) {
        out.push_back(static_cast<char>((value >> shift) & 0xFF));
    }
}

/**
 * @brief 无符号整数大端序读取。
 */
template <typename T>
T readBigEndian(const char* data) {
    T value = 0;
    for (size_t i = 0; i < sizeof(T); ++i) {
        value = (value << 8) | static_cast<uint8_t>(data[i]);
    }
    return value;
}

} // namespace

std::string encodePacket(uint16_t msgId, uint32_t pktSeq, const std::string& body,
                         uint8_t flags) {
    const uint32_t totalLength = static_cast<uint32_t>(kHeaderSize + body.size());
    std::string frame;
    frame.reserve(totalLength);

    writeBigEndian<uint32_t>(frame, totalLength);
    writeBigEndian<uint16_t>(frame, msgId);
    frame.push_back(static_cast<char>(kVersion));
    frame.push_back(static_cast<char>(flags));
    writeBigEndian<uint32_t>(frame, pktSeq);
    frame.append(body);
    return frame;
}

bool parseHeader(const char* data, size_t len, PacketHeader& out) {
    if (len < kHeaderSize) {
        return false;
    }
    out.totalLength = readBigEndian<uint32_t>(data);
    out.msgId = static_cast<uint16_t>(readBigEndian<uint16_t>(data + 4));
    out.version = static_cast<uint8_t>(data[6]);
    out.flags = static_cast<uint8_t>(data[7]);
    out.pktSeq = readBigEndian<uint32_t>(data + 8);

    // 非法长度直接判脏：过小（不足头）或超过上限
    if (out.totalLength < kHeaderSize || out.totalLength > kMaxPacketSize) {
        return false;
    }
    return true;
}

} // namespace lingxi::net
