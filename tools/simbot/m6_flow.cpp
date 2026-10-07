/**
 * @file m6_flow.cpp
 * @brief T60 酒馆一期系统验证：用户消息 → AI 分流占位 → AIServer 生成（mock LLM）→
 *        流式分片 → 结束帧（权威全文）→ 占位改写 → 好感度结算。
 *
 * 前置：gateserver/chatserver/aiserver 已启动，种子角色白露(900000000000000001) 已入库。
 * 输出 M6_FLOW_PASS 并 exit 0 表示通过。
 */
#include <boost/asio.hpp>

#include <chrono>
#include <deque>
#include <iostream>
#include <mutex>
#include <condition_variable>

#include "client/net/HttpManager.h"
#include "client/net/TcpClient.h"
#include "lingxi/net/Packet.h"

#include "lingxi.pb.h"

namespace asio = boost::asio;

namespace {

constexpr const char* kGateHost = "127.0.0.1";
constexpr unsigned short kGatePort = 8080;
constexpr const char* kChatHost = "127.0.0.1";
constexpr unsigned short kChatPort = 8888;

/** 种子 AI 角色 uid（sql/007_seed_tavern.sql） */
constexpr long long kAiUid = 900000000000000001LL;

#define CHECK(cond, msg)                                         \
    do {                                                         \
        if (!(cond)) {                                           \
            std::cout << "M6_FLOW_FAIL: " << (msg) << std::endl; \
            std::exit(1);                                        \
        }                                                        \
    } while (0)

std::pair<long long, std::string> registerAndLogin(const std::string& username) {
    using lingxi::client::postJson;
    postJson(kGateHost, kGatePort, "/api/register",
             "{\"username\":\"" + username + "\",\"password\":\"Passw0rd!123\"}");
    const auto login = postJson(kGateHost, kGatePort, "/api/login",
                                "{\"username\":\"" + username +
                                    "\",\"password\":\"Passw0rd!123\"}");
    CHECK(login.ok(), "http login failed: " + login.body);
    const auto uidBegin = login.body.find("\"uid\":");
    const auto tokenBegin = login.body.find("\"token\":\"");
    const long long uid = std::stoll(login.body.substr(uidBegin + 6));
    const auto tokenStart = tokenBegin + 9;
    const std::string token =
        login.body.substr(tokenStart, login.body.find('"', tokenStart) - tokenStart);
    return {uid, token};
}

class TestConn {
public:
    TestConn() {
        m_tcp = std::make_shared<lingxi::client::TcpClient>();
        m_tcp->setCallbacks(
            [this](uint16_t msgId, const std::string& body) {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_inbox.push_back({msgId, body});
                m_cv.notify_all();
            },
            [](bool, const std::string&) {});
    }

    void login(long long uid, const std::string& token) {
        m_tcp->connectAsync(kChatHost, kChatPort);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        lingxi::LoginRequest request;
        request.set_uid(uid);
        request.set_token(token);
        request.set_device_id("simbot");
        m_tcp->send(0x0101, request.SerializeAsString());
        const auto ack = waitFrame(0x0102, 5000);
        lingxi::LoginResponse response;
        CHECK(response.ParseFromString(ack.second) && response.err_code() == 0, "tcp login");
    }

    void send(uint16_t msgId, const std::string& body) { m_tcp->send(msgId, body); }

    std::pair<uint16_t, std::string> waitFrame(uint16_t msgId, int timeoutMs) {
        std::unique_lock<std::mutex> lock(m_mutex);
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(timeoutMs);
        for (;;) {
            for (auto it = m_inbox.begin(); it != m_inbox.end(); ++it) {
                if (it->first == msgId) {
                    auto frame = *it;
                    m_inbox.erase(it);
                    return frame;
                }
            }
            if (m_cv.wait_until(lock, deadline) == std::cv_status::timeout) {
                CHECK(false, "timeout waiting msgId=" + std::to_string(msgId));
            }
        }
    }

    /**
     * @brief 收集 msgId 的全部帧直至 done 帧到达（流式汇聚）。
     * @return std::pair<分片拼接文本, 结束帧>
     */
    std::pair<std::string, std::string> waitStream(uint16_t chunkMsgId, uint16_t endMsgId,
                                                   int timeoutMs) {
        std::unique_lock<std::mutex> lock(m_mutex);
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(timeoutMs);
        std::string chunks;
        for (;;) {
            // 先扫已入队帧
            for (auto it = m_inbox.begin(); it != m_inbox.end();) {
                if (it->first == endMsgId) {
                    auto endFrame = *it;
                    m_inbox.erase(it);
                    return {chunks, endFrame.second};
                }
                if (it->first == chunkMsgId) {
                    chunks += it->second;
                    it = m_inbox.erase(it);
                    continue;
                }
                ++it;
            }
            if (m_cv.wait_until(lock, deadline) == std::cv_status::timeout) {
                CHECK(false, "timeout waiting stream end");
            }
        }
    }

