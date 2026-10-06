/**
 * @file SocialService.h
 * @brief 客户端社交服务（M3）：好友搜索/申请/处理/删除/列表 + 建群/邀请/退群/解散。
 *
 * 在线的好友资料缓存在内存（每次拉取刷新）；联系人面板直接消费信号。
 */
#pragma once

#include <QObject>
#include <QString>

#include <string>
#include <vector>

#include "lingxi.pb.h"

namespace lingxi::client {

class TcpClient;

/**
 * @brief 社交服务（单例）。
 */
class SocialService : public QObject {
    Q_OBJECT
public:
    static SocialService& instance();

    /**
     * @brief 初始化（注入长连接）。
     */
    void setup(TcpClient* tcp);

    /* ---- 主动请求 ---- */

    /**
     * @brief 搜索用户（0x0201）。
     */
    void search(const std::string& keyword);

    /**
     * @brief 发好友申请（0x0202）。
     */
    void apply(int64_t toUid, const std::string& verifyMsg);

    /**
     * @brief 处理申请（0x0204）。
     */
    void handleApply(int64_t applyId, bool agree);

    /**
     * @brief 删除好友（0x0206）。
     */
    void deleteFriend(int64_t friendUid);

    /**
     * @brief 拉取好友列表（0x0207）。
     */
    void requestFriendList();

    /**
     * @brief 建群（0x0401）。
     */
    void createGroup(const std::string& name, const std::vector<long long>& memberUids);

    /**
     * @brief 邀请入群（0x0402）。
     */
    void inviteGroup(long long convId, const std::vector<long long>& memberUids);

    /**
     * @brief 退群（0x0404）。
     */
    void quitGroup(long long convId);

    /**
     * @brief 解散群（0x0406，群主）。
     */
    void dissolveGroup(long long convId);

    /**
     * @brief IO 线程收帧入口（AccountService 转发）。
     */
    void onPacket(uint16_t msgId, const std::string& body);

    /**
     * @brief 当前好友列表快照（UI 渲染）。
     */
    const FriendListResponse& friends() const { return m_friends; }

    /**
     * @brief 待处理申请快照（UI 渲染，含 applyId）。
     */
    const std::vector<FriendApplyNotice>& pendingApplies() const { return m_pendingApplies; }

    /**
     * @brief 最近一次搜索结果。
     */
    const FriendSearchResponse& searchResult() const { return m_searchResult; }

signals:
    /// 好友列表已更新（含未处理申请数）
    void friendListUpdated(int pendingApplies);
    /// 搜索结果到达
    void searchResultArrived();
    /// 收到好友申请（弹提示/红点）
    void applyReceived(QString fromNickname, QString verifyMsg);
    /// 申请被处理（同意/拒绝/被删）
    void applyResultReceived(bool agree);
    /// 通用操作错误提示
    void socialError(QString message);

private:
    /**
     * @brief 在 UI 线程发射信号的便捷封装。
     */
    template <typename Fn>
    void emitOnUi(Fn&& fn);

    TcpClient* m_tcp = nullptr;             ///< 长连接（借宿主）
    FriendListResponse m_friends;           ///< 好友列表快照
    FriendSearchResponse m_searchResult;    ///< 搜索结果快照
    std::vector<FriendApplyNotice> m_pendingApplies;  ///< 待处理申请（0x0203 到达累积）
};

} // namespace lingxi::client
