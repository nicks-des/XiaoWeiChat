/**
 * @file AccountService.h
 * @brief 账号业务服务：注册/登录 HTTP 流程 + TCP 长连接登录 + 顶号接收（QtCore，无 Widgets）。
 *
 * 线程模型（docs/01 §7）：HTTP 与 TCP 回调在工作/IO 线程触发，
 * 统一经 QMetaObject::invokeMethod(QueuedConnection) 切回 UI 线程后再发 Qt 信号。
 */
#pragma once

#include <QObject>
#include <QString>

#include <memory>
#include <string>

#include "client/net/TcpClient.h"

namespace lingxi::client {

/**
 * @brief 账号服务（单例，归属 UI 线程）。
 */
class AccountService : public QObject {
    Q_OBJECT
public:
    /**
     * @brief 获取全局实例。
     */
    static AccountService& instance();

    /**
     * @brief 初始化（读取配置，构造 TcpClient）。
     * @return bool 配置加载成功为 true
     */
    bool setup();

    /**
     * @brief 注册账号（异步，结果经 registerFinished 信号返回）。
     */
    void registerAccount(const std::string& username, const std::string& password);

    /**
     * @brief 登录（异步：HTTP 换 token → TCP 长连接校验，结果经 loginFinished /
     *        tcpLoginFinished 信号返回）。
     */
    void login(const std::string& username, const std::string& password);

    /**
     * @brief 主动登出（关闭长连接）。
     */
    void logout();

    /**
     * @brief 当前登录 uid（未登录为 0）。
     */
    long long uid() const { return m_uid; }

    /**
     * @brief TCP 连接是否可用。
     */
    bool isTcpConnected() const { return m_tcp != nullptr && m_tcp->isConnected(); }

signals:
    /// 注册结果（工作线程信号，经队列投递）
    void registerFinished(int errCode, const QString& errMsg, long long uid);
    /// HTTP 登录结果（token 已取得；errCode!=0 时后两项无意义）
    void loginFinished(int errCode, const QString& errMsg, long long uid);
    /// TCP 长连接登录结果（登录链路全部完成）
    void tcpLoginFinished(int errCode, const QString& errMsg);
    /// TCP 连接已建立
    void tcpConnected();
    /// 被顶号下线
    void kicked(const QString& deviceInfo);
    /// 连接断开（异常掉线）
    void connectionLost(const QString& reason);

private slots:
    /// Qt 定时器驱动的 TCP 登录超时兜底
    void onTcpLoginTimeout();

private:
    AccountService() = default;

    /**
     * @brief 收帧分发（IO 线程触发 → 切 UI 线程）。
     */
    void onPacket(uint16_t msgId, const std::string& body);

    /**
     * @brief 在 UI 线程发射信号的便捷封装。
     */
    template <typename Fn>
    void emitOnUi(Fn&& fn);

    std::unique_ptr<TcpClient> m_tcp;   ///< 长连接
    std::string m_gateHost;             ///< Gate 地址
    unsigned short m_gatePort = 8080;   ///< Gate HTTP 端口
    std::string m_token;                ///< 当前 token
    long long m_uid = 0;                ///< 当前 uid
    bool m_tcpLoginDone = false;        ///< TCP 登录是否已出结果（超时兜底用）
};

} // namespace lingxi::client
