/**
 * @file GateServer.cpp
 * @brief GateServer 业务实现：注册/登录全流程。
 */
#include "GateServer.h"

#include "lingxi/rpc/RpcFrame.h"

#include <nlohmann/json.hpp>

#include "lingxi/base/TimeUtil.h"
#include "lingxi/base/Uuid.h"
#include "lingxi/config/Config.h"
#include "lingxi/crypto/Crypto.h"
#include "lingxi/logging/Logger.h"

#include "lingxi.pb.h"

namespace lingxi {

namespace {

namespace bhttp = boost::beast::http;

/** 限流窗口 */
constexpr int64_t kRateWindowMs = 10 * 1000;

/** 用户名/密码合法长度（T10-02 校验规则） */
constexpr size_t kUsernameMin = 3;
constexpr size_t kUsernameMax = 32;
constexpr size_t kPasswordMin = 6;
constexpr size_t kPasswordMax = 64;

/**
 * @brief 解析请求 JSON body；失败时返回 std::nullopt。
 */
std::optional<nlohmann::json> parseJsonBody(const bhttp::request<bhttp::string_body>& request) {
    try {
        return nlohmann::json::parse(request.body());
    } catch (const nlohmann::json::parse_error&) {
        return std::nullopt;
    }
}

} // namespace

GateServer& GateServer::instance() {
    static GateServer s_server;
    return s_server;
}

void GateServer::setup(http::HttpServer& server) {
    auto& config = Config::instance();

    m_mysqlOptions.host = config.get<std::string>("mysql.host", "127.0.0.1");
    m_mysqlOptions.port = config.get<int>("mysql.port", 3316);
    m_mysqlOptions.user = config.get<std::string>("mysql.user", "root");
    m_mysqlOptions.password = config.get<std::string>("mysql.password", "");
    m_mysqlOptions.database = config.get<std::string>("mysql.database", "lingxi");
    m_dbPool = std::make_unique<db::MySqlConnectionPool>(m_mysqlOptions);

    m_statusRpc = std::make_unique<rpc::RpcClientPool>(
        config.get<std::string>("statusserver.host", "127.0.0.1"),
        static_cast<unsigned short>(config.get<int>("statusserver.rpcPort", 9000)), 2);

    m_idGenerator = std::make_unique<SnowflakeIdGenerator>(config.get<int>("gateserver.machineId", 11));
    m_rateLimit = static_cast<size_t>(config.get<int>("gateserver.rateLimit", 20));

    server.route("POST", "/api/register",
                 [this](const bhttp::request<bhttp::string_body>& request,
                        const std::string& clientIp) { return handleRegister(request, clientIp); });
    server.route("POST", "/api/login",
                 [this](const bhttp::request<bhttp::string_body>& request,
                        const std::string& clientIp) { return handleLogin(request, clientIp); });

    LX_LOG_INFO("GateServer setup done (mysql {}:{}, status {}:{})", m_mysqlOptions.host,
                m_mysqlOptions.port, config.get<std::string>("statusserver.host", "127.0.0.1"),
                config.get<int>("statusserver.rpcPort", 9000));
}

bool GateServer::allowRequest(const std::string& ip) {
    const int64_t now = TimeUtil::nowMs();
    std::lock_guard<std::mutex> lock(m_rateMutex);
    auto& window = m_rateWindow[ip];
    while (!window.empty() && now - window.front() > kRateWindowMs) {
        window.pop_front();
    }
    if (window.size() >= m_rateLimit) {
        LX_LOG_WARN("rate limit hit, ip={}", ip);
        return false;
    }
    window.push_back(now);
    return true;
}

lingxi::http::HttpResponse GateServer::handleRegister(
    const bhttp::request<bhttp::string_body>& request, const std::string& clientIp) {
    if (!allowRequest(clientIp)) {
        return lingxi::http::makeErrorResponse(bhttp::status::too_many_requests, 429, "too many requests");
    }

    auto body = parseJsonBody(request);
    if (body == std::nullopt || !body->is_object()) {
        return lingxi::http::makeErrorResponse(bhttp::status::bad_request, 400, "invalid json");
    }
    const std::string username = body->value("username", "");
    const std::string password = body->value("password", "");
    if (username.size() < kUsernameMin || username.size() > kUsernameMax) {
        return lingxi::http::makeErrorResponse(bhttp::status::bad_request, 400, "username length invalid");
    }
    if (password.size() < kPasswordMin || password.size() > kPasswordMax) {
        return lingxi::http::makeErrorResponse(bhttp::status::bad_request, 400, "password length invalid");
    }

    auto conn = m_dbPool->acquire();
    if (conn == nullptr) {
        return lingxi::http::makeErrorResponse(bhttp::status::service_unavailable, 503, "db busy");
    }

    // 用户名查重（转义防注入）
    {
        db::MySqlResult result;
        const std::string sql =
            "SELECT id FROM t_user WHERE username='" + conn->escapeString(username) + "'";
        if (!conn->query(sql, result)) {
            return lingxi::http::makeErrorResponse(bhttp::status::internal_server_error, 500, "db error");
        }
        if (result.next()) {
            return lingxi::http::makeErrorResponse(bhttp::status::bad_request, 409, "username exists");
        }
    }

    // 建 uid + 哈希入库
    const int64_t uid = m_idGenerator->nextId();
    const std::string salt = crypto::generateSaltHex();
    const std::string hash = crypto::hashPassword(salt, password);
    const std::string insertSql =
        "INSERT INTO t_user (id, username, password_hash, salt, nickname) VALUES (" +
        std::to_string(uid) + ", '" + conn->escapeString(username) + "', '" + hash + "', '" +
        salt + "', '" + conn->escapeString(username) + "')";
    if (!conn->execute(insertSql)) {
        return lingxi::http::makeErrorResponse(bhttp::status::internal_server_error, 500, "db insert failed");
    }

    LX_LOG_INFO("user registered: uid={} username={}", uid, username);
    return lingxi::http::makeJsonResponse(
        bhttp::status::ok,
        "{\"err_code\":0,\"err_msg\":\"ok\",\"uid\":" + std::to_string(uid) + "}");
}

lingxi::http::HttpResponse GateServer::handleLogin(
    const bhttp::request<bhttp::string_body>& request, const std::string& clientIp) {
    if (!allowRequest(clientIp)) {
        return lingxi::http::makeErrorResponse(bhttp::status::too_many_requests, 429, "too many requests");
    }

    auto body = parseJsonBody(request);
    if (body == std::nullopt || !body->is_object()) {
        return lingxi::http::makeErrorResponse(bhttp::status::bad_request, 400, "invalid json");
    }
    const std::string username = body->value("username", "");
    const std::string password = body->value("password", "");

    auto conn = m_dbPool->acquire();
    if (conn == nullptr) {
        return lingxi::http::makeErrorResponse(bhttp::status::service_unavailable, 503, "db busy");
    }

    int64_t uid = 0;
    std::string passwordHash;
    std::string salt;
    {
        db::MySqlResult result;
        const std::string sql =
            "SELECT id, password_hash, salt FROM t_user WHERE username='" +
            conn->escapeString(username) + "'";
        if (!conn->query(sql, result) || !result.next()) {
            // 用户不存在与密码错误统一返回，避免用户名枚举
            return lingxi::http::makeErrorResponse(bhttp::status::unauthorized, 401,
                                           "username or password wrong");
        }
        uid = result.getInt64(0);
        passwordHash = result.getString(1);
        salt = result.getString(2);
    }
    if (!crypto::verifyPassword(salt, password, passwordHash)) {
        return lingxi::http::makeErrorResponse(bhttp::status::unauthorized, 401,
                                       "username or password wrong");
    }

    // RPC 请求 Status 分配 ChatServer 并签发 token（docs/01 §5.1）
    AllocateChatServerRequest allocateReq;
    allocateReq.set_uid(uid);
    AllocateChatServerResponse allocateRsp;
    try {
        if (!allocateRsp.ParseFromString(m_statusRpc->call(
                rpc::kServiceStatus, 0x04, allocateReq.SerializeAsString()))) {
            throw rpc::RpcError("bad allocate response");
        }
    } catch (const rpc::RpcError& e) {
        LX_LOG_ERROR("allocate chatserver failed: {}", e.what());
        return lingxi::http::makeErrorResponse(bhttp::status::service_unavailable, 503,
                                       "status server unavailable");
    }
    if (allocateRsp.err_code() != 0) {
        return lingxi::http::makeErrorResponse(bhttp::status::service_unavailable, 503,
                                       allocateRsp.err_msg().empty() ? "no chat server"
                                                                     : allocateRsp.err_msg());
    }

    // 登录流水落库（审计）
    conn->execute("INSERT INTO t_login_session (id, uid, token, login_ip) VALUES (" +
                  std::to_string(m_idGenerator->nextId()) + ", " + std::to_string(uid) + ", '" +
                  conn->escapeString(allocateRsp.token()) + "', '" +
                  conn->escapeString(clientIp) + "')");

    LX_LOG_INFO("login ok: uid={} chatServer#{} {}:{}", uid, allocateRsp.chat_server_id(),
                allocateRsp.chat_host(), allocateRsp.chat_port());
    return lingxi::http::makeJsonResponse(
        bhttp::status::ok,
        "{\"err_code\":0,\"err_msg\":\"ok\",\"uid\":" + std::to_string(uid) +
            ",\"token\":\"" + allocateRsp.token() + "\",\"chat_host\":\"" +
            allocateRsp.chat_host() + "\",\"chat_port\":" +
            std::to_string(allocateRsp.chat_port()) + "}");
}

} // namespace lingxi
