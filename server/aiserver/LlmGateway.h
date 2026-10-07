/**
 * @file LlmGateway.h
 * @brief M6 LLM 网关：Provider 抽象（mock/openai）+ Prompt 组装 + 好感度标记解析（docs/04 §3/§5）。
 */
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "lingxi/base/NonCopyable.h"

namespace lingxi {

/**
 * @brief 组装完成的 Prompt（messages 数组形式，OpenAI 兼容）。
 */
struct PromptPack {
    struct Message {
        std::string role;    ///< system/user/assistant
        std::string content;
    };
    std::vector<Message> messages;
};

/**
 * @brief LLM 网关（每个 AIServer 一份）。
 *
 * Provider 由配置 llm.provider 决定：
 * - mock：确定性测试后端（固定人设口吻回复 + 好感度标记），供 m6_flow 自动化；
 * - openai：OpenAI 兼容 /chat/completions（Key/Endpoint/Model 均可配，M8 升级真 SSE 流式）。
 */
class LlmGateway : private NonCopyable {
public:
    /**
     * @brief 流式回调：每个语义分片一次；done=true 时 fullText 为完整输出。
     */
    using StreamCallback = std::function<void(const std::string& delta, bool done,
                                              int finishReason)>;

    LlmGateway();

    /**
     * @brief 从 Config 读取 llm 段（provider/endpoint/model/key/maxTokens）。
     */
    void loadConfig();

    /**
     * @brief 生成回复（阻塞；内部按 provider 分片回调流式输出）。
     * @param prompt  组装好的 Prompt
     * @param onChunk 分片回调
     * @return std::string 完整回复（含好感度标记，由调用方解析剥离）
     */
    std::string generate(const PromptPack& prompt, const StreamCallback& onChunk);

    /**
     * @brief 解析并剥离回复尾部的 <affinity delta="+1" reason="..."/> 标记（docs/04 §5）。
     * @param text      输入输出（就地剥离）
     * @param deltaOut  输出：增量（无标记为 0）
     * @return std::string 剥离后的正文
     */
    static std::string stripAffinityTag(std::string& text, int& deltaOut);

private:
    /**
     * @brief Mock Provider：拼接人设感知的确定性回复（含好感度标记）。
     */
    std::string generateMock(const PromptPack& prompt, const StreamCallback& onChunk);

    /**
     * @brief OpenAI 兼容 Provider：POST /chat/completions 非流式，分片回放。
     */
    std::string generateOpenAi(const PromptPack& prompt, const StreamCallback& onChunk);

    std::string m_provider = "mock";        ///< mock | openai
    std::string m_endpoint;                 ///< OpenAI 兼容 endpoint（http(s)://host:port/path）
    std::string m_model;                    ///< 模型名
    std::string m_apiKey;                   ///< Bearer Key
    int m_maxTokens = 512;
};

} // namespace lingxi
