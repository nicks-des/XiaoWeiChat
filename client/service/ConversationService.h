/**
 * @file ConversationService.h
 * @brief 会话业务服务（M2）：发送/接收/ACK/同步/已读/撤回，串联 TcpClient 与 LocalStore。
 *
 * 线程模型：网络回调在 IO 线程 → invokeMethod 切 UI 线程 → LocalStore（仅 UI 线程访问）。
 */
#pragma once

#include <QObject>
#include <QString>

#include <string>

#include "client/data/LocalStore.h"

namespace lingxi::client {

class TcpClient;

/**
 * @brief 会话服务（单例）。
 */
class ConversationService : public QObject {
    Q_OBJECT
public:
    static ConversationService& instance();

    /**
     * @brief 初始化（注入 TcpClient 与本地存储； TcpClient 收帧由 AccountService 转发）。
     */
    bool setup(TcpClient* tcp, const std::string& uidKey);

    /**
     * @brief 拉取会话列表（0x030A；服务端回包后自动发起增量同步）。
     */
    void requestConversationList();

    /**
     * @brief 增量同步（0x0308，按本地 SQLite 各会话 lastSeq 游标）。
     */
    void requestSync();

    /**
     * @brief 发送文本消息（先本地落库 status=-1 发送中，ACK 后回填 seq）。
     * @param toUid  单聊对端
     * @param text   文本内容
     * @return std::string clientMsgId（幂等 ID）
     */
    std::string sendText(int64_t toUid, const std::string& text);

    /**
     * @brief 已读上报（0x0304，打开会话时调用）。
     */
    void markRead(int64_t convId);

    /**
     * @brief 撤回（0x0306）。
     */
    void recall(int64_t convId, int64_t msgId);

    /**
     * @brief IO 线程收帧入口（AccountService 转发）。
     */
    void onPacket(uint16_t msgId, const std::string& body);

    /**
     * @brief 本地存储访问（ChatWindow 渲染用）。
     */
    LocalStore& store() { return m_store; }

signals:
    /// 会话列表已更新（启动拉取 / 收到新消息）
    void conversationListUpdated();
    /// 收到新消息（convId）
    void messageArrived(long long convId);
    /// 发送确认回填（clientMsgId → seq）
    void messageAcked(QString clientMsgId, long long convId, long long seq);
    /// 增量同步完成
    void syncCompleted();
    /// 消息被撤回
    void messageRecalled(long long convId, long long msgId);
    /// 对端已读推进
    void peerRead(long long convId, long long seq);

private:
    ConversationService() = default;

    /**
     * @brief 在 UI 线程发射信号的便捷封装。
     */
    template <typename Fn>
    void emitOnUi(Fn&& fn);

    LocalStore m_store;     ///< 本地缓存
    TcpClient* m_tcp = nullptr;  ///< 长连接（借宿主）
    std::string m_uidKey;   ///< 本地库文件标识（uid）
};

} // namespace lingxi::client
