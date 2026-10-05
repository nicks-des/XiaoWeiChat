/**
 * @file RpcFrame.cpp
 * @brief RPC 帧头编解码实现（大端序）。
 */
#include "lingxi/rpc/RpcFrame.h"

namespace lingxi::rpc {

namespace {

/**
 * @brief 大端序写入/读取（与 net::Packet 同风格）。
 */
template <typename T>
void writeBigEndian(std::string& out, T value) {
    for (int shift = static_cast<int>(sizeof(T)) * 8 - 8; shift >= 0; shift -= 8) {
        out.push_back(static_cast<char>((value >> shift) & 0xFF));
    }
}

template <typename T>
T readBigEndian(const char* data) {
    T value = 0;
    for (size_t i = 0; i < sizeof(T); ++i) {
        value = (value << 8) | static_cast<uint8_t>(data[i]);
    }
    return value;
}

} // namespace

std::string encodeRpcBody(uint16_t serviceId, uint16_t methodId, uint32_t requestId,
                          uint8_t flags, const std::string& payload) {
    std::string body;
    body.reserve(kRpcHeadSize + payload.size());
    writeBigEndian<uint16_t>(body, serviceId);
    writeBigEndian<uint16_t>(body, methodId);
    writeBigEndian<uint32_t>(body, requestId);
    body.push_back(static_cast<char>(flags));
    body.append(payload);
    return body;
}

bool decodeRpcBody(const std::string& body, RpcHead& outHead, std::string& outPayload) {
    if (body.size() < kRpcHeadSize) {
        return false;
    }
    outHead.serviceId = static_cast<uint16_t>(readBigEndian<uint16_t>(body.data()));
    outHead.methodId = static_cast<uint16_t>(readBigEndian<uint16_t>(body.data() + 2));
    outHead.requestId = readBigEndian<uint32_t>(body.data() + 4);
    outHead.flags = static_cast<uint8_t>(body[8]);
    outPayload.assign(body.data() + kRpcHeadSize, body.size() - kRpcHeadSize);
    return true;
}

} // namespace lingxi::rpc
