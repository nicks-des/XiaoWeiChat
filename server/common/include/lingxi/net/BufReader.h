/**
 * @file BufReader.h
 * @brief TCP 流缓冲读取器：解决粘包/半包问题，从字节流中提取完整帧（docs/02 §1）。
 *
 * 典型用法（socket 可读回调中）：
 * @code
 *   mBufReader.feed(data, len);
 *   net::DecodedPacket packet;
 *   while (mBufReader.tryExtractPacket(packet)) { dispatch(packet); }
 * @endcode
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "lingxi/net/Packet.h"

namespace lingxi::net {

/**
 * @brief 流式帧提取器（非线程安全：须与 socket 读回调在同一线程/strand 内使用）。
 */
class BufReader {
public:
    /**
     * @brief 追加一段新到达的字节流。
     * @param data 数据指针
     * @param len  长度（字节）
     */
    void feed(const char* data, size_t len);

    /**
     * @brief 尝试从缓冲区提取一个完整帧。
     * @param out 输出帧
     * @return bool 提取成功；false 表示剩余数据不足一帧（半包，等待下次 feed）
     *
     * 遇到非法帧头（长度越界/版本不符）时丢弃整个缓冲区并返回 false，由上层断连处理。
     */
    bool tryExtractPacket(DecodedPacket& out);

    /**
     * @brief 当前缓冲区未消费字节数。
     */
    size_t size() const { return m_buffer.size() - m_readPos; }

    /**
     * @brief 是否还有未消费数据。
     */
    bool empty() const { return size() == 0; }

    /**
     * @brief 清空缓冲区。
     */
    void clear();

private:
    /**
     * @brief 压缩缓冲区：当已消费部分超过一半时前移回收内存。
     */
    void compactIfNeeded();

    std::string m_buffer;   ///< 累积缓冲
    size_t m_readPos = 0;   ///< 已消费偏移（避免频繁 memmove）
};

} // namespace lingxi::net
