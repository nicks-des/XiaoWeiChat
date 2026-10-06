/**
 * @file SocialService.cpp
 * @brief M3 社交服务实现（好友 + 群组）。
 */
#include "SocialService.h"

#include "lingxi/base/TimeUtil.h"
#include "lingxi/logging/Logger.h"

#include "lingxi.pb.h"

namespace lingxi {

namespace {

/** 申请 7 天过期（docs/06 T30-01） */
constexpr int64_t kApplyExpireMs = 7LL * 24 * 3600 * 1000;

/** 搜索结果上限 */
constexpr int kSearchLimit = 20;

/** 群成员上限（默认配置） */
constexpr int kGroupMaxMember = 200;

} // namespace

SocialService::SocialService(db::MySqlConnectionPool* db, db::RedisConnectionPool* redis,
                             SnowflakeIdGenerator* idGen)
    : m_db(db), m_redis(redis), m_idGen(idGen) {}

/* ==================== 内部工具 ==================== */

bool SocialService::isOnline(int64_t uid) {
    auto redis = m_redis->acquire();
    if (redis == nullptr) {
        return false;
    }
    auto reply = redis->exec("GET %s", ("route:uid:" + std::to_string(uid)).c_str());
    return reply.ok() && !reply.str().empty();
}

bool SocialService::fillProfile(FriendProfile* profile, int64_t uid, int64_t observerUid) {
    auto conn = m_db->acquire();
    if (conn == nullptr) {
        return false;
    }
    db::MySqlResult row;
    if (!conn->query("SELECT id, username, nickname, signature FROM t_user WHERE id=" +
                         std::to_string(uid),
                     row) || !row.next()) {
        return false;
    }
    profile->set_uid(row.getInt64(0));
    profile->set_username(row.getString(1));
    profile->set_nickname(row.getString(2));
    profile->set_signature(row.getString(3));

    if (observerUid > 0 && observerUid != uid) {
        db::MySqlResult friendRow;
        const bool isFriend =
            conn->query("SELECT 1 FROM t_friend WHERE uid=" + std::to_string(observerUid) +
                            " AND friend_uid=" + std::to_string(uid),
                        friendRow) && friendRow.next();
        profile->set_is_friend(isFriend);
        profile->set_online(isOnline(uid));
    }
    return true;
}

std::vector<int64_t> SocialService::groupMembers(int64_t convId) {
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

int SocialService::groupRole(int64_t convId, int64_t uid) {
    auto conn = m_db->acquire();
    if (conn == nullptr) {
        return -1;
    }
    db::MySqlResult row;
    if (conn->query("SELECT role FROM t_conversation_member WHERE conv_id=" +
                        std::to_string(convId) + " AND uid=" + std::to_string(uid),
                    row) && row.next()) {
        return static_cast<int>(row.getInt64(0));
    }
    return -1;
}

std::vector<SocialService::Delivery> SocialService::broadcastMembers(int64_t convId,
                                                                     const std::string& name) {
    std::vector<Delivery> deliveries;
    auto conn = m_db->acquire();
    if (conn == nullptr) {
        return deliveries;
    }
    GroupJoinNotice notice;
    notice.set_conv_id(convId);
    notice.set_name(name);
    for (int64_t memberUid : groupMembers(convId)) {
        auto* info = notice.add_members();
        info->set_uid(memberUid);
        db::MySqlResult row;
        if (conn->query("SELECT nickname FROM t_user WHERE id=" + std::to_string(memberUid),
                        row) && row.next()) {
            info->set_nickname(row.getString(0));
        }
        info->set_role(static_cast<int32_t>(groupRole(convId, memberUid)));
    }
    for (int64_t memberUid : groupMembers(convId)) {
        deliveries.push_back({memberUid, 0x0403, notice.SerializeAsString()});
    }
    return deliveries;
}

/* ==================== 好友（0x02xx） ==================== */

std::string SocialService::handleSearch(int64_t uid, const std::string& payloadBytes) {
    FriendSearchResponse response;
    FriendSearchRequest request;
    if (!request.ParseFromString(payloadBytes) || request.keyword().empty()) {
        return response.SerializeAsString();
    }
    const std::string& keyword = request.keyword();

    auto conn = m_db->acquire();
    if (conn == nullptr) {
        return response.SerializeAsString();
    }

    // 纯数字按 uid 精确；否则按用户名前缀
    const bool isNumeric = keyword.find_first_not_of("0123456789") == std::string::npos;
    std::string sql = "SELECT id, username, nickname, signature FROM t_user WHERE user_type=0 AND ";
    sql += isNumeric ? ("id=" + keyword)
                     : ("username LIKE '" + conn->escapeString(keyword) + "%'");
    sql += " AND id<>" + std::to_string(uid) + " LIMIT " + std::to_string(kSearchLimit);

    db::MySqlResult rows;
    if (!conn->query(sql, rows)) {
        return response.SerializeAsString();
    }
    while (rows.next()) {
        FriendProfile profile;
        if (fillProfile(&profile, rows.getInt64(0), uid)) {
            *response.add_users() = profile;
        }
    }
    return response.SerializeAsString();
}

std::pair<std::string, std::vector<SocialService::Delivery>> SocialService::handleApply(
    int64_t fromUid, const std::string& payloadBytes) {
    GenericErr response;
    std::vector<Delivery> deliveries;
    FriendApplyRequest request;
    if (!request.ParseFromString(payloadBytes)) {
        response.set_err_code(400);
        response.set_err_msg("bad apply");
        return {response.SerializeAsString(), deliveries};
    }
    const int64_t toUid = request.to_uid();
    if (toUid == fromUid) {
        response.set_err_code(400);
        response.set_err_msg("cannot add yourself");
        return {response.SerializeAsString(), deliveries};
    }

    auto conn = m_db->acquire();
    if (conn == nullptr) {
        response.set_err_code(503);
        response.set_err_msg("db busy");
        return {response.SerializeAsString(), deliveries};
    }

    // 已是好友 / 已有待处理申请 → 幂等拒绝
    db::MySqlResult friendRow;
    if (conn->query("SELECT 1 FROM t_friend WHERE uid=" + std::to_string(fromUid) +
                        " AND friend_uid=" + std::to_string(toUid),
                    friendRow) && friendRow.next()) {
        response.set_err_code(409);
        response.set_err_msg("already friends");
        return {response.SerializeAsString(), deliveries};
    }
    db::MySqlResult pendingRow;
    if (conn->query("SELECT id FROM t_friend_apply WHERE from_uid=" +
                        std::to_string(fromUid) + " AND to_uid=" + std::to_string(toUid) +
                        " AND status=0",
                    pendingRow) && pendingRow.next()) {
        response.set_err_code(409);
        response.set_err_msg("apply pending");
        return {response.SerializeAsString(), deliveries};
    }

    const int64_t applyId = m_idGen->nextId();
    if (!conn->execute("INSERT INTO t_friend_apply (id, from_uid, to_uid, verify_msg) VALUES (" +
                       std::to_string(applyId) + ", " + std::to_string(fromUid) + ", " +
                       std::to_string(toUid) + ", '" +
                       conn->escapeString(request.verify_msg()) + "')")) {
        response.set_err_code(500);
        response.set_err_msg("db insert failed");
        return {response.SerializeAsString(), deliveries};
    }

    // 通知被申请方（0x0203）
    FriendProfile profile;
    fillProfile(&profile, fromUid);
    FriendApplyNotice notice;
    notice.set_apply_id(applyId);
    notice.set_from_uid(fromUid);
    notice.set_from_nickname(profile.nickname());
    notice.set_verify_msg(request.verify_msg());
    deliveries.push_back({toUid, 0x0203, notice.SerializeAsString()});

    LX_LOG_INFO("friend apply: {} -> {} applyId={}", fromUid, toUid, applyId);
    response.set_err_code(0);
    return {response.SerializeAsString(), deliveries};
}

std::pair<std::string, std::vector<SocialService::Delivery>> SocialService::handleApplyHandle(
    int64_t uid, const std::string& payloadBytes) {
    GenericErr response;
    std::vector<Delivery> deliveries;
    FriendApplyHandleRequest request;
    if (!request.ParseFromString(payloadBytes)) {
        response.set_err_code(400);
        return {response.SerializeAsString(), deliveries};
    }

    auto conn = m_db->acquire();
    if (conn == nullptr) {
        response.set_err_code(503);
        return {response.SerializeAsString(), deliveries};
    }

    // 申请单归属与状态校验（含 7 天过期）
    db::MySqlResult row;
    if (!conn->query("SELECT from_uid, to_uid, status, ROUND(UNIX_TIMESTAMP(created_at)*1000) "
                     "FROM t_friend_apply WHERE id=" + std::to_string(request.apply_id()),
                     row) || !row.next()) {
        response.set_err_code(404);
        response.set_err_msg("apply not found");
        return {response.SerializeAsString(), deliveries};
    }
    if (row.getInt64(1) != uid) {
        response.set_err_code(403);
        response.set_err_msg("not your apply");
        return {response.SerializeAsString(), deliveries};
    }
    if (row.getInt64(2) != 0 || TimeUtil::nowMs() - row.getInt64(3) > kApplyExpireMs) {
        response.set_err_code(410);
        response.set_err_msg("apply expired or handled");
        return {response.SerializeAsString(), deliveries};
    }
    const int64_t fromUid = row.getInt64(0);

    if (!conn->execute("UPDATE t_friend_apply SET status=" +
                       std::to_string(request.agree() ? 1 : 2) +
                       ", handled_at=NOW() WHERE id=" + std::to_string(request.apply_id()))) {
        response.set_err_code(500);
        return {response.SerializeAsString(), deliveries};
    }

    if (request.agree()) {
        // 双向好友关系（uk_pair 各一行，INSERT IGNORE 防重复）
        for (const auto& pair : {std::make_pair(uid, fromUid), std::make_pair(fromUid, uid)}) {
            conn->execute("INSERT IGNORE INTO t_friend (id, uid, friend_uid, source) VALUES (" +
                          std::to_string(m_idGen->nextId()) + ", " +
                          std::to_string(pair.first) + ", " + std::to_string(pair.second) +
                          ", 0)");
        }
        FriendApplyResultNotice notice;
        notice.set_apply_id(request.apply_id());
        notice.set_agree(true);
        FriendProfile* profile = notice.mutable_friend_();
        if (fillProfile(profile, uid)) {
            profile->set_is_friend(true);
            profile->set_online(isOnline(uid));
        }
        deliveries.push_back({fromUid, 0x0205, notice.SerializeAsString()});
        LX_LOG_INFO("friend agreed: {} <-> {}", uid, fromUid);
    } else {
        FriendApplyResultNotice notice;
        notice.set_apply_id(request.apply_id());
        notice.set_agree(false);
        deliveries.push_back({fromUid, 0x0205, notice.SerializeAsString()});
        LX_LOG_INFO("friend rejected: {} x {}", uid, fromUid);
    }
    response.set_err_code(0);
    return {response.SerializeAsString(), deliveries};
}

std::pair<std::string, std::vector<SocialService::Delivery>> SocialService::handleFriendDelete(
    int64_t uid, const std::string& payloadBytes) {
    GenericErr response;
    std::vector<Delivery> deliveries;
    FriendDeleteRequest request;
    if (!request.ParseFromString(payloadBytes)) {
        response.set_err_code(400);
        return {response.SerializeAsString(), deliveries};
    }

    auto conn = m_db->acquire();
    if (conn == nullptr) {
        response.set_err_code(503);
        return {response.SerializeAsString(), deliveries};
    }
    // 双向删除
    conn->execute("DELETE FROM t_friend WHERE (uid=" + std::to_string(uid) +
                  " AND friend_uid=" + std::to_string(request.friend_uid()) +
                  ") OR (uid=" + std::to_string(request.friend_uid()) + " AND friend_uid=" +
                  std::to_string(uid) + ")");

    // 通知对端（复用 0x0208 语义：FriendUpdateNotice 体内 GenericErr 携带动作）
    FriendApplyResultNotice notice;  // M3 复用：friend 字段携带被删者，agree=false 表示删除
    notice.set_agree(false);
    FriendProfile* profile = notice.mutable_friend_();
    fillProfile(profile, uid);  // 告知对端「是谁删了你」
    deliveries.push_back({request.friend_uid(), 0x0205, notice.SerializeAsString()});

    LX_LOG_INFO("friend deleted: {} x {}", uid, request.friend_uid());
    response.set_err_code(0);
    return {response.SerializeAsString(), deliveries};
}

std::string SocialService::handleFriendList(int64_t uid) {
    FriendListResponse response;
    auto conn = m_db->acquire();
    if (conn == nullptr) {
        return response.SerializeAsString();
    }

    db::MySqlResult rows;
    if (conn->query("SELECT f.friend_uid, f.remark, f.group_name, u.username, u.nickname, "
                    "u.signature FROM t_friend f JOIN t_user u ON u.id=f.friend_uid "
                    "WHERE f.uid=" + std::to_string(uid) + " ORDER BY f.group_name, u.nickname",
                    rows)) {
        while (rows.next()) {
            FriendProfile* profile = response.add_friends();
            profile->set_uid(rows.getInt64(0));
            profile->set_remark(rows.getString(1));
            profile->set_group_name(rows.getString(2));
            profile->set_username(rows.getString(3));
            profile->set_nickname(rows.getString(4));
            profile->set_signature(rows.getString(5));
            profile->set_is_friend(true);
            profile->set_online(isOnline(rows.getInt64(0)));
        }
    }
    db::MySqlResult pending;
    if (conn->query("SELECT COUNT(*) FROM t_friend_apply WHERE to_uid=" + std::to_string(uid) +
                        " AND status=0 AND created_at>NOW() - INTERVAL 7 DAY",
                    pending) && pending.next()) {
        response.set_pending_applies(static_cast<int32_t>(pending.getInt64(0)));
    }
    return response.SerializeAsString();
}

/* ==================== 群组（0x04xx） ==================== */

std::pair<std::string, std::vector<SocialService::Delivery>> SocialService::handleGroupCreate(
    int64_t ownerUid, const std::string& payloadBytes) {
    GroupCreateResponse response;
    std::vector<Delivery> deliveries;
    GroupCreateRequest request;
    if (!request.ParseFromString(payloadBytes) || request.name().empty()) {
        response.set_err_code(400);
        response.set_err_msg("bad group create");
        return {response.SerializeAsString(), deliveries};
    }
    if (request.member_uids_size() + 1 > kGroupMaxMember) {
        response.set_err_code(400);
        response.set_err_msg("too many members");
        return {response.SerializeAsString(), deliveries};
    }

    auto conn = m_db->acquire();
    if (conn == nullptr) {
        response.set_err_code(503);
        return {response.SerializeAsString(), deliveries};
    }

    const int64_t convId = m_idGen->nextId();
    if (!conn->execute("INSERT INTO t_conversation (id, type, name, owner_uid) VALUES (" +
                       std::to_string(convId) + ", 2, '" + conn->escapeString(request.name()) +
                       "', " + std::to_string(ownerUid) + ")")) {
        response.set_err_code(500);
        response.set_err_msg("db failed");
        return {response.SerializeAsString(), deliveries};
    }
    if (!conn->execute("INSERT INTO t_group (conv_id, name) VALUES (" +
                       std::to_string(convId) + ", '" + conn->escapeString(request.name()) +
                       "')")) {
        response.set_err_code(500);
        response.set_err_msg("db failed");
        return {response.SerializeAsString(), deliveries};
    }
    // 群主 + 初始成员（存在性校验：仅真人且存在的 uid 入群）
    conn->execute("INSERT IGNORE INTO t_conversation_member (conv_id, uid, role) VALUES (" +
                  std::to_string(convId) + ", " + std::to_string(ownerUid) + ", 0)");
    for (const auto& memberUid : request.member_uids()) {
        if (memberUid == ownerUid) {
            continue;
        }
        db::MySqlResult exists;
        if (conn->query("SELECT 1 FROM t_user WHERE id=" + std::to_string(memberUid), exists) &&
            exists.next()) {
            conn->execute("INSERT IGNORE INTO t_conversation_member (conv_id, uid, role) "
                          "VALUES (" +
                          std::to_string(convId) + ", " + std::to_string(memberUid) + ", 2)");
        }
    }

    LX_LOG_INFO("group created: conv={} name={} owner={}", convId, request.name(), ownerUid);
    response.set_err_code(0);
    response.set_conv_id(convId);
    deliveries = broadcastMembers(convId, request.name());
    return {response.SerializeAsString(), deliveries};
}

std::pair<std::string, std::vector<SocialService::Delivery>> SocialService::handleGroupInvite(
    int64_t uid, const std::string& payloadBytes) {
    GenericErr response;
    std::vector<Delivery> deliveries;
    GroupInviteRequest request;
    if (!request.ParseFromString(payloadBytes)) {
        response.set_err_code(400);
        return {response.SerializeAsString(), deliveries};
    }
    if (groupRole(request.conv_id(), uid) < 0) {
        response.set_err_code(403);
        response.set_err_msg("not a member");
        return {response.SerializeAsString(), deliveries};
    }

    auto conn = m_db->acquire();
    if (conn == nullptr) {
        response.set_err_code(503);
        return {response.SerializeAsString(), deliveries};
    }
    db::MySqlResult groupRow;
    if (!conn->query("SELECT name FROM t_group WHERE conv_id=" +
                         std::to_string(request.conv_id()),
                     groupRow) || !groupRow.next()) {
        response.set_err_code(404);
        response.set_err_msg("group not found");
        return {response.SerializeAsString(), deliveries};
    }

    for (const auto& memberUid : request.member_uids()) {
        db::MySqlResult exists;
        if (conn->query("SELECT 1 FROM t_user WHERE id=" + std::to_string(memberUid), exists) &&
            exists.next()) {
            conn->execute("INSERT IGNORE INTO t_conversation_member (conv_id, uid, role) "
                          "VALUES (" +
                          std::to_string(request.conv_id()) + ", " +
                          std::to_string(memberUid) + ", 2)");
        }
    }
    response.set_err_code(0);
    deliveries = broadcastMembers(request.conv_id(), groupRow.getString(0));
    return {response.SerializeAsString(), deliveries};
}

std::pair<std::string, std::vector<SocialService::Delivery>> SocialService::handleGroupQuit(
    int64_t uid, const std::string& payloadBytes) {
    GenericErr response;
    std::vector<Delivery> deliveries;
    GroupQuitRequest request;
    if (!request.ParseFromString(payloadBytes)) {
        response.set_err_code(400);
        return {response.SerializeAsString(), deliveries};
    }
    const int role = groupRole(request.conv_id(), uid);
    if (role < 0) {
        response.set_err_code(403);
        response.set_err_msg("not a member");
        return {response.SerializeAsString(), deliveries};
    }
    if (role == 0) {
        response.set_err_code(400);
        response.set_err_msg("owner cannot quit, dissolve instead");
        return {response.SerializeAsString(), deliveries};
    }

    auto conn = m_db->acquire();
    if (conn == nullptr) {
        response.set_err_code(503);
        return {response.SerializeAsString(), deliveries};
    }
    conn->execute("DELETE FROM t_conversation_member WHERE conv_id=" +
                  std::to_string(request.conv_id()) + " AND uid=" + std::to_string(uid));

    std::string name;
    db::MySqlResult groupRow;
    if (conn->query("SELECT name FROM t_group WHERE conv_id=" +
                        std::to_string(request.conv_id()),
                    groupRow) && groupRow.next()) {
        name = groupRow.getString(0);
    }
    GroupUpdateNotice notice;
    notice.set_action(1);
    notice.set_conv_id(request.conv_id());
    notice.set_target_uid(uid);
    notice.set_name(name);
    for (int64_t memberUid : groupMembers(request.conv_id())) {
        deliveries.push_back({memberUid, 0x0408, notice.SerializeAsString()});
    }
    // 退群者自己也收一份便于客户端清理
    deliveries.push_back({uid, 0x0408, notice.SerializeAsString()});
    response.set_err_code(0);
    return {response.SerializeAsString(), deliveries};
}

std::pair<std::string, std::vector<SocialService::Delivery>> SocialService::handleGroupKick(
    int64_t uid, const std::string& payloadBytes) {
    GenericErr response;
    std::vector<Delivery> deliveries;
    GroupKickRequest request;
    if (!request.ParseFromString(payloadBytes)) {
        response.set_err_code(400);
        return {response.SerializeAsString(), deliveries};
    }
    const int role = groupRole(request.conv_id(), uid);
    if (role != 0 && role != 1) {
        response.set_err_code(403);
        response.set_err_msg("no permission");
        return {response.SerializeAsString(), deliveries};
    }
    if (request.target_uid() == uid) {
        response.set_err_code(400);
        response.set_err_msg("cannot kick yourself");
        return {response.SerializeAsString(), deliveries};
    }

    auto conn = m_db->acquire();
    if (conn == nullptr) {
        response.set_err_code(503);
        return {response.SerializeAsString(), deliveries};
    }
    conn->execute("DELETE FROM t_conversation_member WHERE conv_id=" +
                  std::to_string(request.conv_id()) + " AND uid=" +
                  std::to_string(request.target_uid()));

    std::string name;
    db::MySqlResult groupRow;
    if (conn->query("SELECT name FROM t_group WHERE conv_id=" +
                        std::to_string(request.conv_id()),
                    groupRow) && groupRow.next()) {
        name = groupRow.getString(0);
    }
    GroupUpdateNotice notice;
    notice.set_action(2);
    notice.set_conv_id(request.conv_id());
    notice.set_target_uid(request.target_uid());
    notice.set_name(name);
    for (int64_t memberUid : groupMembers(request.conv_id())) {
        deliveries.push_back({memberUid, 0x0408, notice.SerializeAsString()});
    }
    deliveries.push_back({request.target_uid(), 0x0408, notice.SerializeAsString()});
    response.set_err_code(0);
    return {response.SerializeAsString(), deliveries};
}

std::pair<std::string, std::vector<SocialService::Delivery>> SocialService::handleGroupDissolve(
    int64_t uid, const std::string& payloadBytes) {
    GenericErr response;
    std::vector<Delivery> deliveries;
    GroupDissolveRequest request;
    if (!request.ParseFromString(payloadBytes)) {
        response.set_err_code(400);
        return {response.SerializeAsString(), deliveries};
    }
    if (groupRole(request.conv_id(), uid) != 0) {
        response.set_err_code(403);
        response.set_err_msg("owner only");
        return {response.SerializeAsString(), deliveries};
    }

    auto conn = m_db->acquire();
    if (conn == nullptr) {
        response.set_err_code(503);
        return {response.SerializeAsString(), deliveries};
    }
    std::string name;
    db::MySqlResult groupRow;
    if (conn->query("SELECT name FROM t_group WHERE conv_id=" +
                        std::to_string(request.conv_id()),
                    groupRow) && groupRow.next()) {
        name = groupRow.getString(0);
    }
    // 先取成员名单再删（广播依赖）
    const std::vector<int64_t> membersBefore = groupMembers(request.conv_id());
    conn->execute("DELETE FROM t_group WHERE conv_id=" + std::to_string(request.conv_id()));
    conn->execute("DELETE FROM t_conversation_member WHERE conv_id=" +
                  std::to_string(request.conv_id()));
    conn->execute("DELETE FROM t_conversation WHERE id=" + std::to_string(request.conv_id()));

    GroupUpdateNotice notice;
    notice.set_action(3);
    notice.set_conv_id(request.conv_id());
    notice.set_name(name);
    for (int64_t memberUid : membersBefore) {
        deliveries.push_back({memberUid, 0x0408, notice.SerializeAsString()});
    }
    response.set_err_code(0);
    return {response.SerializeAsString(), deliveries};
}

std::string SocialService::handleGroupInfo(int64_t uid, const std::string& payloadBytes) {
    GroupInfoResponse response;
    GroupInfoRequest request;
    if (!request.ParseFromString(payloadBytes)) {
        return response.SerializeAsString();
    }
    if (groupRole(request.conv_id(), uid) < 0) {
        return response.SerializeAsString();  // 非成员返回空
    }

    auto conn = m_db->acquire();
    if (conn == nullptr) {
        return response.SerializeAsString();
    }
    db::MySqlResult groupRow;
    if (conn->query("SELECT name, announcement, owner_uid FROM t_group g JOIN t_conversation c "
                    "ON c.id=g.conv_id WHERE g.conv_id=" +
                        std::to_string(request.conv_id()),
                    groupRow) && groupRow.next()) {
        response.set_conv_id(request.conv_id());
        response.set_name(groupRow.getString(0));
        response.set_announcement(groupRow.getString(1));
        response.set_owner_uid(groupRow.getInt64(2));
    }
    for (int64_t memberUid : groupMembers(request.conv_id())) {
        auto* info = response.add_members();
        info->set_uid(memberUid);
        info->set_role(static_cast<int32_t>(groupRole(request.conv_id(), memberUid)));
        db::MySqlResult row;
        if (conn->query("SELECT nickname FROM t_user WHERE id=" + std::to_string(memberUid),
                        row) && row.next()) {
            info->set_nickname(row.getString(0));
        }
    }
    return response.SerializeAsString();
}

} // namespace lingxi
