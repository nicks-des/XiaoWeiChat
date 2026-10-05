/**
 * @file MessageService.h
 * @brief M2 消息内核：发送管线（去重→seq→落库→ACK）+ 会话列表 + 增量同步 + 已读 + 撤回。
 *
 * 可靠性规则见 docs/02 §4（R1 先落库后投递 / R2 幂等去重 / R3 seq 排序 / R4 补拉 / R6 seq 权威）。
 * 所有方法阻塞（DB/Redis），调用方须在工作线程池中执行。
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "lingxi/base/NonCopyable.h"
#include "lingxi/base/SnowflakeIdGenerator.h"
#include "lingxi/db/MySqlPool.h"
#include "lingxi/db/RedisPool.h"

namespace lingxi {

/**
 * @brief 消息业务服务（每个 ChatServer 一份）。
 */
class MessageService : private NonCopyable {
public:
    /**
     * @brief 单条投递任务。
     */
    struct Delivery {
        int64_t uid = 0;          ///< 目标用户
        uint16_t msgId = 0;       ///< IM 帧 msgId（0x0303/0x0305/0x0307）
        std::string body;         ///< protobuf body
    };

    /**
     * @brief 发送管线结果。
     */
    struct SendOutcome {
        int errCode = 0;              ///< 0 成功
        std::string errMsg;
        std::string ackBody;          ///< MessageAck(0x0302) protobuf（发送者）
        std::vector<Delivery> deliveries;  ///< 接收方投递列表
    };

    /**
     * @param db      MySQL 连接池（权威存储）
     * @param redis   Redis 连接池（seq/dedup/路由）
     * @param idGen   雪花生成器（msg_id；本服务独占 machineId）
     */
    MessageService(db::MySqlConnectionPool* db, db::RedisConnectionPool* redis,
                   SnowflakeIdGenerator* idGen);

    /**
     * @brief 发送管线（T20-02）：校验成员 → 幂等去重 → INCR seq → 落库 → 构造 ACK 与投递。
     * @param fromUid      发送者
     * @param payloadBytes MessageSendRequest protobuf
     */
    SendOutcome handleSend(int64_t fromUid, const std::string& payloadBytes);

    /**
     * @brief 会话列表（T20-04 前置）：用户全部会话 + 对端昵称 + 未读 + 最近预览。
     * @return ConversationListResponse protobuf 字节
     */
    std::string handleConversationList(int64_t uid);

    /**
     * @brief 增量同步（T20-04）：按各会话 lastSeq 游标差量补拉（分页上限 500 条）。
     * @return SyncResponse protobuf 字节
     */
    std::string handleSync(int64_t uid, const std::string& payloadBytes);

    /**
     * @brief 已读上报（T20-05）：推进 last_read_seq，并向其他成员发 ReadNotice。
     */
    std::vector<Delivery> handleReadAck(int64_t uid, const std::string& payloadBytes);

    /**
     * @brief 撤回（T20-06）：本人 + 2 分钟内 + status=0 才可撤；广播 RecallNotice。
     */
    std::vector<Delivery> handleRecall(int64_t uid, const std::string& payloadBytes);

    /**
     * @brief 从 JSON 信封提取文本预览（会话列表用）。
     */
    static std::string extractPreview(const std::string& payloadJson);

private:
    /**
     * @brief 单聊会话定位/创建：member_key="min_max" 复用（docs/03 §2.2）。
     * @return int64_t conv_id（0 表示失败）
     */
    int64_t getOrCreateDirectConversation(int64_t uidA, int64_t uidB);

    /**
     * @brief 校验 uid 是否为会话成员。
     */
    bool isMember(int64_t convId, int64_t uid);

    /**
     * @brief 幂等查询：clientMsgId 已存在时返回该消息 seq（0 表示不存在）。
     */
    int64_t findExistingSeq(int64_t convId, const std::string& clientMsgId);

    /**
     * @brief 会话全部成员 uid（含发送者）。
     */
    std::vector<int64_t> members(int64_t convId);

    db::MySqlConnectionPool* m_db;        ///< MySQL 池（借宿主生命周期）
    db::RedisConnectionPool* m_redis;     ///< Redis 池
    SnowflakeIdGenerator* m_idGen;        ///< msg_id 生成器
};

} // namespace lingxi
