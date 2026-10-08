/**
 * @file StoryService.cpp
 * @brief M7 剧情群编排实现。
 */
#include "StoryService.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <random>
#include <sstream>

#include "lingxi/base/TimeUtil.h"
#include "lingxi/base/Uuid.h"
#include "lingxi/logging/Logger.h"
#include "lingxi/rpc/RpcFrame.h"

#include "lingxi.pb.h"

namespace lingxi {

namespace {

/** 记忆摘要触发阈值（未摘要消息条数，docs/04 §7 L2） */
constexpr int64_t kSummaryThreshold = 40;

/** 导演 JSON 输出正则兜底（非 JSON 回退为随机轮换） */

/**
 * @brief 从消息 payload JSON 提取文本。
 */
std::string extractText(const std::string& payload) {
    try {
        auto parsed = nlohmann::json::parse(payload);
        if (parsed.contains("text") && parsed["text"].is_string()) {
            return parsed["text"].get<std::string>();
        }
    } catch (const nlohmann::json::exception&) {
    }
    return "";
}

} // namespace

StoryService::StoryService(db::MySqlConnectionPool* db, SnowflakeIdGenerator* idGen,
                           ThreadPool& taskPool, LlmGateway& llm)
    : m_db(db), m_idGen(idGen), m_taskPool(taskPool), m_llm(llm) {}

void StoryService::registerHandlers(rpc::RpcServer& server) {
    server.registerHandler(
        rpc::kServiceAi, 0x02, [this](const std::string& payload) {
            return handleSubmitStoryTurn(payload);
        });
    server.registerHandler(
        rpc::kServiceAi, 0x03, [this](const std::string& payload) {
            return handleSummaryTask(payload);
        });
    LX_LOG_INFO("StoryService handlers registered (story turn + summary)");
}

void StoryService::setPushChannel(rpc::RpcClientPool* pushChannel) {
    m_pushChannel = pushChannel;
}

/* ==================== SubmitStoryTurn ==================== */

std::string StoryService::handleSubmitStoryTurn(const std::string& payloadBytes) {
    AiSubmitStoryTurnRequest request;
    AiSubmitChatResponse response;
    if (!request.ParseFromString(payloadBytes)) {
        response.set_err_code(400);
        response.set_err_msg("bad story turn");
        return response.SerializeAsString();
    }

    m_taskPool.submit([this, request] {
        auto conn = m_db->acquire();
        if (conn == nullptr) {
            return;
        }

        // 1. 读取触发消息文本
        db::MySqlResult triggerRow;
        std::string userText;
        if (conn->query("SELECT payload FROM t_message WHERE conv_id=" +
                            std::to_string(request.conv_id()) + " AND seq=" +
                            std::to_string(request.trigger_seq()),
                        triggerRow) && triggerRow.next()) {
            userText = extractText(triggerRow.getString(0));
        }

        // 2. 导演调度：选人 + beat + 旁白
        std::vector<int64_t> aiUids(request.ai_uids().begin(), request.ai_uids().end());
        auto director = runDirector(request.conv_id(), aiUids, userText);

        // 3. 旁白消息落库（MSG_NARRATION, from_uid=0）+ 推送
        if (!director.narration.empty()) {
            auto seqReply =
                conn->query("SELECT last_seq FROM t_conversation WHERE id=" +
                                std::to_string(request.conv_id()),
                            triggerRow) && triggerRow.next()
                ? triggerRow.getInt64(0) + 1 : 1;
            conn->execute("UPDATE t_conversation SET last_seq=" + std::to_string(seqReply) +
                          " WHERE id=" + std::to_string(request.conv_id()));
            nlohmann::json narrationPayload = {{"text", director.narration}};
            conn->execute("INSERT INTO t_message (conv_id, seq, msg_id, client_msg_id, "
                          "from_uid, msg_type, status, payload) VALUES (" +
                          std::to_string(request.conv_id()) + ", " + std::to_string(seqReply) +
                          ", " + std::to_string(m_idGen->nextId()) + ", '" + Uuid::generate() +
                          "', 0, " + std::to_string(MSG_NARRATION) + ", 0, '" +
                          conn->escapeString(narrationPayload.dump()) + "')");

            MsgBody body;
            body.set_conv_id(request.conv_id());
            body.set_from_uid(0);
            body.set_conv_seq(seqReply);
            body.set_msg_type(MSG_NARRATION);
            body.set_send_time_ms(TimeUtil::nowMs());
            body.set_payload(narrationPayload.dump());
            MessageNotify notify;
            *notify.mutable_body() = body;
            pushToGroup(request.conv_id(), 0x0303, notify.SerializeAsString());
        }

        // 4. 依序驱动每个 speaker
        for (int64_t aiUid : director.speakers) {
            // 预占 seq（status=2 占位）→ LLM 生成 → 占位改写 → 推送
            auto seqReply =
                conn->query("SELECT last_seq FROM t_conversation WHERE id=" +
                                std::to_string(request.conv_id()),
                            triggerRow) && triggerRow.next()
                ? triggerRow.getInt64(0) + 1 : 1;
            conn->execute("UPDATE t_conversation SET last_seq=" + std::to_string(seqReply) +
                          " WHERE id=" + std::to_string(request.conv_id()));
            driveSpeaker(request.conv_id(), aiUid, request.user_uid(), seqReply, director.beat);
        }

        // 5. 记忆摘要阈值检查（T70-01）
        db::MySqlResult extRow;
        int64_t lastSummarized = 0;
        if (conn->query("SELECT last_summarized_seq FROM t_ai_conversation_ext WHERE conv_id=" +
                            std::to_string(request.conv_id()),
                        extRow) && extRow.next()) {
            lastSummarized = extRow.getInt64(0);
        }
        db::MySqlResult countRow;
        if (conn->query("SELECT last_seq FROM t_conversation WHERE id=" +
                            std::to_string(request.conv_id()),
                        countRow) && countRow.next()) {
            const int64_t lastSeq = countRow.getInt64(0);
            if (lastSeq - lastSummarized > kSummaryThreshold) {
                // 异步摘要任务（简化：直接更新游标，摘要内容由 M7 完整实现时写入 memory_summary）
                conn->execute("UPDATE t_ai_conversation_ext SET last_summarized_seq=" +
                              std::to_string(lastSeq) + " WHERE conv_id=" +
                              std::to_string(request.conv_id()));
                LX_LOG_INFO("memory summary checkpoint: conv={} seq={}", request.conv_id(), lastSeq);
            }
        }
    });
    response.set_err_code(0);
    return response.SerializeAsString();
}

/* ==================== 导演调度（docs/04 §9.2） ==================== */

StoryService::DirectorResult StoryService::runDirector(
    int64_t convId, const std::vector<int64_t>& aiUids, const std::string& userText) {
    DirectorResult result;

    // 规则兜底：无 LLM 或 LLM 输出不合法 → 轮换顺序选 1 人 + 无旁白
    if (aiUids.empty()) {
        return result;
    }

    // M7：LLM 导演调用（一次调用选人 + beat + 旁白 JSON）
    // mock provider 简化：随机选 1 人 + 固定 beat + 无旁白
    // M7 完整版：prompt 前置导演指令 + JSON 输出契约
    static std::mt19937 generator{std::random_device{}()};
    const int64_t chosen = aiUids[generator() % aiUids.size()];
    result.speakers.push_back(chosen);
    result.beat = "自然推进剧情";

    // mock 旁白：第一条消息时给场景描述
    auto conn = m_db->acquire();
    if (conn != nullptr) {
        db::MySqlResult countRow;
        if (conn->query("SELECT COUNT(*) FROM t_message WHERE conv_id=" +
                            std::to_string(convId) + " AND seq=1",
                        countRow) && countRow.next() && countRow.getInt64(0) > 0) {
            // 非首回合：不产生旁白
        } else {
            result.narration = "夜色渐深，酒馆的烛火摇曳，众人围坐在长桌旁。";
        }
    }
    return result;
}

/* ==================== 驱动 AI 发言 ==================== */

void StoryService::driveSpeaker(int64_t convId, int64_t aiUid, int64_t userUid,
                                int64_t placeholderSeq, const std::string& beat) {
    auto conn = m_db->acquire();
    if (conn == nullptr) {
        return;
    }

    // 查角色
    db::MySqlResult charRow;
    std::string charName = "AI";
    std::string description;
    std::string personality;
    if (conn->query("SELECT name, IFNULL(description,''), IFNULL(personality,'') FROM "
                    "t_ai_character WHERE uid=" + std::to_string(aiUid),
                    charRow) && charRow.next()) {
        charName = charRow.getString(0);
        description = charRow.getString(1);
        personality = charRow.getString(2);
    }

    // 占位落库
    nlohmann::json placeholderPayload = {{"text", ""}, {"versions", nlohmann::json::array()},
                                         {"activeIndex", 0}, {"msgId", placeholderSeq}};
    if (!conn->execute("INSERT INTO t_message (conv_id, seq, msg_id, client_msg_id, from_uid, "
                       "msg_type, status, payload) VALUES (" +
                       std::to_string(convId) + ", " + std::to_string(placeholderSeq) + ", " +
                       std::to_string(m_idGen->nextId()) + ", '" + Uuid::generate() + "', " +
                       std::to_string(aiUid) + ", " + std::to_string(MSG_TEXT) + ", 2, '" +
                       conn->escapeString(placeholderPayload.dump()) + "')")) {
        return;
    }

    // Prompt：角色人设 + beat + 群聊上下文
    PromptPack prompt;
    std::string systemText = "你是「" + charName + "」。" + description + " 性格：" + personality +
                             "\n当前节拍：" + beat +
                             "\n用中文回复，动作用*星号*包裹，100 字以内。";
    prompt.messages.push_back({"system", systemText});

    // 近期上下文（含其他角色发言）
    db::MySqlResult rows;
    if (conn->query("SELECT from_uid, payload FROM t_message WHERE conv_id=" +
                        std::to_string(convId) + " AND status=0 ORDER BY seq DESC LIMIT 8",
                    rows)) {
        std::vector<std::pair<int64_t, std::string>> history;
        while (rows.next()) {
            history.emplace_back(rows.getInt64(0), rows.getString(1));
        }
        std::reverse(history.begin(), history.end());
        for (const auto& entry : history) {
            std::string text = extractText(entry.second);
            if (text.empty()) {
                text = "[动作/旁白]";
            }
            const bool isSelf = entry.first == aiUid;
            prompt.messages.push_back({isSelf ? "assistant" : "user", text});
        }
    }
    prompt.messages.push_back({"user", "（继续你的角色扮演， responding to beat: " + beat + "）"});

    // LLM 生成（流式推送）
    std::string finalText;
    try {
        finalText = m_llm.generate(prompt, [&](const std::string& delta, bool done, int) {
            if (done) {
                return;
            }
            AiStreamChunk chunk;
            chunk.set_conv_id(convId);
            chunk.set_target_seq(placeholderSeq);
            chunk.set_delta_text(delta);
            pushToGroup(convId, 0x0701, chunk.SerializeAsString());
        });
    } catch (const std::exception& e) {
        LX_LOG_ERROR("driveSpeaker LLM failed: {}", e.what());
        return;
    }

    int affinityDelta = 0;
    std::string cleanText = LlmGateway::stripAffinityTag(finalText, affinityDelta);
    nlohmann::json payload = {{"text", cleanText},
                              {"versions", nlohmann::json::array({cleanText})},
                              {"activeIndex", 0}, {"msgId", placeholderSeq}};
    conn->execute("UPDATE t_message SET status=0, payload='" +
                  conn->escapeString(payload.dump()) + "' WHERE conv_id=" +
                  std::to_string(convId) + " AND seq=" + std::to_string(placeholderSeq));

    // 正式消息推送
    MsgBody finalBody;
    finalBody.set_conv_id(convId);
    finalBody.set_from_uid(aiUid);
    finalBody.set_conv_seq(placeholderSeq);
    finalBody.set_msg_type(MSG_TEXT);
    finalBody.set_send_time_ms(TimeUtil::nowMs());
    finalBody.set_payload(payload.dump());
    finalBody.set_status(0);
    MessageNotify notify;
    *notify.mutable_body() = finalBody;
    pushToGroup(convId, 0x0303, notify.SerializeAsString(), aiUid);

    LX_LOG_INFO("story speaker done: conv={} ai={} seq={}", convId, aiUid, placeholderSeq);
}

/* ==================== 推送 ==================== */

void StoryService::pushToGroup(int64_t convId, uint16_t msgId, const std::string& body,
                               int64_t excludeUid) {
    if (m_pushChannel == nullptr) {
        return;
    }
    // 查群成员并逐个推送（M7 简化：不做缓存优化）
    auto conn = m_db->acquire();
    if (conn == nullptr) {
        return;
    }
    db::MySqlResult rows;
    if (!conn->query("SELECT uid FROM t_conversation_member WHERE conv_id=" +
                         std::to_string(convId),
                     rows)) {
        return;
    }
    while (rows.next()) {
        const int64_t uid = rows.getInt64(0);
        if (uid == excludeUid) {
            continue;
        }
        PushToUidRequest request;
        request.set_target_uid(uid);
        request.set_msg_id(msgId);
        request.set_msg_body(body);
        try {
            m_pushChannel->call(rpc::kServiceChat, 0x01, request.SerializeAsString());
        } catch (const rpc::RpcError& e) {
            LX_LOG_WARN("push to group member failed: uid={} err={}", uid, e.what());
        }
    }
}

/* ==================== 记忆摘要 ==================== */

std::string StoryService::handleSummaryTask(const std::string& payloadBytes) {
    AiSummaryTaskRequest request;
    AiSubmitChatResponse response;
    if (!request.ParseFromString(payloadBytes)) {
        response.set_err_code(400);
        return response.SerializeAsString();
    }
    m_taskPool.submit([this, request] {
        // M7 完整版：LLM 摘要「旧摘要 + 新片段」→ 更新 memory_summary
        // 当前实现：只推进游标（摘要内容写入排期在 M7 后期迭代）
        auto conn = m_db->acquire();
        if (conn != nullptr) {
            conn->execute("UPDATE t_ai_conversation_ext SET last_summarized_seq=" +
                          std::to_string(request.last_seq()) + " WHERE conv_id=" +
                          std::to_string(request.conv_id()));
        }
        LX_LOG_INFO("summary checkpoint: conv={} seq={}", request.conv_id(), request.last_seq());
    });
    response.set_err_code(0);
    return response.SerializeAsString();
}

} // namespace lingxi
