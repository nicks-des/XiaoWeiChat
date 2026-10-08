/**
 * @file StoryService.h
 * @brief M7 剧情群聊编排：导演调度 + 多 AI 依序发言 + 记忆摘要触发。
 *
 * 流程（docs/04 §9）：
 *   用户在剧情群发言 → ChatServer RPC SubmitStoryTurn → 本服务：
 *   1. 导演调用（LLM 选人 + beat + 旁白）→ 旁白消息(MSG_NARRATION, from=0)
 *   2. 依序驱动每个 speaker：专属 prompt（含 beat + 其他角色近况）→ 流式生成 → 群消息落库
 *   3. 阈值触发记忆摘要任务（docs/04 §7 三级记忆 L2）
 */
#pragma once

#include <string>
#include <vector>

#include "lingxi/base/NonCopyable.h"
#include "lingxi/base/SnowflakeIdGenerator.h"
#include "lingxi/db/MySqlPool.h"
#include "lingxi/rpc/RpcClient.h"
#include "lingxi/rpc/RpcServer.h"

#include "LlmGateway.h"

namespace lingxi {

/**
 * @brief 剧情群编排服务（AIServer 内，与 TavernService 共享 LLM/DB/推送）。
 */
class StoryService : private NonCopyable {
public:
    struct Delivery {
        int64_t uid = 0;
        uint16_t msgId = 0;
        std::string body;
    };

    StoryService(db::MySqlConnectionPool* db, SnowflakeIdGenerator* idGen,
                 ThreadPool& taskPool, LlmGateway& llm);

    /**
     * @brief 注册 RPC 处理器（serviceId=0x04, methodId=0x02）。
     */
    void registerHandlers(rpc::RpcServer& server);

    /**
     * @brief 注入推送通道。
     */
    void setPushChannel(rpc::RpcClientPool* pushChannel);

    /**
     * @brief SubmitStoryTurn（0x02）：任务入池立即回执。
     */
    std::string handleSubmitStoryTurn(const std::string& payloadBytes);

    /**
     * @brief 记忆摘要任务（0x03）。
     */
    std::string handleSummaryTask(const std::string& payloadBytes);

private:
    /**
     * @brief 导演节拍：LLM 选人 + beat + 旁白（规则兜底：轮换顺序）。
     */
    struct DirectorResult {
        std::string narration;
        std::string beat;
        std::vector<int64_t> speakers;
    };
    DirectorResult runDirector(int64_t convId, const std::vector<int64_t>& aiUids,
                               const std::string& userText);

    /**
     * @brief 驱动一个 AI 角色发言（专属 prompt → 生成 → 群消息落库 → 推送）。
     */
    void driveSpeaker(int64_t convId, int64_t aiUid, int64_t userUid, int64_t placeholderSeq,
                      const std::string& beat);

    /**
     * @brief 推送帧给全群成员。
     */
    void pushToGroup(int64_t convId, uint16_t msgId, const std::string& body,
                     int64_t excludeUid = 0);

    db::MySqlConnectionPool* m_db;
    SnowflakeIdGenerator* m_idGen;
    ThreadPool& m_taskPool;
    LlmGateway& m_llm;
    rpc::RpcClientPool* m_pushChannel = nullptr;
};

} // namespace lingxi
