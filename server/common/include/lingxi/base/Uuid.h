/**
 * @file Uuid.h
 * @brief UUID v4 生成器（仅头文件实现，用于 clientMsgId 等幂等标识）。
 */
#pragma once

#include <cstdint>
#include <cstdio>
#include <random>
#include <string>

namespace lingxi {

/**
 * @brief UUID 工具（非密码学安全，仅用于业务标识）。
 */
class Uuid {
public:
    /**
     * @brief 生成随机 UUID v4 字符串。
     * @return std::string 形如 "550e8400-e29b-41d4-a716-446655440000"
     */
    static std::string generate() {
        thread_local std::mt19937_64 generator{std::random_device{}()};

        uint64_t high = generator();
        uint64_t low = generator();

        // 按 RFC 4122 设置版本号(4)与变体位(10xx)
        high = (high & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
        low = (low & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;

        char buffer[40] = {0};
        std::snprintf(buffer, sizeof(buffer),
                      "%08x-%04x-%04x-%04x-%012llx",
                      static_cast<uint32_t>(high >> 32),
                      static_cast<uint32_t>((high >> 16) & 0xFFFF),
                      static_cast<uint32_t>(high & 0xFFFF),
                      static_cast<uint32_t>(low >> 48),
                      static_cast<unsigned long long>(low & 0xFFFFFFFFFFFFULL));
        return buffer;
    }
};

} // namespace lingxi
