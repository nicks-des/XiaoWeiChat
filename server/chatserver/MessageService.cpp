/**
 * @file MessageService.cpp
 * @brief M2 消息内核实现。
 */
#include "MessageService.h"

#include <nlohmann/json.hpp>

#include "lingxi/base/TimeUtil.h"
#include "lingxi/base/Uuid.h"
#include "lingxi/logging/Logger.h"

#include "lingxi.pb.h"

namespace lingxi {

namespace {

/** 单次同步补拉上限（条） */
constexpr int64_t kSyncLimit = 500;

/** 撤回时限（毫秒，docs/02 §4：2 分钟） */
constexpr int64_t kRecallWindowMs = 2 * 60 * 1000;

/**
 * @brief 从 t_message 行构造 MsgBody。
 */
MsgBody buildMsgBody(db::MySqlResult& row) {
    MsgBody body;
    body.set_conv_id(row.getInt64(0));
    body.set_conv_seq(row.getInt64(1));
    body.set_msg_id(row.getInt64(2));
    body.set_client_msg_id(row.getString(3));
    body.set_from_uid(row.getInt64(4));
    body.set_msg_type(static_cast<MsgType>(row.getInt64(5)));
    body.set_status(static_cast<int32_t>(row.getInt64(6)));
    body.set_payload(row.getString(7));
    body.set_send_time_ms(
        static_cast<int64_t>(row.getInt64(8)));
    return body;
}

/** 消息表标准查询列顺序 */
constexpr const char* kMsgColumns =
    "conv_id, seq, msg_id, client_msg_id, from_uid, msg_type, status, payload, "
    "ROUND(UNIX_TIMESTAMP(created_at) * 1000)";

} // namespace

MessageService::MessageService(db::MySqlConnectionPool* db, db::RedisConnectionPool* redis,
                               SnowflakeIdGenerator* idGen)
    : m_db(db), m_redis(redis), m_idGen(idGen) {}

/* ==================== 发送管线（T20-02） ==================== */

MessageService::SendOutcome MessageService::handleSend(int64_t fromUid,
                                                       const std::string& payloadBytes) {
    SendOutcome outcome;
    MessageSendRequest request;
    if (!request.ParseFromString(payloadBytes)) {
        outcome.errCode = 400;
        outcome.errMsg = "bad send request";
        return outcome;
    }
    const MsgBody& input = request.body();

    // 1. 会话定位：优先 conv_id，单聊缺省按 to_uid 建会话
    int64_t convId = input.conv_id();
    if (convId <= 0 && input.to_uid() > 0) {
        convId = getOrCreateDirectConversation(fromUid, input.to_uid());
    }
    if (convId <= 0) {
        outcome.errCode = 400;
        outcome.errMsg = "conv unresolved (need conv_id or to_uid)";
        return outcome;
    }
    if (!isMember(convId, fromUid)) {
        outcome.errCode = 403;
        outcome.errMsg = "not a member of conversation";
        return outcome;
    }
    if (input.client_msg_id().empty()) {
        outcome.errCode = 400;
        outcome.errMsg = "client_msg_id required";
        return outcome;
    }

    auto conn = m_db->acquire();
    if (conn == nullptr) {
        outcome.errCode = 503;
        outcome.errMsg = "db busy";
        return outcome;
    }

    // 2. 幂等快路径：Redis dedup 命中 → 直接回放 ACK（R2）
    const std::string dedupKey =
        "dedup:" + std::to_string(convId) + ":" + input.client_msg_id();
    {
        auto redis = m_redis->acquire();
        if (redis != nullptr) {
            auto cached = redis->exec("GET %s", dedupKey.c_str());
            if (cached.ok() && !cached.str().empty()) {
                outcome.ackBody = cached.str();  // 存储的是完整 MessageAck
                LX_LOG_INFO("send dedup hit: conv={} clientMsgId={}", convId,
                            input.client_msg_id());
                return outcome;
            }
        }
    }

    // 3. seq 分配（R6：服务端权威）+ 落库（R1：先落库后投递）
    auto redis = m_redis->acquire();
    if (redis == nullptr) {
        outcome.errCode = 503;
        outcome.errMsg = "redis busy";
        return outcome;
    }
    auto seqReply = redis->exec("INCR %s", ("seq:conv:" + std::to_string(convId)).c_str());
    if (!seqReply.ok() || seqReply.integer() < 0) {
        outcome.errCode = 503;
        outcome.errMsg = "seq alloc failed";
        return outcome;
    }
    const int64_t seq = seqReply.integer();
    const int64_t msgId = m_idGen->nextId();
    const int64_t nowMs = TimeUtil::nowMs();

    // payload JSON：服务端统一信封（附带 msg_id 便于客户端撤回寻址）
    std::string payloadJson(input.payload().begin(), input.payload().end());
    try {
        auto parsed = nlohmann::json::parse(payloadJson.empty() ? "{}" : payloadJson);
        parsed["msgId"] = msgId;
        payloadJson = parsed.dump();
    } catch (const nlohmann::json::exception&) {
        payloadJson = nlohmann::json({{"msgId", msgId}}).dump();
    }

    const std::string insertSql =
        "INSERT INTO t_message (conv_id, seq, msg_id, client_msg_id, from_uid, msg_type, "
        "status, payload, created_at) VALUES (" + std::to_string(convId) + ", " +
        std::to_string(seq) + ", " + std::to_string(msgId) + ", '" +
        conn->escapeString(input.client_msg_id()) + "', " + std::to_string(fromUid) + ", " +
        std::to_string(input.msg_type()) + ", 0, '" + conn->escapeString(payloadJson) +
        "', FROM_UNIXTIME(" + std::to_string(nowMs / 1000) + "))";
    if (!conn->execute(insertSql)) {
        if (conn->errorCode() == 1062) {  // 唯一键冲突：client_msg_id 重复 → 幂等回放（R2 兜底）
            const int64_t existingSeq = findExistingSeq(convId, input.client_msg_id());
            MessageAck ack;
            ack.set_err_code(0);
            ack.set_conv_id(convId);
            ack.set_client_msg_id(input.client_msg_id());
            ack.set_conv_seq(existingSeq);
            ack.set_send_time_ms(nowMs);
            outcome.ackBody = ack.SerializeAsString();
            LX_LOG_WARN("send unique-key dedup: conv={} seq={}", convId, existingSeq);
            return outcome;
        }
        outcome.errCode = 500;
        outcome.errMsg = "db insert failed";
        return outcome;
    }

    // 会话游标推进（会话列表加速）
    conn->execute("UPDATE t_conversation SET last_seq=" + std::to_string(seq) +
                  ", last_msg_at=FROM_UNIXTIME(" + std::to_string(nowMs / 1000) +
                  ") WHERE id=" + std::to_string(convId));

    // 4. dedup 写回（幂等 ACK 缓存 24h）
    {
        MessageAck ack;
        ack.set_err_code(0);
        ack.set_conv_id(convId);
        ack.set_client_msg_id(input.client_msg_id());
        ack.set_conv_seq(seq);
        ack.set_send_time_ms(nowMs);
        outcome.ackBody = ack.SerializeAsString();
        redis->exec("SET %s %s EX 86400", dedupKey.c_str(), outcome.ackBody.c_str());
    }

    // 5. 构造投递（R1：已落库，投递失败靠补拉兜底）
    MsgBody notify;
    notify.set_conv_id(convId);
    notify.set_from_uid(fromUid);
    notify.set_conv_seq(seq);
    notify.set_client_msg_id(input.client_msg_id());
    notify.set_msg_type(input.msg_type());
    notify.set_send_time_ms(nowMs);
    notify.set_payload(payloadJson);
    notify.set_status(0);
    notify.set_msg_id(msgId);
    MessageNotify notifyMsg;
    *notifyMsg.mutable_body() = notify;
    for (int64_t memberUid : members(convId)) {
        if (memberUid == fromUid) {
            continue;  // 发送者不需要自投递（ACK 已确认）
        }
        outcome.deliveries.push_back(
            {memberUid, 0x0303, notifyMsg.SerializeAsString()});
    }

    LX_LOG_INFO("msg persisted: conv={} seq={} from={} clientMsgId={}", convId, seq, fromUid,
                input.client_msg_id());
    return outcome;
}

/* ==================== 会话列表 / 同步 / 已读 / 撤回 ==================== */

std::string MessageService::handleConversationList(int64_t uid) {
    ConversationListResponse response;
    auto conn = m_db->acquire();
    if (conn == nullptr) {
        return response.SerializeAsString();
    }

    db::MySqlResult rows;
    if (!conn->query(
            "SELECT conv_id, last_read_seq FROM t_conversation_member WHERE uid=" +
                std::to_string(uid),
            rows)) {
        return response.SerializeAsString();
    }

    struct ConvRow {
        int64_t convId;
        int64_t lastReadSeq;
    };
    std::vector<ConvRow> myConvs;
    while (rows.next()) {
        myConvs.push_back({rows.getInt64(0), rows.getInt64(1)});
    }

    for (const auto& entry : myConvs) {
        db::MySqlResult convRow;
        if (!conn->query("SELECT type, member_key, last_seq, UNIX_TIMESTAMP(last_msg_at)*1000 "
                         "FROM t_conversation WHERE id=" + std::to_string(entry.convId),
                         convRow) || !convRow.next()) {
            continue;
        }
        ConversationInfo info;
        info.set_conv_id(entry.convId);
        info.set_type(static_cast<int32_t>(convRow.getInt64(0)));
        info.set_last_seq(convRow.getInt64(2));
        info.set_last_read_seq(entry.lastReadSeq);
        info.set_last_msg_time_ms(convRow.getInt64(3));

        // 单聊对端昵称
        const std::string memberKey = convRow.getString(1);
        const auto underscore = memberKey.find('_');
        if (convRow.getInt64(0) == 1 && underscore != std::string::npos) {
            const int64_t uidA = std::stoll(memberKey.substr(0, underscore));
            const int64_t uidB = std::stoll(memberKey.substr(underscore + 1));
            const int64_t peerUid = (uidA == uid) ? uidB : uidA;
            info.set_peer_uid(peerUid);
            db::MySqlResult userRow;
            if (conn->query("SELECT nickname FROM t_user WHERE id=" + std::to_string(peerUid),
                            userRow) && userRow.next()) {
                info.set_peer_nickname(userRow.getString(0));
            }
        } else {
            db::MySqlResult groupRow;
            if (conn->query("SELECT name FROM t_group WHERE conv_id=" +
                                std::to_string(entry.convId),
                            groupRow) && groupRow.next()) {
                info.set_peer_nickname(groupRow.getString(0));
            }
        }

        // 最近一条消息预览
        db::MySqlResult msgRow;
        if (conn->query("SELECT payload FROM t_message WHERE conv_id=" +
                            std::to_string(entry.convId) + " ORDER BY seq DESC LIMIT 1",
                        msgRow) && msgRow.next()) {
            info.set_last_preview(extractPreview(msgRow.getString(0)));
        }
        *response.add_conversations() = info;
    }
    return response.SerializeAsString();
}

std::string MessageService::handleSync(int64_t uid, const std::string& payloadBytes) {
    SyncResponse response;
    SyncRequest request;
    if (!request.ParseFromString(payloadBytes)) {
        return response.SerializeAsString();
    }

    auto conn = m_db->acquire();
    if (conn == nullptr) {
        return response.SerializeAsString();
    }

    bool truncated = false;
    for (const auto& cursor : request.cursors()) {
        if (response.messages_size() >= kSyncLimit) {
            truncated = true;
            break;
        }
        if (!isMember(cursor.conv_id(), uid)) {
            continue;  // 越权防护：只同步自己是成员的会话
        }
        db::MySqlResult rows;
        if (!conn->query("SELECT " + std::string(kMsgColumns) + " FROM t_message WHERE conv_id=" +
                             std::to_string(cursor.conv_id()) + " AND seq>" +
                             std::to_string(cursor.last_seq()) + " ORDER BY seq ASC LIMIT " +
                             std::to_string(kSyncLimit),
                         rows)) {
            continue;
        }
        while (rows.next()) {
            *response.add_messages() = buildMsgBody(rows);
        }
    }
    response.set_has_more(truncated);
    return response.SerializeAsString();
}

std::vector<MessageService::Delivery> MessageService::handleReadAck(
    int64_t uid, const std::string& payloadBytes) {
    std::vector<Delivery> deliveries;
    MessageReadAck request;
    if (!request.ParseFromString(payloadBytes)) {
        return deliveries;
    }
    if (!isMember(request.conv_id(), uid)) {
        return deliveries;
    }

    auto conn = m_db->acquire();
    if (conn == nullptr) {
        return deliveries;
    }
    conn->execute("UPDATE t_conversation_member SET last_read_seq=" +
                  std::to_string(request.last_read_seq()) + " WHERE conv_id=" +
                  std::to_string(request.conv_id()) + " AND uid=" + std::to_string(uid) +
                  " AND last_read_seq<" + std::to_string(request.last_read_seq()));

    MessageReadNotice notice;
    notice.set_conv_id(request.conv_id());
    notice.set_peer_uid(uid);
    notice.set_last_read_seq(request.last_read_seq());
    for (int64_t memberUid : members(request.conv_id())) {
        if (memberUid != uid) {
            deliveries.push_back({memberUid, 0x0305, notice.SerializeAsString()});
        }
    }
    return deliveries;
}

std::vector<MessageService::Delivery> MessageService::handleRecall(
    int64_t uid, const std::string& payloadBytes) {
    std::vector<Delivery> deliveries;
    MessageRecallRequest request;
    if (!request.ParseFromString(payloadBytes)) {
        return deliveries;
    }
    if (!isMember(request.conv_id(), uid)) {
        return deliveries;
    }

    auto conn = m_db->acquire();
    if (conn == nullptr) {
        return deliveries;
    }
    db::MySqlResult row;
    if (!conn->query("SELECT from_uid, ROUND(UNIX_TIMESTAMP(created_at)*1000), status FROM "
                     "t_message WHERE conv_id=" + std::to_string(request.conv_id()) +
                         " AND msg_id=" + std::to_string(request.msg_id()),
                     row) || !row.next()) {
        return deliveries;
    }
    if (row.getInt64(0) != uid) {
        LX_LOG_WARN("recall denied (not sender): uid={} msg={}", uid, request.msg_id());
        return deliveries;
    }
    if (TimeUtil::nowMs() - row.getInt64(1) > kRecallWindowMs) {
        LX_LOG_WARN("recall denied (timeout): msg={}", request.msg_id());
        return deliveries;
    }
    if (row.getInt64(2) != 0) {
        return deliveries;  // 已撤回/生成中，不可重复撤
    }
    if (!conn->execute("UPDATE t_message SET status=1 WHERE conv_id=" +
                       std::to_string(request.conv_id()) + " AND msg_id=" +
                       std::to_string(request.msg_id()))) {
        return deliveries;
    }

    MessageRecallNotice notice;
    notice.set_conv_id(request.conv_id());
    notice.set_msg_id(request.msg_id());
    notice.set_operator_uid(uid);
    for (int64_t memberUid : members(request.conv_id())) {
        deliveries.push_back({memberUid, 0x0307, notice.SerializeAsString()});
    }
    return deliveries;
}

/* ==================== 内部工具 ==================== */

int64_t MessageService::getOrCreateDirectConversation(int64_t uidA, int64_t uidB) {
    auto conn = m_db->acquire();
    if (conn == nullptr) {
        return 0;
    }
    const int64_t minUid = std::min(uidA, uidB);
    const int64_t maxUid = std::max(uidA, uidB);
    const std::string memberKey = std::to_string(minUid) + "_" + std::to_string(maxUid);

    db::MySqlResult row;
    if (conn->query("SELECT id FROM t_conversation WHERE member_key='" + memberKey + "'", row) &&
        row.next()) {
        return row.getInt64(0);
    }

    const int64_t convId = m_idGen->nextId();
    if (!conn->execute("INSERT INTO t_conversation (id, type, member_key) VALUES (" +
                       std::to_string(convId) + ", 1, '" + memberKey + "')")) {
        // 并发建会话：member_key 唯一键冲突 → 重新查询复用
        db::MySqlResult retry;
        if (conn->query("SELECT id FROM t_conversation WHERE member_key='" + memberKey + "'",
                        retry) && retry.next()) {
            return retry.getInt64(0);
        }
        return 0;
    }
    for (int64_t uid : {minUid, maxUid}) {
        conn->execute("INSERT IGNORE INTO t_conversation_member (conv_id, uid) VALUES (" +
                      std::to_string(convId) + ", " + std::to_string(uid) + ")");
    }
    LX_LOG_INFO("conversation created: conv={} members={}", convId, memberKey);
    return convId;
}

bool MessageService::isMember(int64_t convId, int64_t uid) {
    auto conn = m_db->acquire();
    if (conn == nullptr) {
        return false;
    }
    db::MySqlResult row;
    return conn->query("SELECT 1 FROM t_conversation_member WHERE conv_id=" +
                           std::to_string(convId) + " AND uid=" + std::to_string(uid),
                       row) && row.next();
}

int64_t MessageService::findExistingSeq(int64_t convId, const std::string& clientMsgId) {
    auto conn = m_db->acquire();
    if (conn == nullptr) {
        return 0;
    }
    db::MySqlResult row;
    if (conn->query("SELECT seq FROM t_message WHERE conv_id=" + std::to_string(convId) +
                        " AND client_msg_id='" + conn->escapeString(clientMsgId) + "'",
                    row) && row.next()) {
        return row.getInt64(0);
    }
    return 0;
}

std::vector<int64_t> MessageService::members(int64_t convId) {
    std::vector<int64_t> result;
    auto conn = m_db->acquire();
    if (conn == nullptr) {
        return result;
    }
    db::MySqlResult rows;
    if (conn->query("SELECT uid FROM t_conversation_member WHERE conv_id=" +
                        std::to_string(convId),
                    rows)) {
        while (rows.next()) {
            result.push_back(rows.getInt64(0));
        }
    }
    return result;
}

std::string MessageService::extractPreview(const std::string& payloadJson) {
    try {
        auto parsed = nlohmann::json::parse(payloadJson);
        if (parsed.contains("text") && parsed["text"].is_string()) {
            std::string text = parsed["text"].get<std::string>();
            if (text.size() > 60) {
                text = text.substr(0, 60) + "…";
            }
            return text;
        }
    } catch (const nlohmann::json::exception&) {
    }
    return "[消息]";
}

} // namespace lingxi
