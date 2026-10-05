/**
 * @file SnowflakeIdGenerator.h
 * @brief 雪花算法 ID 生成器：全局唯一、趋势递增（用于 uid/conv_id/msg_id/fid 等）。
 *
 * 位分配（64 位）：41 位毫秒时间戳 | 10 位机器号 | 12 位毫秒内序列。
 * 单机单实例 QPS 上限约 409.6 万，满足本项目全部 ID 场景。
 */
#pragma once

#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <thread>

#include "lingxi/base/TimeUtil.h"

namespace lingxi {

/**
 * @brief 雪花 ID 生成器（线程安全）。
 */
class SnowflakeIdGenerator {
public:
    /**
     * @param machineId 机器号 [0, 1023]，多进程部署时必须互不相同（来自配置）。
     */
    explicit SnowflakeIdGenerator(int machineId = 1) : m_machineId(machineId) {
        if (machineId < 0 || machineId >= (1 << kMachineBits)) {
            throw std::invalid_argument("machineId out of range [0, 1023]");
        }
        m_lastTimestamp = TimeUtil::nowMs();
    }

    /**
     * @brief 生成下一个全局唯一 ID。
     * @return int64_t 64 位 ID（趋势递增）
     * @throws std::runtime_error 时钟回拨超过容忍阈值（kMaxBackwardMs）
     */
    int64_t nextId() {
        std::lock_guard<std::mutex> lock(m_mutex);
        int64_t now = TimeUtil::nowMs();

        // 时钟回拨处理：小幅回拨自旋等待追平，大幅回拨拒绝服务并告警
        if (now < m_lastTimestamp) {
            int64_t backward = m_lastTimestamp - now;
            if (backward > kMaxBackwardMs) {
                throw std::runtime_error("clock moved backwards too much: " +
                                         std::to_string(backward) + "ms");
            }
            // 以最近一次合法时间戳为基准继续分配（保证单调）
            now = m_lastTimestamp;
        }

        if (now == m_lastTimestamp) {
            // 当前毫秒序列耗尽：自旋等待进入下一毫秒
            m_sequence = (m_sequence + 1) & kSequenceMask;
            if (m_sequence == 0) {
                while ((now = TimeUtil::nowMs()) <= m_lastTimestamp) {
                    std::this_thread::yield();
                }
            }
        } else {
            m_sequence = 0;
        }

        m_lastTimestamp = now;
        return ((now - kEpoch) << (kMachineBits + kSequenceBits)) |
               (static_cast<int64_t>(m_machineId) << kSequenceBits) | m_sequence;
    }

private:
    static constexpr int kMachineBits = 10;                        ///< 机器号位数
    static constexpr int kSequenceBits = 12;                       ///< 序列位数
    static constexpr int64_t kSequenceMask = (1 << kSequenceBits) - 1; ///< 序列掩码
    static constexpr int64_t kEpoch = 1735689600000LL;             ///< 起始纪元 2025-01-01（延长可用年限）
    static constexpr int64_t kMaxBackwardMs = 5000;                ///< 时钟回拨容忍上限（毫秒）

    std::mutex m_mutex;
    int m_machineId;                 ///< 机器号
    int64_t m_lastTimestamp = 0;     ///< 最近一次分配的毫秒时间戳
    int64_t m_sequence = 0;          ///< 当前毫秒内已用序列
};

} // namespace lingxi
