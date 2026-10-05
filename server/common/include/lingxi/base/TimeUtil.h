/**
 * @file TimeUtil.h
 * @brief 时间工具：毫秒时间戳与格式化输出（仅头文件实现）。
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>

namespace lingxi {

/**
 * @brief 时间工具集（全部为静态方法）。
 */
class TimeUtil {
public:
    /**
     * @brief 当前 Unix 时间戳（毫秒）。
     * @return int64_t 毫秒时间戳
     */
    static int64_t nowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    /**
     * @brief 当前 Unix 时间戳（秒）。
     * @return int64_t 秒时间戳
     */
    static int64_t nowSec() {
        return std::chrono::duration_cast<std::chrono::seconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    /**
     * @brief 将毫秒时间戳格式化为本地时间字符串。
     * @param timeMs   毫秒时间戳（0 表示当前时间）
     * @param withMs   是否附加毫秒部分
     * @return std::string 形如 "2026-10-05 14:30:00.123"
     */
    static std::string formatMs(int64_t timeMs = 0, bool withMs = false) {
        if (timeMs <= 0) {
            timeMs = nowMs();
        }
        std::time_t seconds = static_cast<std::time_t>(timeMs / 1000);
        std::tm localTm{};
#ifdef _WIN32
        localtime_s(&localTm, &seconds);
#else
        localtime_r(&seconds, &localTm);
#endif
        char buffer[40] = {0};
        if (withMs) {
            std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                          localTm.tm_year + 1900, localTm.tm_mon + 1, localTm.tm_mday,
                          localTm.tm_hour, localTm.tm_min, localTm.tm_sec,
                          static_cast<int>(timeMs % 1000));
        } else {
            std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d",
                          localTm.tm_year + 1900, localTm.tm_mon + 1, localTm.tm_mday,
                          localTm.tm_hour, localTm.tm_min, localTm.tm_sec);
        }
        return buffer;
    }
};

} // namespace lingxi
