/**
 * @file CallService.h
 * @brief M5 通话信令服务： invite/ring/accept/reject/cancel/hangup 状态机 + ICE 中继 + 通话记录。
 *
 * 状态机（docs/05 §2）：calling → ringing → active → ended；
 * 占线/超时(45s)/被叫离线由服务端裁决并写 t_call_record。
 */
#pragma once

#include <boost/asio.hpp>

namespace asio = boost::asio;  ///< 项目内简写

#include <cstdint>
#include <map>
#include <vector>
#include <memory>
#include <mutex>
#include <string>

#include "lingxi/base/NonCopyable.h"
#include "lingxi/base/SnowflakeIdGenerator.h"
#include "lingxi/db/MySqlPool.h"

namespace lingxi {

class ChatServer;

/**
 * @brief 通话信令服务（每个 ChatServer 一份）。
 */
class CallService : private NonCopyable {
public:
    /**
     * @brief 通话状态机。
     */
    enum State { kCalling = 0, kRinging = 1, kActive = 2 };

    /**
     * @brief 通话记录状态（t_call_record.state）。
     */
    enum RecordState { kRecMissed = 0, kRecConnected = 1, kRecRejected = 2, kRecCancelled = 3, kRecAbnormal = 4 };

    CallService(ChatServer& server, db::MySqlConnectionPool* db,
                SnowflakeIdGenerator* idGen);

    /**
     * @brief 启动 5s 扫描器：振铃超时（45s）与僵尸会话清理（异步定时器）。
     */
    void startSweeper(asio::io_context& ioContext);

    /* ---- 信令处理（工作线程池执行；返回需投递的帧） ---- */

    struct Delivery {
        int64_t uid = 0;
        uint16_t msgId = 0;
        std::string body;
    };

    /**
     * @brief 主叫邀请（0x0601）：占线校验 → 转振铃 → 通知被叫。
     */
    std::pair<std::string, std::vector<Delivery>> handleInvite(
        int64_t callerUid, const std::string& payloadBytes);

    /**
     * @brief 被叫接听（0x0603）：状态切 active → 通知主叫（携带 Answer）。
     */
    std::pair<std::string, std::vector<Delivery>> handleAccept(
        int64_t calleeUid, const std::string& payloadBytes);

    /**
     * @brief 被叫拒绝（0x0604 走 GenericErr 语义体）：通知主叫 + 记录。
     */
    std::pair<std::string, std::vector<Delivery>> handleReject(
        int64_t calleeUid, const std::string& payloadBytes);

    /**
     * @brief 主叫取消（0x0605）：通知被叫 + 记录。
     */
    std::pair<std::string, std::vector<Delivery>> handleCancel(
        int64_t callerUid, const std::string& payloadBytes);

    /**
     * @brief 挂断（0x0606）：任一端 → 通知对端 + 记录时长。
     */
    std::pair<std::string, std::vector<Delivery>> handleHangup(
        int64_t uid, const std::string& payloadBytes);

    /**
     * @brief SDP 重协商 / ICE 候选中继（0x0607/0x0608）：校验通话成员后转发对端。
     */
    std::vector<Delivery> handleRelay(int64_t uid, uint16_t msgId,
                                      const std::string& payloadBytes);

private:
    /**
     * @brief 单路通话会话。
     */
    struct CallSession {
        std::string callId;
        int64_t callerUid = 0;
        int64_t calleeUid = 0;
        int32_t mediaType = 1;
        std::string offerSdp;
        int state = kCalling;
        int64_t connectedAtMs = 0;   ///< 接通时间（计时长）
        int64_t createdAtMs = 0;     ///< 邀请时间（超时判定）
    };

    /**
     * @brief 查找某 uid 正在进行的通话（主叫或被叫身份）。
     * @return CallSession* 无则 nullptr
     */
    CallSession* findActiveByUid(int64_t uid);

    /**
     * @brief 结束会话并写通话记录。
     * @param recordState t_call_record.state
     */
    void endCall(const std::string& callId, int recordState, int32_t durationSec);

    /**
     * @brief 写通话记录。
     */
    void writeRecord(const CallSession& session, int recordState, int32_t durationSec);

    /**
     * @brief 扫描器节拍：振铃超时 → CallStateNotice(超时) + 记录未接。
     */
    void sweep(asio::steady_timer& timer);

    ChatServer& m_server;
    db::MySqlConnectionPool* m_db;
    SnowflakeIdGenerator* m_idGen;
    std::mutex m_mutex;                              ///< 会话表锁
    std::map<std::string, CallSession> m_sessions;   ///< callId → 会话
    int m_ringTimeoutSec = 45;                       ///< 振铃超时（docs/05 §2）
    std::shared_ptr<asio::steady_timer> m_sweepTimer;  ///< 扫描定时器（随服务存活）
};

} // namespace lingxi
