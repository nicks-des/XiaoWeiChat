/**
 * @file Packet.h
 * @brief 传输帧定义与编解码：12 字节大端帧头 + Protobuf body（docs/02 §1）。
 *
 * 帧格式：totalLength(4) | msgId(2) | version(1) | flags(1) | pktSeq(4) | body
 */
#pragma once

#include <cstdint>
#include <string>

namespace lingxi::net {

/** 帧头固定长度（字节） */
constexpr size_t kHeaderSize = 12;

/** 协议版本号（docs/02 §7 版本兼容策略） */
constexpr uint8_t kVersion = 1;

/** 单帧上限 16MB，超出视为恶意/脏数据并断开 */
constexpr uint32_t kMaxPacketSize = 16 * 1024 * 1024;

/**
 * @brief 帧标志位（按位或组合）。
 */
enum PacketFlag : uint8_t {
    kFlagNone = 0x00,       ///< 无标志
    kFlagCompressed = 0x01, ///< body 已压缩（预留）
    kFlagPush = 0x02,       ///< 服务端主动推送帧（RPC Push 方向指示）
    kFlagEncrypted = 0x04,  ///< body 已加密（预留）
};

/**
 * @brief 解码后的帧头。
 */
struct PacketHeader {
    uint32_t totalLength = 0; ///< 整帧长度（含帧头自身）
    uint16_t msgId = 0;       ///< 消息/信令 ID（docs/02 §2 分配表）
    uint8_t version = kVersion;
    uint8_t flags = kFlagNone;
    uint32_t pktSeq = 0;      ///< 包序号：请求-响应匹配与日志追踪（非业务 seq）
};

/**
 * @brief 完整解码帧。
 */
struct DecodedPacket {
    PacketHeader header;   ///< 帧头
    std::string body;      ///< body 原始字节（Protobuf 编码内容）
};

/**
 * @brief 将整帧编码为字节串。
 * @param msgId  消息/信令 ID
 * @param pktSeq 包序号
 * @param body   Protobuf 编码后的消息体
 * @param flags  标志位
 * @return std::string 完整帧字节串（可直接 write 到 socket）
 */
std::string encodePacket(uint16_t msgId, uint32_t pktSeq, const std::string& body,
                         uint8_t flags = kFlagNone);

/**
 * @brief 从缓冲区起始处解析帧头（缓冲区须至少 kHeaderSize 字节）。
 * @param data  缓冲区指针
 * @param len   缓冲区长度（须 >= kHeaderSize）
 * @param out   输出帧头
 * @return bool 解析成功（长度/版本合法）为 false 表示脏数据
 */
bool parseHeader(const char* data, size_t len, PacketHeader& out);

} // namespace lingxi::net
