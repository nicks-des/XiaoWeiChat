/**
 * @file StatusService.cpp
 * @brief StatusServer 业务实现。
 */
#include "StatusService.h"

#include "lingxi/base/TimeUtil.h"
#include "lingxi/config/Config.h"
#include "lingxi/crypto/Crypto.h"
#include "lingxi/logging/Logger.h"
#include "lingxi/rpc/RpcServer.h"
#include "lingxi/rpc/RpcFrame.h"

#include "lingxi.pb.h"

namespace lingxi {

StatusService& StatusService::instance() {
    static StatusService s_service;
    return s_service;
}

void StatusService::registerHandlers(rpc::RpcServer& server) {
    auto& config = Config::instance();
    m_hmacSecret = config.get<std::string>("statusserver.hmacSecret", "dev-secret-change-me");
    m_tokenTtlMs = config.get<long long>("statusserver.tokenTtlMs", 7LL * 24 * 3600 * 1000);

    server.registerHandler(rpc::kServiceStatus, 0x01,
                           [this](const std::string& payload) { return handleRegister(payload); });
    server.registerHandler(rpc::kServiceStatus, 0x02,
                           [this](const std::string& payload) { return handleHeartbeat(payload); });
    server.registerHandler(rpc::kServiceStatus, 0x03,
                           [this](const std::string& payload) { return handleVerifyToken(payload); });
    server.registerHandler(rpc::kServiceStatus, 0x04,
                           [this](const std::string& payload) {
                               return handleAllocateChatServer(payload);
                           });
    LX_LOG_INFO("StatusService handlers registered (tokenTtl={}ms)", m_tokenTtlMs);
}

std::string StatusService::handleRegister(const std::string& payload) {
    RegisterRequest request;
    RegisterResponse response;
    if (!request.ParseFromString(payload)) {
        response.set_err_code(400);
        response.set_err_msg("bad register request");
        return response.SerializeAsString();
    }

    if (request.server_type() == SERVER_CHAT) {
        std::lock_guard<std::mutex> lock(m_mutex);
        ChatNode& node = m_chatNodes[request.server_id()];
        node.clientHost = request.client_host();
        node.clientPort = request.client_port();
        node.rpcHost = request.rpc_host();
        node.rpcPort = request.rpc_port();
        node.load = request.load();
        node.lastSeenMs = TimeUtil::nowMs();
    }

    LX_LOG_INFO("server registered: type={} id={} load={}", request.server_type(),
                request.server_id(), request.load());
    response.set_err_code(0);
    return response.SerializeAsString();
}

std::string StatusService::handleHeartbeat(const std::string& payload) {
    HeartbeatRequest request;
    HeartbeatResponse response;
    if (!request.ParseFromString(payload)) {
        response.set_err_code(400);
        return response.SerializeAsString();
    }

    if (request.server_type() == SERVER_CHAT) {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_chatNodes.find(request.server_id());
        if (it != m_chatNodes.end()) {
            it->second.load = request.load();
            it->second.lastSeenMs = TimeUtil::nowMs();
        }
    }
    response.set_err_code(0);
    return response.SerializeAsString();
}

std::string StatusService::handleVerifyToken(const std::string& payload) {
    VerifyTokenRequest request;
    VerifyTokenResponse response;
    if (!request.ParseFromString(payload) || !verifyToken(request.uid(), request.token())) {
        response.set_err_code(401);
        response.set_err_msg("token invalid or expired");
        return response.SerializeAsString();
    }
    response.set_err_code(0);
    response.set_uid(request.uid());
    return response.SerializeAsString();
}

std::string StatusService::handleAllocateChatServer(const std::string& payload) {
    AllocateChatServerRequest request;
    AllocateChatServerResponse response;
    if (!request.ParseFromString(payload)) {
        response.set_err_code(400);
        response.set_err_msg("bad allocate request");
        return response.SerializeAsString();
    }

    int32_t nodeId = 0;
    const ChatNode* node = pickChatNode(nodeId);
    if (node == nullptr) {
        response.set_err_code(503);
        response.set_err_msg("no chat server available");
        return response.SerializeAsString();
    }

    int64_t expireMs = 0;
    response.set_err_code(0);
    response.set_chat_host(node->clientHost);
    response.set_chat_port(node->clientPort);
    response.set_chat_server_id(nodeId);
    response.set_token(issueToken(request.uid(), expireMs));
    response.set_token_expire_ms(expireMs);

    LX_LOG_INFO("allocated chatServer#{} {}:{} for uid {}", nodeId, node->clientHost,
                node->clientPort, request.uid());
    return response.SerializeAsString();
}

std::string StatusService::issueToken(int64_t uid, int64_t& expireMs) {
    expireMs = TimeUtil::nowMs() + m_tokenTtlMs;
    const std::string plain = std::to_string(uid) + "." + std::to_string(expireMs);
    // token 结构 "uid.expire.hmac"：校验端重算 HMAC 即可，无状态（docs/02 §6）
    return plain + "." + crypto::hmacSha256Hex(m_hmacSecret, plain);
}

bool StatusService::verifyToken(int64_t uid, const std::string& token) const {
    const auto firstDot = token.find('.');
    const auto secondDot = token.find('.', firstDot == std::string::npos ? 0 : firstDot + 1);
    if (firstDot == std::string::npos || secondDot == std::string::npos) {
        return false;
    }
    try {
        const int64_t tokenUid = std::stoll(token.substr(0, firstDot));
        const int64_t expireMs = std::stoll(token.substr(firstDot + 1, secondDot - firstDot - 1));
        if (tokenUid != uid || expireMs <= TimeUtil::nowMs()) {
            return false;
        }
        const std::string plain = token.substr(0, secondDot);
        // 常量时间比较防时序侧信道
        const std::string expected = crypto::hmacSha256Hex(m_hmacSecret, plain);
        const std::string actual = token.substr(secondDot + 1);
        if (expected.size() != actual.size()) {
            return false;
        }
        volatile unsigned char diff = 0;
        for (size_t i = 0; i < expected.size(); ++i) {
            diff |= static_cast<unsigned char>(expected[i]) ^ static_cast<unsigned char>(actual[i]);
        }
        return diff == 0;
    } catch (const std::exception&) {
        return false;
    }
}

const StatusService::ChatNode* StatusService::pickChatNode(int32_t& outNodeId) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const int64_t nowMs = TimeUtil::nowMs();
    const ChatNode* best = nullptr;
    for (const auto& entry : m_chatNodes) {
        if (nowMs - entry.second.lastSeenMs > m_nodeTimeoutMs) {
            continue;  // 心跳过期
        }
        if (best == nullptr || entry.second.load < best->load) {
            best = &entry.second;
            outNodeId = entry.first;
        }
    }
    return best;
}

} // namespace lingxi
