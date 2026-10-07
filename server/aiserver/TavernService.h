/**
 * @file TavernService.h
 * @brief M6 酒馆编排服务：SubmitChat 任务 → Prompt 组装 → LLM → 流式推送 + 占位改写 + 好感度。
 */
#pragma once

#include <cstdint>
#include <string>

#include "lingxi/base/NonCopyable.h"
#include "lingxi/base/SnowflakeIdGenerator.h"
#include "lingxi/base/ThreadPool.h"
#include "lingxi/db/MySqlPool.h"
#include "lingxi/rpc/RpcClient.h"
#include "lingxi/rpc/RpcServer.h"

#include "LlmGateway.h"

namespace lingxi {

/**
 * @brief 酒馆业务服务（AIServer 进程核心）。
 */
class TavernService : private NonCopyable {
public:
    TavernService(db::MySqlConnectionPool* db, SnowflakeIdGenerator* idGen,
                  ThreadPool& handlerPool);

    /**
     * @brief 向 RPC 服务端注册处理器（serviceId=0x04）。
     */
    void registerHandlers(rpc::RpcServer& server);

    /**
     * @brief 注入推送通道（指向 ChatServer 的 RPC 客户端池）。
     */
    void setPushChannel(rpc::RpcClientPool* pushChannel, int64_t chatServerId);

private:
    /**
     * @brief SubmitChat（0x01）：任务入线程池，立即回执。
     */
    std::string handleSubmitChat(const std::string& payloadBytes);

    /**
     * @brief 实际生成编排：取角色/世界书/历史 → Prompt → LLM → 流式推送 → 改写占位 → 好感度。
     */
    void runChatTask(int64_t convId, int64_t aiUid, int64_t userUid, int64_t placeholderSeq,
                     int64_t triggerSeq);

    /**
     * @brief 组装 Prompt：人设区 + 世界书触发注入 + 近期窗口 + 输出契约（docs/04 §5）。
     */
    PromptPack buildPrompt(int64_t aiUid, int64_t convId, int64_t triggerSeq);

    /**
     * @brief 经 ChatServer PushToUid 推送流式帧给用户。
     */
    void pushToUser(int64_t userUid, uint16_t msgId, const std::string& body);

    db::MySqlConnectionPool* m_db;
    SnowflakeIdGenerator* m_idGen;
    ThreadPool& m_taskPool;                ///< 生成任务执行池
    rpc::RpcClientPool* m_pushChannel = nullptr;  ///< ChatServer RPC（推送通道）
    int64_t m_chatServerId = 1;
    LlmGateway m_llm;                      ///< LLM 网关
    int m_historyWindow = 8;               ///< 近期窗口轮数
};

} // namespace lingxi
