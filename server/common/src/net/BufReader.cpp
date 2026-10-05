/**
 * @file BufReader.cpp
 * @brief TCP 流缓冲读取器实现。
 */
#include "lingxi/net/BufReader.h"

#include "lingxi/logging/Logger.h"

namespace lingxi::net {

void BufReader::feed(const char* data, size_t len) {
    if (data == nullptr || len == 0) {
        return;
    }
    m_buffer.append(data, len);
    compactIfNeeded();
}

bool BufReader::tryExtractPacket(DecodedPacket& out) {
    while (size() >= kHeaderSize) {
        const char* cursor = m_buffer.data() + m_readPos;

        PacketHeader header;
        if (!parseHeader(cursor, size(), header)) {
            // 脏数据（长度越界等）：丢弃全部缓冲，由上层触发断连重连
            LX_LOG_ERROR("BufReader: invalid header, totalLength={}, drop {} bytes",
                         header.totalLength, size());
            clear();
            return false;
        }

        if (size() < header.totalLength) {
            return false;  // 半包：等待更多数据
        }

        // 帧校验通过：拷贝 body 并前移读指针
        out.header = header;
        out.body.assign(cursor + kHeaderSize, header.totalLength - kHeaderSize);
        m_readPos += header.totalLength;
        compactIfNeeded();
        return true;
    }
    return false;
}

void BufReader::clear() {
    m_buffer.clear();
    m_readPos = 0;
}

void BufReader::compactIfNeeded() {
    if (m_readPos > kHeaderSize * 64 && m_readPos * 2 > m_buffer.size()) {
        m_buffer.erase(0, m_readPos);
        m_readPos = 0;
    }
}

} // namespace lingxi::net
