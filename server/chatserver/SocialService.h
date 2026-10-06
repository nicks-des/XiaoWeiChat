/**
 * @file SocialService.h
 * @brief M3 社交服务：好友（搜索/申请/处理/删除/列表）+ 群组（建/邀/退/踢/散/资料）。
 *
 * 所有方法阻塞（DB/Redis），调用方须在工作线程池执行（与 MessageService 同约定）。
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "lingxi/base/NonCopyable.h"
#include "lingxi/base/SnowflakeIdGenerator.h"
#include "lingxi/db/MySqlPool.h"
#include "lingxi/db/RedisPool.h"

#include "lingxi.pb.h"  ///< FriendProfile 等协议类型

namespace lingxi {

/**
 * @brief 社交业务服务（每个 ChatServer 一份）。
 */
class SocialService : private NonCopyable {
public:
    /**
     * @brief 投递任务（与 MessageService::Delivery 同构）。
     */
    struct Delivery {
        int64_t uid = 0;
        uint16_t msgId = 0;
        std::string body;
    };

    SocialService(db::MySqlConnectionPool* db, db::RedisConnectionPool* redis,
                  SnowflakeIdGenerator* idGen);

    /* ---- 好友（0x02xx） ---- */

    /**
     * @brief 搜索（0x0201）：uid 精确或用户名前缀，最多 20 条。
     * @return FriendSearchResponse 字节
     */
    std::string handleSearch(int64_t uid, const std::string& payloadBytes);

    /**
     * @brief 好友申请（0x0202）：返回 GenericErr 同步回执；被申请方收 0x0203 通知。
     */
    std::pair<std::string, std::vector<Delivery>> handleApply(
        int64_t fromUid, const std::string& payloadBytes);

    /**
     * @brief 处理申请（0x0204）：同意建双向好友关系并通知申请方（0x0205）。
     */
    std::pair<std::string, std::vector<Delivery>> handleApplyHandle(
        int64_t uid, const std::string& payloadBytes);

    /**
     * @brief 删除好友（0x0206）：双向删除并通知对端（0x0208 语义复用 FriendUpdate）。
     */
    std::pair<std::string, std::vector<Delivery>> handleFriendDelete(
        int64_t uid, const std::string& payloadBytes);

    /**
     * @brief 好友列表（0x0207）：含在线状态与待处理申请数。
     * @return FriendListResponse 字节
     */
    std::string handleFriendList(int64_t uid);

    /* ---- 群组（0x04xx） ---- */

    /**
     * @brief 建群（0x0401）：返回 GroupCreateResponse；成员收 0x0403。
     */
    std::pair<std::string, std::vector<Delivery>> handleGroupCreate(
        int64_t ownerUid, const std::string& payloadBytes);

    /**
     * @brief 邀请入群（0x0402，免确认）：成员收 0x0403。
     */
    std::pair<std::string, std::vector<Delivery>> handleGroupInvite(
        int64_t uid, const std::string& payloadBytes);

    /**
     * @brief 退群（0x0404，群主不可退）：全群收 0x0408。
     */
    std::pair<std::string, std::vector<Delivery>> handleGroupQuit(
        int64_t uid, const std::string& payloadBytes);

    /**
     * @brief 踢人（0x0405，群主/管理员）：全群收 0x0408。
     */
    std::pair<std::string, std::vector<Delivery>> handleGroupKick(
        int64_t uid, const std::string& payloadBytes);

    /**
     * @brief 解散群（0x0406，仅群主）：全群收 0x0408。
     */
    std::pair<std::string, std::vector<Delivery>> handleGroupDissolve(
        int64_t uid, const std::string& payloadBytes);

    /**
     * @brief 群资料（0x0407）。
     * @return GroupInfoResponse 字节
     */
    std::string handleGroupInfo(int64_t uid, const std::string& payloadBytes);

private:
    /**
     * @brief uid 是否在线（Redis 路由存在即在线）。
     */
    bool isOnline(int64_t uid);

    /**
     * @brief 填充 FriendProfile 的用户基础字段。
     * @param observerUid 大于 0 时额外查询是否已互为好友与在线状态
     * @return bool 用户存在为 true
     */
    bool fillProfile(FriendProfile* profile, int64_t uid, int64_t observerUid = 0);

    /**
     * @brief 群成员 uid 列表。
     */
    std::vector<int64_t> groupMembers(int64_t convId);

    /**
     * @brief uid 在群中的角色（-1 表示不在群）。
     */
    int groupRole(int64_t convId, int64_t uid);

    /**
     * @brief 广播群成员变更后的全量成员列表（0x0403）。
     */
    std::vector<Delivery> broadcastMembers(int64_t convId, const std::string& name);

    db::MySqlConnectionPool* m_db;
    db::RedisConnectionPool* m_redis;
    SnowflakeIdGenerator* m_idGen;
};

} // namespace lingxi
