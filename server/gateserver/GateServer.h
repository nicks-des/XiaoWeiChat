/**
 * @file GateServer.h
 * @brief GateServer 业务：注册/登录 HTTP 接口（Beast）+ 密码哈希 + RPC 调 Status 分配。
 *
 * 接口定义（docs/01 §3.1）：
 * - POST /api/register {username,password} → {err_code,err_msg,uid}
 * - POST /api/login    {username,password} → {err_code,err_msg,uid,token,chat_host,chat_port}
 * 防护：IP 滑动窗口限流（20 次/10s）、防重放预留位、密码 salt+SHA256。
 */
#pragma once

#include <chrono>
#include <deque>
#include <map>
#include <mutex>
#include <string>

#include "lingxi/base/NonCopyable.h"
#include "lingxi/base/ThreadPool.h"
#include "lingxi/base/SnowflakeIdGenerator.h"
#include "lingxi/db/MySqlPool.h"
#include "lingxi/http/HttpServer.h"
#include "lingxi/rpc/RpcClient.h"

namespace lingxi {

/**
 * @brief GateServer 核心服务。
 */
class GateServer : private NonCopyable {
public:
    /**
     * @brief 构造并装配路由（MySQL 连接池 / Status RPC 客户端池 / 雪花生成器）。
     */
    static GateServer& instance();

    /**
     * @brief 装配路由到 HttpServer 并完成初始化。
     */
    void setup(http::HttpServer& server);

private:
    GateServer() = default;

    /* ---- 路由处理器 ---- */
    http::HttpResponse handleRegister(const http::HttpRequest& request,
                                      const std::string& clientIp);
    http::HttpResponse handleLogin(const http::HttpRequest& request,
                                   const std::string& clientIp);

    /**
     * @brief IP 滑动窗口限流。
     * @param ip 客户端 IP
     * @return bool 放行为 true（超限返回 false → 429）
     */
    bool allowRequest(const std::string& ip);

    db::MySqlOptions m_mysqlOptions;               ///< MySQL 配置
    std::unique_ptr<db::MySqlConnectionPool> m_dbPool; ///< MySQL 连接池
    std::unique_ptr<rpc::RpcClientPool> m_statusRpc;   ///< Status RPC 客户端池
    std::unique_ptr<SnowflakeIdGenerator> m_idGenerator;  ///< uid 生成器（NonCopyable 故指针持有）

    std::mutex m_rateMutex;                                    ///< 限流表锁
    std::map<std::string, std::deque<int64_t>> m_rateWindow;   ///< ip → 请求时间戳
};

} // namespace lingxi
