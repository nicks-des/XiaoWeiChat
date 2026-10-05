/**
 * @file RpcFrame.h
 * @brief 自研 RPC 帧头：复用 IM 传输帧，body 前置 9 字节 RPC 头（docs/01 §4.1）。
 *
 * RPC 帧结构：net::Packet(msgId=0xFF01) 的 body = RpcHead(9B) + protobuf payload。
 */
#pragma once

#include <cstdint>
#include <string>

namespace lingxi::rpc {

/** RPC 专用 IM 帧 msgId（所有 RPC 请求/响应/推送共用） */
constexpr uint16_t kMsgIdRpcFrame = 0xFF01;

/** RPC 头固定长度（字节） */
constexpr size_t kRpcHeadSize = 9;

/**
 * @brief RPC 头标志位。
 */
enum RpcFlag : uint8_t {
    kRpcFlagNone = 0x00,     ///< 请求
    kRpcFlagResponse = 0x01, ///< 响应（requestId 与请求一致）
    kRpcFlagPush = 0x02,     ///< 单向推送（无需响应）
};

/**
 * @brief 服务号分配（docs/01 §4.4）。
 */
enum ServiceId : uint16_t {
    kServiceStatus = 0x01, ///< StatusServer：注册/心跳/token/分配
    kServiceChat = 0x02,   ///< ChatServer：跨节点推送
    kServiceFile = 0x03,   ///< FileServer（预留）
    kServiceAi = 0x04,     ///< AIServer（预留）
};

/**
 * @brief RPC 头。
 */
struct RpcHead {
    uint16_t serviceId = 0; ///< 服务号（ServiceId）
    uint16_t methodId = 0;  ///< 方法号（各服务自定义）
    uint32_t requestId = 0; ///< 请求匹配序号（响应回填同值）
    uint8_t flags = kRpcFlagNone;
};

/**
 * @brief 编码完整 RPC 帧体（RpcHead + payload），调用方再用 encodePacket 封装传输帧。
 * @return std::string RPC 帧体（kRpcHeadSize + payload.size() 字节）
 */
std::string encodeRpcBody(uint16_t serviceId, uint16_t methodId, uint32_t requestId,
                          uint8_t flags, const std::string& payload);

/**
 * @brief 解码 RPC 帧体。
 * @param body    传输帧 body（须以 RpcHead 开头）
 * @param outHead 输出 RPC 头
 * @param outPayload 输出 protobuf payload
 * @return bool 合法为 true
 */
bool decodeRpcBody(const std::string& body, RpcHead& outHead, std::string& outPayload);

} // namespace lingxi::rpc
