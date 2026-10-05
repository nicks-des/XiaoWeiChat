/**
 * @file LocalStore.h
 * @brief 客户端本地缓存（SQLite）：会话表 + 消息表 + 已读游标（离线可看、增量同步，ADR-009）。
 *
 * 线程约定：仅 UI 线程访问（ConversationService 已负责线程切换）。
 */
#pragma once

#include <sqlite3.h>

#include <cstdint>
#include <string>
#include <vector>

#include "lingxi/base/NonCopyable.h"

namespace lingxi::client {

/**
 * @brief 本地存储（每用户一个库文件）。
 */
class LocalStore : private NonCopyable {
public:
    /**
     * @brief 会话行。
     */
    struct ConvRow {
        int64_t convId = 0;
        int type = 1;
        int64_t peerUid = 0;
        std::string peerName;
        int64_t lastSeq = 0;      ///< 服务端最新 seq
        int64_t lastReadSeq = 0;  ///< 本地已读游标（同步补拉起点）
        std::string preview;
        int64_t lastMsgTimeMs = 0;
    };

    /**
     * @brief 消息行。
     */
    struct MsgRow {
        int64_t convId = 0;
        int64_t seq = 0;          ///< 0 表示发送中（ACK 后回填）
        int64_t msgId = 0;
        std::string clientMsgId;
        int64_t fromUid = 0;
        int msgType = 1;
        int status = 0;           ///< -1 发送中 / 0 正常 / 1 已撤回
        std::string payload;      ///< JSON 信封
        int64_t sendTimeMs = 0;
    };

    LocalStore() = default;
    ~LocalStore();

    /**
     * @brief 打开（不存在则建表）。
     * @param path SQLite 文件路径
     */
    bool open(const std::string& path);

    /* ---- 会话 ---- */

    /**
     * @brief 插入或更新会话（存在则刷新游标/预览）。
     */
    void upsertConversation(const ConvRow& row);

    /**
     * @brief 按最近消息时间倒序加载全部会话。
     */
    std::vector<ConvRow> loadConversations();

    /**
     * @brief 更新会话最近消息（seq/预览/时间）。
     */
    void updateConversationTail(int64_t convId, int64_t seq, const std::string& preview,
                                int64_t timeMs);

    /**
     * @brief 推进已读游标。
     */
    void markRead(int64_t convId, int64_t seq);

    /* ---- 消息 ---- */

    /**
     * @brief 插入或替换消息（按 client_msg_id 幂等）。
     */
    void insertMessage(const MsgRow& row);

    /**
     * @brief ACK 回填：按 clientMsgId 写入 conv/seq/时间。
     * @return bool 命中本地待确认消息为 true
     */
    bool confirmMessage(const std::string& clientMsgId, int64_t convId, int64_t seq,
                        int64_t sendTimeMs);

    /**
     * @brief 加载会话消息（seq 升序，最近 limit 条）。
     */
    std::vector<MsgRow> loadMessages(int64_t convId, int limit = 200);

    /**
     * @brief 会话内本地最大已落 seq（同步游标）。
     */
    int64_t maxSeq(int64_t convId);

    /**
     * @brief 撤回标记（按 msgId）。
     */
    void markRecalled(int64_t convId, int64_t msgId);

private:
    /**
     * @brief 执行无结果 SQL（建表等）。
     */
    bool exec(const std::string& sql);

    sqlite3* m_db = nullptr;  ///< SQLite 连接
};

} // namespace lingxi::client
