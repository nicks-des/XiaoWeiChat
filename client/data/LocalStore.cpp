/**
 * @file LocalStore.cpp
 * @brief 客户端 SQLite 缓存实现。
 */
#include "LocalStore.h"

#include "lingxi/logging/Logger.h"

namespace lingxi::client {

LocalStore::~LocalStore() {
    if (m_db != nullptr) {
        sqlite3_close(m_db);
        m_db = nullptr;
    }
}

bool LocalStore::open(const std::string& path) {
    if (sqlite3_open(path.c_str(), &m_db) != SQLITE_OK) {
        LX_LOG_ERROR("LocalStore open failed: {}", sqlite3_errmsg(m_db));
        return false;
    }
    exec("PRAGMA journal_mode=WAL");  // 读写并发友好
    const bool ok =
        exec("CREATE TABLE IF NOT EXISTS conversations ("
             "conv_id INTEGER PRIMARY KEY, type INTEGER, peer_uid INTEGER, peer_name TEXT,"
             "last_seq INTEGER DEFAULT 0, last_read_seq INTEGER DEFAULT 0,"
             "preview TEXT DEFAULT '', last_msg_time_ms INTEGER DEFAULT 0)") &&
        exec("CREATE TABLE IF NOT EXISTS messages ("
             "conv_id INTEGER, seq INTEGER DEFAULT 0, msg_id INTEGER, client_msg_id TEXT,"
             "from_uid INTEGER, msg_type INTEGER, status INTEGER, payload TEXT,"
             "send_time_ms INTEGER, PRIMARY KEY(conv_id, client_msg_id))") &&
        exec("CREATE INDEX IF NOT EXISTS idx_msg_seq ON messages(conv_id, seq)") &&
        exec("CREATE INDEX IF NOT EXISTS idx_msg_client ON messages(client_msg_id)");
    LX_LOG_INFO("LocalStore opened: {} ({})", path, ok ? "ok" : "schema failed");
    return ok;
}

bool LocalStore::exec(const std::string& sql) {
    char* errMsg = nullptr;
    if (sqlite3_exec(m_db, sql.c_str(), nullptr, nullptr, &errMsg) != SQLITE_OK) {
        LX_LOG_ERROR("LocalStore exec failed: {} ({})", errMsg == nullptr ? "?" : errMsg, sql);
        sqlite3_free(errMsg);
        return false;
    }
    return true;
}

/* ==================== 会话 ==================== */

void LocalStore::upsertConversation(const ConvRow& row) {
    const std::string sql =
        "INSERT INTO conversations (conv_id, type, peer_uid, peer_name, last_seq, "
        "last_read_seq, preview, last_msg_time_ms) VALUES (?,?,?,?,?,?,?,?) "
        "ON CONFLICT(conv_id) DO UPDATE SET type=excluded.type, peer_uid=excluded.peer_uid,"
        "peer_name=excluded.peer_name, last_seq=MAX(last_seq, excluded.last_seq),"
        "preview=CASE WHEN excluded.last_seq>=last_seq THEN excluded.preview ELSE preview END,"
        "last_msg_time_ms=CASE WHEN excluded.last_seq>=last_seq THEN excluded.last_msg_time_ms"
        " ELSE last_msg_time_ms END";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return;
    }
    sqlite3_bind_int64(stmt, 1, row.convId);
    sqlite3_bind_int(stmt, 2, row.type);
    sqlite3_bind_int64(stmt, 3, row.peerUid);
    sqlite3_bind_text(stmt, 4, row.peerName.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 5, row.lastSeq);
    sqlite3_bind_int64(stmt, 6, row.lastReadSeq);
    sqlite3_bind_text(stmt, 7, row.preview.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 8, row.lastMsgTimeMs);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

std::vector<LocalStore::ConvRow> LocalStore::loadConversations() {
    std::vector<ConvRow> rows;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, "SELECT conv_id, type, peer_uid, peer_name, last_seq, "
                                 "last_read_seq, preview, last_msg_time_ms FROM conversations "
                                 "ORDER BY last_msg_time_ms DESC",
                           -1, &stmt, nullptr) != SQLITE_OK) {
        return rows;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        ConvRow row;
        row.convId = sqlite3_column_int64(stmt, 0);
        row.type = sqlite3_column_int(stmt, 1);
        row.peerUid = sqlite3_column_int64(stmt, 2);
        row.peerName = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        row.lastSeq = sqlite3_column_int64(stmt, 4);
        row.lastReadSeq = sqlite3_column_int64(stmt, 5);
        row.preview = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
        row.lastMsgTimeMs = sqlite3_column_int64(stmt, 7);
        rows.push_back(std::move(row));
    }
    sqlite3_finalize(stmt);
    return rows;
}