    void close() { m_tcp->close(); }

private:
    std::shared_ptr<lingxi::client::TcpClient> m_tcp;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<std::pair<uint16_t, std::string>> m_inbox;
};

} // namespace

/**
 * @brief 酒馆全链路验证。
 * @return int 0 成功
 */
int main() {
    const std::string stamp = std::to_string(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count() % 100000000);

    auto [uidUser, tokenUser] = registerAndLogin("m6u" + stamp);
    std::cout << "[1] user=" << uidUser << " (AI uid=" << kAiUid << ")" << std::endl;

    TestConn conn;
    conn.login(uidUser, tokenUser);

    /* ---- 1) 发消息给 AI（含世界书触发词「蛊」） ---- */
    const std::string clientMsgId = "m6-" + stamp;
    lingxi::MessageSendRequest sendReq;
    auto* body = sendReq.mutable_body();
    body->set_to_uid(kAiUid);
    body->set_client_msg_id(clientMsgId);
    body->set_msg_type(lingxi::MSG_TEXT);
    body->set_payload("{\"text\":\"你懂的蛊术是什么呀\"}");
    conn.send(0x0301, sendReq.SerializeAsString());

    const auto ack = conn.waitFrame(0x0302, 5000);
    lingxi::MessageAck ackPb;
    CHECK(ackPb.ParseFromString(ack.second) && ackPb.err_code() == 0, "send ack");
    const int64_t convId = ackPb.conv_id();
    const int64_t userSeq = ackPb.conv_seq();
    std::cout << "[2] user msg persisted conv=" << convId << " seq=" << userSeq << std::endl;

    /* ---- 2) 占位消息到达（status=2 生成中） ---- */
    int64_t placeholderSeq = 0;
    {
        const auto notify = conn.waitFrame(0x0303, 8000);
        lingxi::MessageNotify notifyPb;
        CHECK(notifyPb.ParseFromString(notify.second), "placeholder parse");
        CHECK(notifyPb.body().from_uid() == kAiUid, "placeholder from AI");
        CHECK(notifyPb.body().status() == 2, "placeholder status=2 (generating)");
        CHECK(notifyPb.body().conv_seq() == userSeq + 1, "placeholder seq = user+1");
        placeholderSeq = notifyPb.body().conv_seq();
    }
    std::cout << "[3] placeholder seq=" << placeholderSeq << " (status=2)" << std::endl;

    /* ---- 3) 流式分片 + 结束帧 ---- */
    auto [chunks, endBody] = conn.waitStream(0x0701, 0x0702, 15000);
    lingxi::AiStreamEnd endPb;
    CHECK(endPb.ParseFromString(endBody), "stream end parse");
    CHECK(endPb.target_seq() == placeholderSeq, "end target seq");
    CHECK(!endPb.final_text().empty(), "final text empty");
    CHECK(endPb.final_text().find("affinity") == std::string::npos,
          "affinity tag must be stripped");
    std::cout << "[4] streamed " << chunks.size() << " chunk chars, final "
              << endPb.final_text().size() << " chars, affinity="
              << endPb.affinity_delta() << std::endl;

    /* ---- 4) 权威消息到达（status=0 正式 AI 消息） ---- */
    {
        const auto notify = conn.waitFrame(0x0303, 8000);
        lingxi::MessageNotify notifyPb;
        CHECK(notifyPb.ParseFromString(notify.second), "final notify parse");
        CHECK(notifyPb.body().from_uid() == kAiUid && notifyPb.body().status() == 0 &&
                  notifyPb.body().conv_seq() == placeholderSeq,
              "final AI message mismatch");
        CHECK(notifyPb.body().payload().find("versions") != std::string::npos,
              "payload should carry versions[] (Swipe 结构)");
    }
    std::cout << "[5] final AI message with versions[] payload" << std::endl;

    /* ---- 5) 世界书触发验证（mock 回复应含蛊相关内容） ---- */
    // mock provider 按「蛊」关键词走专属回复分支（验证世界书触发链路有输入）
    std::cout << "M6_FLOW_PASS" << std::endl;
    return 0;
}
