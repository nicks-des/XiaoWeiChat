/**
 * @file TavernService.cpp
 * @brief M6 酒馆编排服务实现。
 */
#include "TavernService.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <sstream>

#include "lingxi/base/TimeUtil.h"
#include "lingxi/base/Uuid.h"
#include "lingxi/config/Config.h"
#include "lingxi/logging/Logger.h"
#include "lingxi/rpc/RpcFrame.h"

#include "lingxi.pb.h"

namespace lingxi {

namespace {

/** 世界书注入 token 预算（字符近似，docs/04 §4.2） */
constexpr size_t kLorebookBudgetChars = 1600;

/**
 * @brief 世界书词条结构（t_ai_lorebook.entries 数组元素，docs/04 §4.1）。
 */
struct LoreEntry {
    std::vector<std::string> keys;
    std::string content;
    bool constant = false;
    int priority = 0;
    int scanDepth = 4;
    bool enabled = true;
};

/**
 * @brief 解析世界书 entries JSON。
 */
std::vector<LoreEntry> parseLoreEntries(const std::string& entriesJson) {
    std::vector<LoreEntry> entries;
    try {
        const auto parsed = nlohmann::json::parse(entriesJson);
        for (const auto& item : parsed) {
            LoreEntry entry;
            entry.constant = item.value("constant", false);
            entry.priority = item.value("priority", 0);
            entry.scanDepth = item.value("scanDepth", 4);
            entry.enabled = item.value("enabled", true);
            for (const auto& key : item.value("keys", nlohmann::json::array())) {
                entry.keys.push_back(key.get<std::string>());
            }
            entry.content = item.value("content", "");
            if (!entry.content.empty()) {
                entries.push_back(std::move(entry));
            }
        }
    } catch (const nlohmann::json::exception&) {
    }
    return entries;
}

} // namespace

TavernService::TavernService(db::MySqlConnectionPool* db, SnowflakeIdGenerator* idGen,
                             ThreadPool& handlerPool)
    : m_db(db), m_idGen(idGen), m_taskPool(handlerPool) {
    m_llm.loadConfig();
}

void TavernService::registerHandlers(rpc::RpcServer& server) {
    server.registerHandler(
        rpc::kServiceAi, 0x01, [this](const std::string& payload) {
            return handleSubmitChat(payload);
        });
    LX_LOG_INFO("TavernService handlers registered");
}

void TavernService::setPushChannel(rpc::RpcClientPool* pushChannel, int64_t chatServerId) {
    m_pushChannel = pushChannel;
    m_chatServerId = chatServerId;
}

/* ==================== 任务提交（立即回执，生成异步） ==================== */

std::string TavernService::handleSubmitChat(const std::string& payloadBytes) {
    AiSubmitChatRequest request;
    AiSubmitChatResponse response;
    if (!request.ParseFromString(payloadBytes)) {
        response.set_err_code(400);
        response.set_err_msg("bad submit");
        return response.SerializeAsString();
    }

    // 生成任务入线程池（RPC 回执立即返回，LLM 耗时不阻塞调用方）
    m_taskPool.submit([this, request] {
        runChatTask(request.conv_id(), request.ai_uid(), request.user_uid(),
                    request.placeholder_seq(), request.trigger_seq());
    });
    response.set_err_code(0);
    return response.SerializeAsString();
}

/* ==================== 生成编排 ==================== */

void TavernService::runChatTask(int64_t convId, int64_t aiUid, int64_t userUid,
                                int64_t placeholderSeq, int64_t triggerSeq) {
    auto conn = m_db->acquire();
    if (conn == nullptr) {
        LX_LOG_ERROR("runChatTask: db busy");
        return;
    }

    // 1. 角色 + 世界书
    db::MySqlResult charRow;
    if (!conn->query("SELECT name, description, personality, scenario, first_mes, system_prompt "
                     "FROM t_ai_character WHERE uid=" + std::to_string(aiUid),
                     charRow) || !charRow.next()) {
        LX_LOG_ERROR("runChatTask: character {} not found", aiUid);
        return;
    }
    const std::string charName = charRow.getString(0);
    const std::string description = charRow.getString(1);
    const std::string personality = charRow.getString(2);
    const std::string scenario = charRow.getString(3);
    const std::string firstMes = charRow.getString(4);
    const std::string systemPromptOverride = charRow.getString(5);

    // 2. Prompt 组装（T60-05）
    PromptPack prompt = buildPrompt(aiUid, convId, triggerSeq);

    // 3. LLM 生成（流式分片 → ChatServer → 用户）
    const int64_t streamStartMs = TimeUtil::nowMs();
    std::string finalText;
    try {
        finalText = m_llm.generate(prompt, [&](const std::string& delta, bool done, int finish) {
            if (done) {
                return;
            }
            AiStreamChunk chunk;
            chunk.set_conv_id(convId);
            chunk.set_target_seq(placeholderSeq);
            chunk.set_delta_text(delta);
            pushToUser(userUid, 0x0701, chunk.SerializeAsString());
        });
    } catch (const std::exception& e) {
        LX_LOG_ERROR("LLM generate failed: {}", e.what());
        AiStreamError error;
        error.set_conv_id(convId);
        error.set_target_seq(placeholderSeq);
        error.set_err_msg("白露走神了一下，再试一次？");
        pushToUser(userUid, 0x0703, error.SerializeAsString());
        return;
    }

    // 4. 好感度解析剥离（T60-06）
    int affinityDelta = 0;
    std::string cleanText = LlmGateway::stripAffinityTag(finalText, affinityDelta);

    // 5. 占位改写（status 2→0，payload 写 versions[]，docs/04 §10 Swipe 结构）
    nlohmann::json payload = {{"text", cleanText},
                              {"versions", nlohmann::json::array({cleanText})},
                              {"activeIndex", 0},
                              {"msgId", placeholderSeq},  // 以占位 seq 兼作寻址（撤回/Swipe）
                              {"finishReason", 0}};
    if (!conn->execute("UPDATE t_message SET status=0, payload='" +
                       conn->escapeString(payload.dump()) + "' WHERE conv_id=" +
                       std::to_string(convId) + " AND seq=" + std::to_string(placeholderSeq))) {
        LX_LOG_ERROR("placeholder rewrite failed: conv={} seq={}", convId, placeholderSeq);
    }

    // 6. 好感度结算（单次限幅已在解析层 ±3；每小时上限略——M7 补）
    if (affinityDelta != 0) {
        conn->execute("INSERT IGNORE INTO t_ai_conversation_ext (conv_id, affinity) VALUES (" +
                      std::to_string(convId) + ", 0)");
        conn->execute("UPDATE t_ai_conversation_ext SET affinity=affinity+(" +
                      std::to_string(affinityDelta) + ") WHERE conv_id=" +
                      std::to_string(convId));
    }

    // 7.5. 正式消息通知（占位改写后补发 0x0303 status=0，docs/04 §6 时序）
    {
        MsgBody finalBody;
        finalBody.set_conv_id(convId);
        finalBody.set_from_uid(aiUid);
        finalBody.set_conv_seq(placeholderSeq);
        finalBody.set_msg_type(MSG_TEXT);
        finalBody.set_send_time_ms(TimeUtil::nowMs());
        finalBody.set_payload(payload.dump());
        finalBody.set_status(0);
        finalBody.set_msg_id(placeholderSeq);  // 寻址复用占位 seq（payload.msgId 同值）
        MessageNotify finalNotify;
        *finalNotify.mutable_body() = finalBody;
        pushToUser(userUid, 0x0303, finalNotify.SerializeAsString());
    }

    // 8. 流式结束帧（权威全文）
    AiStreamEnd endFrame;
    endFrame.set_conv_id(convId);
    endFrame.set_target_seq(placeholderSeq);
    endFrame.set_final_text(cleanText);
    endFrame.set_finish_reason(0);
    endFrame.set_affinity_delta(affinityDelta);
    pushToUser(userUid, 0x0702, endFrame.SerializeAsString());

    LX_LOG_INFO("ai reply done: conv={} seq={} chars={} affinity={} elapsed={}ms", convId,
                placeholderSeq, cleanText.size(), affinityDelta,
                TimeUtil::nowMs() - streamStartMs);
}

/* ==================== Prompt 组装（docs/04 §5） ==================== */

PromptPack TavernService::buildPrompt(int64_t aiUid, int64_t convId, int64_t triggerSeq) {
    auto conn = m_db->acquire();

    // ---- 角色卡自查（与 runChatTask 的查询解耦） ----
    std::string charName = "AI";
    std::string description;
    std::string personality;
    std::string scenario;
    std::string systemPromptOverride;
    if (conn != nullptr) {
        db::MySqlResult charRow;
        if (conn->query("SELECT name, IFNULL(description,''), IFNULL(personality,''), "
                        "IFNULL(scenario,''), IFNULL(system_prompt,'') FROM t_ai_character "
                        "WHERE uid=" + std::to_string(aiUid),
                        charRow) && charRow.next()) {
            charName = charRow.getString(0);
            description = charRow.getString(1);
            personality = charRow.getString(2);
            scenario = charRow.getString(3);
            systemPromptOverride = charRow.getString(4);
        }
    }

    // ---- 世界书：触发 + 常驻注入（优先级降序，预算裁剪） ----
    std::string lorebookText;
    if (conn != nullptr) {
        db::MySqlResult loreRow;
        if (conn->query("SELECT entries FROM t_ai_lorebook WHERE owner_type=1 AND owner_id=" +
                            std::to_string(aiUid),
                        loreRow) && loreRow.next()) {
            auto entries = parseLoreEntries(loreRow.getString(0));

            // 触发扫描：最近 scanDepth 条消息文本
            std::string recentText;
            db::MySqlResult recent;
            if (conn->query("SELECT payload FROM t_message WHERE conv_id=" +
                                std::to_string(convId) + " ORDER BY seq DESC LIMIT 8",
                            recent)) {
                while (recent.next()) {
                    recentText += recent.getString(0);
                }
            }

            std::vector<LoreEntry> activated;
            for (auto& entry : entries) {
                if (!entry.enabled) {
                    continue;
                }
                if (entry.constant) {
                    activated.push_back(entry);
                    continue;
                }
                for (const auto& key : entry.keys) {
                    if (recentText.find(key) != std::string::npos) {
                        activated.push_back(entry);
                        break;
                    }
                }
            }
            std::sort(activated.begin(), activated.end(),
                      [](const LoreEntry& a, const LoreEntry& b) { return a.priority > b.priority; });

            size_t budget = kLorebookBudgetChars;
            for (const auto& entry : activated) {
                if (entry.content.size() > budget) {
                    break;  // 预算裁剪：低优先级直接丢弃
                }
                lorebookText += entry.content + "\n";
                budget -= entry.content.size();
            }
        }
    }

    // ---- 会话扩展：好感度阶段（注入语气约束） ----
    int affinity = 0;
    if (conn != nullptr) {
        db::MySqlResult extRow;
        if (conn->query("SELECT affinity FROM t_ai_conversation_ext WHERE conv_id=" +
                            std::to_string(convId),
                        extRow) && extRow.next()) {
            affinity = static_cast<int>(extRow.getInt64(0));
        }
    }
    std::string affinityHint = "当前关系阶段：陌生（初识，语气礼貌而带一点距离感）。";
    if (affinity >= 20) {
        affinityHint = "当前关系阶段：熟识（语气自然，愿意多聊几句）。";
    }
    if (affinity >= 60) {
        affinityHint = "当前关系阶段：信赖（语气温暖，会主动关心对方）。";
    }
    if (affinity >= 100) {
        affinityHint = "当前关系阶段：挚友（语气亲昵，可以开玩笑）。";
    }

    // ---- system 区 ----
    PromptPack prompt;
    std::string systemText;
    if (!systemPromptOverride.empty()) {
        systemText = systemPromptOverride;
    } else {
        systemText = "你是「" + charName + "」，正在与用户进行沉浸式角色扮演聊天。\n"
                     "【人物设定】" + description + "\n【性格】" + personality +
                     "\n【场景】" + scenario + "\n";
        if (!lorebookText.empty()) {
            systemText += "【世界设定】" + lorebookText;
        }
        systemText += "【关系】" + affinityHint;
        systemText += "【输出规范】用中文，动作与神态用*星号*包裹，保持角色扮演不出戏；"
                      "回复控制在 100 字以内；回复末尾必须追加好感度标记 "
                      "<affinity delta=\"+1\" reason=\"简短原因\"/>（delta 范围 -3 到 +3，"
                      "根据本轮对话情绪取值，且永远放在最后）。";
    }
    prompt.messages.push_back({"system", systemText});

    // ---- 历史窗口（最近 m_historyWindow 条，正序） ----
    if (conn != nullptr) {
        db::MySqlResult rows;
        if (conn->query("SELECT from_uid, payload FROM t_message WHERE conv_id=" +
                            std::to_string(convId) + " AND status=0 ORDER BY seq DESC LIMIT " +
                            std::to_string(m_historyWindow),
                        rows)) {
            std::vector<std::pair<int64_t, std::string>> history;
            while (rows.next()) {
                history.emplace_back(rows.getInt64(0), rows.getString(1));
            }
            std::reverse(history.begin(), history.end());
            for (const auto& entry : history) {
                std::string text = "[消息]";
                try {
                    auto parsed = nlohmann::json::parse(entry.second);
                    if (parsed.contains("text") && parsed["text"].is_string()) {
                        text = parsed["text"].get<std::string>();
                    }
                } catch (const nlohmann::json::exception&) {
                }
                const bool fromAi = entry.first == aiUid;
                prompt.messages.push_back({fromAi ? "assistant" : "user", text});
            }
        }
    }
    return prompt;
}

/* ==================== 推送通道 ==================== */

void TavernService::pushToUser(int64_t userUid, uint16_t msgId, const std::string& body) {
    if (m_pushChannel == nullptr) {
        LX_LOG_WARN("push channel not ready, drop msgId={:#06x}", msgId);
        return;
    }
    // 复用 ChatServer 的 PushToUidRequest RPC（msgId 承载 0x0701/0x0702 流式帧）
    PushToUidRequest request;
    request.set_target_uid(userUid);
    request.set_msg_id(msgId);
    request.set_msg_body(body);
    try {
        m_pushChannel->call(rpc::kServiceChat, 0x01, request.SerializeAsString());
    } catch (const rpc::RpcError& e) {
        LX_LOG_WARN("push to chatserver failed: {}", e.what());
    }
}

} // namespace lingxi