void LocalStore::updateConversationTail(int64_t convId, int64_t seq, const std::string& preview,
                                        int64_t timeMs) {
    const std::string sql =
        "UPDATE conversations SET last_seq=MAX(last_seq, ?), "
        "preview=CASE WHEN ?>=last_seq THEN ? ELSE preview END, "
        "last_msg_time_ms=CASE WHEN ?>=last_seq THEN ? ELSE last_msg_time_ms END "
        "WHERE conv_id=?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return;
    }
    sqlite3_bind_int64(stmt, 1, seq);
    sqlite3_bind_int64(stmt, 2, seq);
    sqlite3_bind_text(stmt, 3, preview.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 4, seq);
    sqlite3_bind_int64(stmt, 5, timeMs);
    sqlite3_bind_int64(stmt, 6, convId);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void LocalStore::markRead(int64_t convId, int64_t seq) {
    const std::string sql =
        "UPDATE conversations SET last_read_seq=MAX(last_read_seq, ?) WHERE conv_id=?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return;
    }
    sqlite3_bind_int64(stmt, 1, seq);
    sqlite3_bind_int64(stmt, 2, convId);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

/* ==================== 消息 ==================== */

void LocalStore::insertMessage(const MsgRow& row) {
    const std::string sql =
        "INSERT OR REPLACE INTO messages (conv_id, seq, msg_id, client_msg_id, from_uid, "
        "msg_type, status, payload, send_time_ms) VALUES (?,?,?,?,?,?,?,?,?)";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return;
    }
    sqlite3_bind_int64(stmt, 1, row.convId);
    sqlite3_bind_int64(stmt, 2, row.seq);
    sqlite3_bind_int64(stmt, 3, row.msgId);
    sqlite3_bind_text(stmt, 4, row.clientMsgId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 5, row.fromUid);
    sqlite3_bind_int(stmt, 6, row.msgType);
    sqlite3_bind_int(stmt, 7, row.status);
    sqlite3_bind_text(stmt, 8, row.payload.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 9, row.sendTimeMs);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

bool LocalStore::confirmMessage(const std::string& clientMsgId, int64_t convId, int64_t seq,
                                int64_t sendTimeMs) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db,
                           "UPDATE messages SET conv_id=?, seq=?, send_time_ms=?, status=0 "
                           "WHERE client_msg_id=? AND seq=0",
                           -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_int64(stmt, 1, convId);
    sqlite3_bind_int64(stmt, 2, seq);
    sqlite3_bind_int64(stmt, 3, sendTimeMs);
    sqlite3_bind_text(stmt, 4, clientMsgId.c_str(), -1, SQLITE_TRANSIENT);
    const bool changed = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(m_db) > 0;
    sqlite3_finalize(stmt);
    return changed;
}

std::vector<LocalStore::MsgRow> LocalStore::loadMessages(int64_t convId, int limit) {
    std::vector<MsgRow> rows;
    sqlite3_stmt* stmt = nullptr;
    const std::string sql =
        "SELECT conv_id, seq, msg_id, client_msg_id, from_uid, msg_type, status, payload, "
        "send_time_ms FROM messages WHERE conv_id=? AND seq>0 ORDER BY seq DESC LIMIT " +
        std::to_string(limit);
    if (sqlite3_prepare_v2(m_db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return rows;
    }
    sqlite3_bind_int64(stmt, 1, convId);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        MsgRow row;
        row.convId = sqlite3_column_int64(stmt, 0);
        row.seq = sqlite3_column_int64(stmt, 1);
        row.msgId = sqlite3_column_int64(stmt, 2);
        row.clientMsgId = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        row.fromUid = sqlite3_column_int64(stmt, 4);
        row.msgType = sqlite3_column_int(stmt, 5);
        row.status = sqlite3_column_int(stmt, 6);
        row.payload = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
        row.sendTimeMs = sqlite3_column_int64(stmt, 8);
        rows.push_back(std::move(row));
    }
    sqlite3_finalize(stmt);
    // 转为时间正序展示
    std::reverse(rows.begin(), rows.end());
    return rows;
}

int64_t LocalStore::maxSeq(int64_t convId) {
    sqlite3_stmt* stmt = nullptr;
    int64_t value = 0;
    if (sqlite3_prepare_v2(m_db, "SELECT MAX(seq) FROM messages WHERE conv_id=?", -1, &stmt,
                           nullptr) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_int64(stmt, 1, convId);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        value = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return value;
}

void LocalStore::markRecalled(int64_t convId, int64_t msgId) {
    const std::string sql = "UPDATE messages SET status=1 WHERE conv_id=? AND msg_id=?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return;
    }
    sqlite3_bind_int64(stmt, 1, convId);
    sqlite3_bind_int64(stmt, 2, msgId);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

} // namespace lingxi::client
