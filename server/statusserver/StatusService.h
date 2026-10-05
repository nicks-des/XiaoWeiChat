/**
 * @file StatusService.h
 * @brief StatusServer 业务：服务注册表、心跳摘除、HMAC token 签发/校验、ChatServer 分配。
 *
 * 实现要点（docs/01 §3.2）：
 * - 注册表为内存权威 + 90s 心跳窗口判活（3 × 30s）；
 * - token 无状态：base 格式 "uid.expire.hmac"，签名 HMAC-SHA256(secret, "uid.expire")；
 * - 分配策略：在线 ChatServer 中取最小负载（M1 无 Redis 依赖）。
 */
#pragma once

#include <map>
#include <mutex>
#include <string>

#include "lingxi/base/NonCopyable.h"

namespace lingxi {

namespace rpc {
class RpcServer;  ///< 前向声明（common/rpc）
}

/**
 * @brief StatusServer 核心服务。
 */
class StatusService : private NonCopyable {
public:
    /**
     * @brief 从 Config 读取配置并构造（config/statusserver 段）。
     */
    static StatusService& instance();

    /**
     * @brief 向 RPC 服务端注册全部处理器（serviceId=0x01）。
     */
    void registerHandlers(class rpc::RpcServer& server);

private:
    StatusService() = default;

    /* ---- RPC 方法实现（入参/出参均为 protobuf 字节） ---- */
    std::string handleRegister(const std::string& payload);       // 0x01
    std::string handleHeartbeat(const std::string& payload);      // 0x02
    std::string handleVerifyToken(const std::string& payload);    // 0x03
    std::string handleAllocateChatServer(const std::string& payload); // 0x04

    /**
     * @brief 签发访问令牌。
     * @param uid      用户 ID
     * @param expireMs 输出：过期时间戳（毫秒）
     * @return std::string token 字符串
     */
    std::string issueToken(int64_t uid, int64_t& expireMs);

    /**
     * @brief 校验令牌（签名一致且未过期）。
     */
    bool verifyToken(int64_t uid, const std::string& token) const;

    /**
     * @brief 单个 ChatServer 实例的注册信息。
     */
    struct ChatNode {
        std::string clientHost;   ///< 面向客户端地址
        int clientPort = 0;       ///< 面向客户端端口
        std::string rpcHost;      ///< RPC 地址
        int rpcPort = 0;
        int load = 0;             ///< 当前连接数
        int64_t lastSeenMs = 0;   ///< 最近心跳时间
    };

    /**
     * @brief 在存活节点中选取最小负载的 ChatServer。
     * @param outNodeId 输出选中的实例编号
     * @return const ChatNode* 无可用节点返回 nullptr
     */
    const ChatNode* pickChatNode(int32_t& outNodeId);

    std::map<int32_t, ChatNode> m_chatNodes;  ///< ChatServer 注册表
    mutable std::mutex m_mutex;               ///< 注册表互斥锁
    std::string m_hmacSecret;                 ///< token 签名密钥
    int64_t m_tokenTtlMs = 7 * 24 * 3600 * 1000LL; ///< token 有效期
    int64_t m_nodeTimeoutMs = 90 * 1000LL;    ///< 节点判活窗口
};

} // namespace lingxi
