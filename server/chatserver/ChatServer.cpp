/**
 * @file ChatServer.cpp
 * @brief ChatServer 骨架实现（M1）。
 */
#include "ChatServer.h"

#include "ChatSession.h"
#include "lingxi/base/TimeUtil.h"
#include "lingxi/config/Config.h"
#include "lingxi/logging/Logger.h"
#include "lingxi/rpc/RpcFrame.h"

#include "lingxi.pb.h"

namespace lingxi {

ChatServer::ChatServer(asio::io_context& ioContext, ThreadPool& handlerPool)
    : m_io(ioContext), m_handlerPool(handlerPool),
      m_clientAcceptor(ioContext, {asio::ip::tcp::v4(), 8888}) {
    auto& config = Config::instance();
    m_serverId = static_cast<int32_t>(config.get<int>("chatserver.serverId", 1));
    m_clientHost = config.get<std::string>("chatserver.host", "127.0.0.1");
    m_clientPort = static_cast<unsigned short>(config.get<int>("chatserver.clientPort", 8888));

    // 监听端口以配置为准（acceptor 已用默认端口构造，此处重开）
    boost::system::error_code ec;
    m_clientAcceptor.close(ec);
    const tcp::endpoint endpoint(asio::ip::tcp::v4(), m_clientPort);
    m_clientAcceptor.open(endpoint.protocol());
    m_clientAcceptor.set_option(asio::socket_base::reuse_address(true));
    m_clientAcceptor.bind(endpoint);
    m_clientAcceptor.listen(asio::socket_base::max_listen_connections);

    m_rpcServer = std::make_unique<rpc::RpcServer>(
        ioContext, static_cast<unsigned short>(config.get<int>("chatserver.rpcPort", 9001)),
        handlerPool);
    m_statusRpc = std::make_unique<rpc::RpcClientPool>(
        config.get<std::string>("statusserver.host", "127.0.0.1"),
        static_cast<unsigned short>(config.get<int>("statusserver.rpcPort", 9000)), 2);
    m_redis = std::make_unique<db::RedisConnectionPool>([&] {
        db::RedisOptions options;
        options.host = config.get<std::string>("redis.host", "127.0.0.1");
        options.port = config.get<int>("redis.port", 6379);
        options.password = config.get<std::string>("redis.password", "");
        options.poolSize = config.get<int>("redis.poolSize", 8);
        return options;
    }());
}

void ChatServer::start() {
    // RPC：跨节点推送
    m_rpcServer->registerHandler(
        rpc::kServiceChat, 0x01, [this](const std::string& payload) -> std::string {
            PushToUidRequest request;
            PushToUidResponse response;
            if (!request.ParseFromString(payload)) {
                response.set_err_code(400);
                return response.SerializeAsString();
            }
            std::shared_ptr<ChatSession> session;
            {
                std::lock_guard<std::mutex> lock(m_sessionsMutex);
                auto it = m_uidToSession.find(request.target_uid());
                if (it != m_uidToSession.end()) {
                    session = it->second;
                }
            }
            if (session == nullptr) {
                response.set_online(false);
                response.set_err_code(0);
                return response.SerializeAsString();
            }
            session->sendFrame(static_cast<uint16_t>(request.msg_id()),
                               request.msg_body());
            response.set_online(true);
            response.set_err_code(0);
            return response.SerializeAsString();
        });
    m_rpcServer->start();

    if (!registerToStatus()) {
        LX_LOG_ERROR("register to status failed (will retry via heartbeat)");
    }
    startHeartbeatTimer();
    doAccept();
}

void ChatServer::doAccept() {
    m_clientAcceptor.async_accept([this](boost::system::error_code ec, tcp::socket socket) {
        if (ec) {
            LX_LOG_WARN("ChatServer accept failed: {}", ec.message());
        } else {
            const auto peer = socket.remote_endpoint();
            static std::atomic<uint64_t> sessionSeq{0};
            auto session = std::make_shared<ChatSession>(std::move(socket), *this,
                                                         ++sessionSeq);
            LX_LOG_INFO("client connected: {}:{} sessionId={}", peer.address().to_string(),
                        peer.port(), sessionSeq.load());
            session->start();
        }
        doAccept();
    });
}

void ChatServer::startHeartbeatTimer() {
    // 定时器以 shared_ptr 携带，经成员函数递归续期（避免捕获栈上 lambda 的悬垂引用）
    auto timer = std::make_shared<asio::steady_timer>(m_io);
    doHeartbeat(timer);
}

void ChatServer::doHeartbeat(const std::shared_ptr<asio::steady_timer>& timer) {
    timer->expires_after(std::chrono::seconds(30));
    timer->async_wait([this, timer](const boost::system::error_code& ec) {
        if (ec) {
            return;  // 定时器被取消
        }
        HeartbeatRequest request;
        request.set_server_type(SERVER_CHAT);
        request.set_server_id(m_serverId);
        request.set_load(currentLoad());
        try {
            m_statusRpc->call(rpc::kServiceStatus, 0x02, request.SerializeAsString());
        } catch (const rpc::RpcError& e) {
            LX_LOG_WARN("heartbeat failed: {}", e.what());
        }
        doHeartbeat(timer);
    });
}

bool ChatServer::registerToStatus() {
    RegisterRequest request;
    request.set_server_type(SERVER_CHAT);
    request.set_server_id(m_serverId);
    request.set_rpc_host(m_clientHost);  // M1 单机：RPC 与客户端同址
    request.set_rpc_port(static_cast<int32_t>(
        Config::instance().get<int>("chatserver.rpcPort", 9001)));
    request.set_client_host(m_clientHost);
    request.set_client_port(m_clientPort);
    request.set_load(currentLoad());

    RegisterResponse response;
    try {
        if (!response.ParseFromString(
                m_statusRpc->call(rpc::kServiceStatus, 0x01, request.SerializeAsString()))) {
            return false;
        }
    } catch (const rpc::RpcError& e) {
        LX_LOG_ERROR("register rpc error: {}", e.what());
        return false;
    }
    LX_LOG_INFO("registered to status: id={}", m_serverId);
    return response.err_code() == 0;
}

void ChatServer::bindSession(int64_t uid, const std::shared_ptr<ChatSession>& session) {
    std::shared_ptr<ChatSession> oldSession;
    {
        std::lock_guard<std::mutex> lock(m_sessionsMutex);
        auto it = m_uidToSession.find(uid);
        if (it != m_uidToSession.end()) {
            oldSession = it->second;  // 旧会话将被顶号
        }
        m_uidToSession[uid] = session;
    }
    m_load.fetch_add(1);
    writeRoute(uid);

    if (oldSession != nullptr) {
        kickExisting(uid, oldSession);
    }
}

void ChatServer::kickExisting(int64_t uid, const std::shared_ptr<ChatSession>& oldSession) {
    LX_LOG_WARN("kick existing session: uid={}", uid);
    KickNotice notice;
    notice.set_device_id("new login");
    notice.set_login_ip("-");
    notice.set_login_time(TimeUtil::nowMs());
    oldSession->sendFrameThenClose(0x0104, notice.SerializeAsString());
}

void ChatServer::unbindSession(int64_t uid, ChatSession* session) {
    if (uid <= 0) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_sessionsMutex);
        auto it = m_uidToSession.find(uid);
        if (it != m_uidToSession.end() && it->second.get() == session) {
            m_uidToSession.erase(it);
            m_load.fetch_sub(1);
        } else {
            return;  // 已被新会话顶替，不动路由
        }
    }
    clearRoute(uid);
}

int ChatServer::currentLoad() const {
    return m_load.load();
}

void ChatServer::writeRoute(int64_t uid) {
    auto conn = m_redis->acquire();
    if (conn == nullptr) {
        LX_LOG_WARN("writeRoute: redis busy, uid={}", uid);
        return;
    }
    const std::string key = "route:uid:" + std::to_string(uid);
    conn->exec("SET %s %s EX 120", key.c_str(), std::to_string(m_serverId).c_str());
}

void ChatServer::clearRoute(int64_t uid) {
    auto conn = m_redis->acquire();
    if (conn == nullptr) {
        return;
    }
    const std::string key = "route:uid:" + std::to_string(uid);
    auto reply = conn->exec("GET %s", key.c_str());
    if (reply.ok() && reply.str() == std::to_string(m_serverId)) {
        conn->exec("DEL %s", key.c_str());
    }
}

} // namespace lingxi
