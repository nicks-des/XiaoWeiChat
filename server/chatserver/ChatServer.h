/**
 * @file ChatServer.h
 * @brief ChatServer 骨架（M1）：客户端 TCP 接入 + token 校验 + 在线路由 + 顶号。
 *
 * 职责（docs/01 §3.3）：
 * - 客户端长连接 8888，RPC 监听 9001；
 * - 登录：RPC 校验 token（Status）→ 绑定 uid → 写路由表 → 同账号旧连接顶号；
 * - 心跳 30s 上报负载到 Status；跨节点推送方法 PushToUid。
 */
#pragma once

#include <boost/asio.hpp>

namespace asio = boost::asio;  ///< 项目内简写（见 docs/01：全栈 boost）
using asio::ip::tcp;             ///< TCP 类型简写

#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "lingxi/base/NonCopyable.h"
#include "lingxi/base/SnowflakeIdGenerator.h"
#include "lingxi/base/ThreadPool.h"
#include "lingxi/db/MySqlPool.h"
#include "lingxi/db/RedisPool.h"
#include "lingxi/rpc/RpcClient.h"
#include "lingxi/rpc/RpcServer.h"

namespace lingxi {

class ChatSession;
class MessageService;
class SocialService;
class CallService;

/**
 * @brief ChatServer 核心服务。
 */
class ChatServer : private NonCopyable {
public:
    /**
     * @brief 构造：读取配置（config/chatserver 段）并初始化各组件。
     */
    ChatServer(asio::io_context& ioContext, ThreadPool& handlerPool);

    /**
     * @brief 析构（在 cpp 定义：unique_ptr<MessageService> 需完整类型）。
     */
    ~ChatServer();

    /**
     * @brief 启动：RPC 注册 + 客户端接受循环 + 心跳定时器。
     */
    void start();

    /**
     * @brief 登录成功后绑定会话（重复登录触发顶号）。
     * @param uid     登录用户
     * @param session 新会话
     * @return bool true=绑定成功；false=参数异常
     */
    void bindSession(int64_t uid, const std::shared_ptr<ChatSession>& session);

    /**
     * @brief 解绑会话（登出/断开），并清理路由表。
     */
    void unbindSession(int64_t uid, ChatSession* session);

    /**
     * @brief 当前在线连接数（心跳上报的负载值）。
     */
    int currentLoad() const;

    /**
     * @brief Status RPC 客户端池（ChatSession 校验 token 用）。
     */
    rpc::RpcClientPool* statusRpc() { return m_statusRpc.get(); }

    /**
     * @brief 消息内核（ChatSession 业务帧处理用）。
     */
    MessageService* messageService() { return m_messageService.get(); }

    /**
     * @brief 社交服务（好友/群组，ChatSession 业务帧处理用）。
     */
    SocialService* socialService() { return m_socialService.get(); }

    /**
     * @brief 通话信令服务（ChatSession 业务帧处理用）。
     */
    CallService* callService() { return m_callService.get(); }

    /**
     * @brief 投递路由：本机在线直推；跨节点经 Redis 路由 + RPC PushToUid；离线跳过（靠补拉）。
     * @param uid   目标用户
     * @param msgId IM 帧 msgId
     * @param body  protobuf body
     */
    void deliverToUid(int64_t uid, uint16_t msgId, const std::string& body);

    /**
     * @brief 处理器线程池（登录/业务帧等阻塞操作移出 IO 线程）。
     */
    ThreadPool& handlerPool() { return m_handlerPool; }

    /**
     * @brief AIServer RPC 客户端池（酒馆任务提交）。
     */
    rpc::RpcClientPool* aiRpc() { return m_aiRpc.get(); }

    /**
     * @brief 提交酒馆生成任务到 AIServer（异步：AIServer 内部线程池执行）。
     * @param convId          会话 ID
     * @param aiUid           AI 角色 uid
     * @param userUid         触发用户
     * @param placeholderSeq  AI 回复占位 seq
     * @param triggerSeq      触发消息 seq
     */
    void submitAiChat(int64_t convId, int64_t aiUid, int64_t userUid, int64_t placeholderSeq,
                      int64_t triggerSeq);

    /**
     * @brief 本实例编号（路由表值）。
     */
    int32_t serverId() const { return m_serverId; }

private:
    /**
     * @brief 异步接受客户端连接。
     */
    void doAccept();

    /**
     * @brief 启动 30s 心跳定时器（向 Status 上报负载）。
     */
    void startHeartbeatTimer();

    /**
     * @brief 心跳节拍：上报负载后自行续期（定时器经 shared_ptr 传递，避免悬垂）。
     */
    void doHeartbeat(const std::shared_ptr<asio::steady_timer>& timer);

    /**
     * @brief 向 StatusServer 注册自身（启动时同步调用一次）。
     * @return bool 成功为 true
     */
    bool registerToStatus();

    /**
     * @brief 通过 Redis 写路由表 route:uid:{uid} = serverId。
     */
    void writeRoute(int64_t uid);

    /**
     * @brief 通过 Redis 清理路由表（仅当值仍为本实例时）。
     */
    void clearRoute(int64_t uid);

    /**
     * @brief 顶号：向旧会话下发 KickNotice 并关闭。
     */
    void kickExisting(int64_t uid, const std::shared_ptr<ChatSession>& newSession);

    asio::io_context& m_io;
    ThreadPool& m_handlerPool;
    tcp::acceptor m_clientAcceptor;                    ///< 客户端监听器
    std::unique_ptr<rpc::RpcServer> m_rpcServer;       ///< RPC 服务端
    std::unique_ptr<rpc::RpcClientPool> m_statusRpc;   ///< Status RPC 客户端
    std::unique_ptr<db::RedisConnectionPool> m_redis;  ///< Redis 池（路由表）

    int32_t m_serverId = 1;                            ///< 实例编号
    std::string m_clientHost;                          ///< 对外地址
    unsigned short m_clientPort = 8888;                ///< 对外端口
    int m_machineId = 31;                              ///< 雪花机器号（消息内核专用）
    std::unique_ptr<db::MySqlConnectionPool> m_dbPool;      ///< MySQL 池（消息权威存储）
    std::unique_ptr<SnowflakeIdGenerator> m_idGen;          ///< msg_id 生成器
    std::unique_ptr<MessageService> m_messageService;       ///< 消息内核
    std::unique_ptr<SocialService> m_socialService;         ///< 社交服务（好友/群组）
    std::unique_ptr<CallService> m_callService;             ///< 通话信令服务
    std::mutex m_peerRpcMutex;                              ///< 跨节点 RPC 客户端表锁
    std::map<int32_t, std::unique_ptr<rpc::RpcClientPool>> m_peerRpc;  ///< serverId → 客户端池
    std::unique_ptr<rpc::RpcClientPool> m_aiRpc;            ///< AIServer RPC（酒馆任务提交）

    std::mutex m_sessionsMutex;                                        ///< 会话表锁
    std::map<int64_t, std::shared_ptr<ChatSession>> m_uidToSession;    ///< uid → 会话
    std::atomic<int32_t> m_load{0};                                    ///< 当前连接数
};

} // namespace lingxi
