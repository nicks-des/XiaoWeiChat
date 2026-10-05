/**
 * @file m2_flow.cpp
 * @brief T20-08 M2 可靠性系统验证：不丢/不重/不乱序/断线补拉/幂等重发（docs/02 R1~R4、R6）。
 *
 * 场景：A/B 两个用户——
 *  1) A 连发 10 条 → 10 个 ACK seq 连续递增（R3/R6）
 *  2) B 在线收 10 条 → 顺序与 ACK 一致（R1 投递）
 *  3) B 断线，A 再发 5 条
 *  4) B 重连重登 → SyncRequest(lastSeq=10) → 恰好补到 5 条（R4）
 *  5) A 重发 msg#3 的 clientMsgId → ACK 返回原 seq（R2 幂等）
 *
 * 前置：statusserver/gateserver/chatserver 已启动。
 * 输出 M2_FLOW_PASS 并 exit 0 表示通过。
 */
#include <boost/asio.hpp>

#include <chrono>
#include <deque>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <condition_variable>

#include "client/net/HttpManager.h"
#include "client/net/TcpClient.h"
#include "lingxi/base/Uuid.h"
#include "lingxi/net/Packet.h"

#include "lingxi.pb.h"

namespace asio = boost::asio;

namespace {

/** 服务地址（与 config/dev.json 一致） */
constexpr const char* kGateHost = "127.0.0.1";
constexpr unsigned short kGatePort = 8080;
constexpr const char* kChatHost = "127.0.0.1";
constexpr unsigned short kChatPort = 8888;

/** 检查宏 */
#define CHECK(cond, msg)                                                         \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::cout << "M2_FLOW_FAIL: " << (msg) << std::endl;                 \
            std::exit(1);                                                        \
        }                                                                        \
    } while (0)

/**
 * @brief HTTP 注册并登录，返回 {uid, token}。
 */
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
    CHECK(uidBegin != std::string::npos && tokenBegin != std::string::npos, "bad login json");
    const long long uid = std::stoll(login.body.substr(uidBegin + 6));
    const auto tokenStart = tokenBegin + 9;
    const std::string token =
        login.body.substr(tokenStart, login.body.find('"', tokenStart) - tokenStart);
    return {uid, token};
}

/**
 * @brief 测试用长连接封装：收帧入队，按 msgId 等待。
 */
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

    /**
     * @brief 连接并以 token 完成 TCP 登录。
     */
    void login(long long uid, const std::string& token) {
        m_tcp->connectAsync(kChatHost, kChatPort);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));  // 等连接建立
        lingxi::LoginRequest request;
        request.set_uid(uid);
        request.set_token(token);
        request.set_device_id("simbot");
        m_tcp->send(0x0101, request.SerializeAsString());
        const auto ack = waitFrame(0x0102, 5000);
        lingxi::LoginResponse response;
        CHECK(response.ParseFromString(ack.second) && response.err_code() == 0,
              "tcp login failed");
    }

    /**
     * @brief 发送一帧。
     */
    void send(uint16_t msgId, const std::string& body) { m_tcp->send(msgId, body); }

    /**
     * @brief 等待指定 msgId 的下一帧。
     * @return std::pair<uint16_t, std::string> 帧
     */
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
     * @brief 关闭连接。
     */
    void close() { m_tcp->close(); }

private:
    std::shared_ptr<lingxi::client::TcpClient> m_tcp;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<std::pair<uint16_t, std::string>> m_inbox;
};

/**
 * @brief 构造文本消息发送帧。
 */
std::string buildSendFrame(long long toUid, const std::string& clientMsgId, const std::string& text) {
    lingxi::MessageSendRequest request;
    auto* body = request.mutable_body();
    body->set_to_uid(toUid);
    body->set_client_msg_id(clientMsgId);
    body->set_msg_type(lingxi::MSG_TEXT);
    body->set_send_time_ms(0);
    body->set_payload("{\"text\":\"" + text + "\"}");
    return request.SerializeAsString();
}

} // namespace

/**
 * @brief 可靠性验证主流程。
 * @return int 0 成功
 */
int main() {
    const std::string stamp = std::to_string(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count() % 100000000);

    /* ---- 双用户登录 ---- */
    auto [uidA, tokenA] = registerAndLogin("m2a" + stamp);
    auto [uidB, tokenB] = registerAndLogin("m2b" + stamp);
    std::cout << "[1] users: A=" << uidA << " B=" << uidB << std::endl;

    TestConn connA;
    connA.login(uidA, tokenA);
    TestConn connB;
    connB.login(uidB, tokenB);

    /* ---- 1) A 连发 10 条，ACK seq 必须连续递增 ---- */
    std::vector<int64_t> ackSeqs;
    for (int i = 1; i <= 10; ++i) {
        connA.send(0x0301, buildSendFrame(uidB, lingxi::Uuid::generate(),
                                          "msg-" + std::to_string(i)));
        const auto ackFrame = connA.waitFrame(0x0302, 5000);
        lingxi::MessageAck ack;
        CHECK(ack.ParseFromString(ackFrame.second) && ack.err_code() == 0,
              "ack " + std::to_string(i) + " failed");
        ackSeqs.push_back(ack.conv_seq());
    }
    for (size_t i = 1; i < ackSeqs.size(); ++i) {
        CHECK(ackSeqs[i] == ackSeqs[i - 1] + 1, "ack seq not contiguous (R3/R6)");
    }
    const int64_t convId = [&] {
        // 从任一 ACK 取 conv_id
        connA.send(0x030A, lingxi::ConversationListRequest().SerializeAsString());
        const auto list = connA.waitFrame(0x030A, 5000);
        lingxi::ConversationListResponse response;
        CHECK(response.ParseFromString(list.second) && !response.conversations().empty(),
              "conversation list empty");
        return response.conversations(0).conv_id();
    }();
    std::cout << "[2] 10 acks contiguous, conv=" << convId
              << " seq=" << ackSeqs.front() << ".." << ackSeqs.back() << std::endl;

    /* ---- 2) B 在线收 10 条：顺序与 ACK 一致（R1） ---- */
    for (int i = 0; i < 10; ++i) {
        const auto notify = connB.waitFrame(0x0303, 5000);
        lingxi::MessageNotify message;
        CHECK(message.ParseFromString(notify.second), "notify parse failed");
        CHECK(message.body().conv_seq() == ackSeqs[static_cast<size_t>(i)],
              "B seq mismatch at " + std::to_string(i));
        CHECK(message.body().payload().find("msg-" + std::to_string(i + 1)) !=
                  std::string::npos,
              "B payload order wrong at " + std::to_string(i));
    }
    std::cout << "[3] B received 10 in order" << std::endl;

    /* ---- 3) B 断线，A 再发 5 条 ---- */
    connB.close();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    std::vector<int64_t> offlineSeqs;
    for (int i = 11; i <= 15; ++i) {
        connA.send(0x0301, buildSendFrame(uidB, lingxi::Uuid::generate(),
                                          "msg-" + std::to_string(i)));
        const auto ackFrame = connA.waitFrame(0x0302, 5000);
        lingxi::MessageAck ack;
        CHECK(ack.ParseFromString(ackFrame.second) && ack.err_code() == 0, "offline ack failed");
        offlineSeqs.push_back(ack.conv_seq());
    }
    std::cout << "[4] 5 offline msgs persisted, seq=" << offlineSeqs.front() << ".."
              << offlineSeqs.back() << std::endl;

    /* ---- 4) B 重连重登 → 补拉恰好 5 条（R4） ---- */
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    auto [uidB2, tokenB2] = registerAndLogin("m2b" + stamp);  // 重登换 token（顶掉旧死连接）
    CHECK(uidB2 == uidB, "relogin uid mismatch");
    TestConn connB2;
    connB2.login(uidB, tokenB2);
    lingxi::SyncRequest sync;
    auto* cursor = sync.add_cursors();
    cursor->set_conv_id(convId);
    cursor->set_last_seq(ackSeqs.back());  // 本地游标：只差 5 条
    connB2.send(0x0308, sync.SerializeAsString());
    const auto syncFrame = connB2.waitFrame(0x0309, 5000);
    lingxi::SyncResponse syncRsp;
    CHECK(syncRsp.ParseFromString(syncFrame.second), "sync parse failed");
    CHECK(!syncRsp.has_more(), "sync has_more unexpected");
    CHECK(syncRsp.messages_size() == 5, "sync expected 5, got " +
                                            std::to_string(syncRsp.messages_size()));
    for (int i = 0; i < 5; ++i) {
        CHECK(syncRsp.messages(static_cast<int>(i)).conv_seq() == offlineSeqs[static_cast<size_t>(i)],
              "sync seq mismatch at " + std::to_string(i));
    }
    std::cout << "[5] B resync got exactly 5 missed messages" << std::endl;

    /* ---- 5) 幂等重发：msg#3 的 clientMsgId → 返回原 seq（R2） ---- */
    // 取 msg#3 的 clientMsgId：重新发一条并记录，再重发一次
    const std::string dupClientMsgId = lingxi::Uuid::generate();
    connA.send(0x0301, buildSendFrame(uidB, dupClientMsgId, "dup-probe"));
    const auto ack1 = connA.waitFrame(0x0302, 5000);
    lingxi::MessageAck ackOne;
    CHECK(ackOne.ParseFromString(ack1.second) && ackOne.err_code() == 0, "dup first failed");
    const int64_t originalSeq = ackOne.conv_seq();
    connA.send(0x0301, buildSendFrame(uidB, dupClientMsgId, "dup-probe"));  // 同 clientMsgId 重发
    const auto ack2 = connA.waitFrame(0x0302, 5000);
    lingxi::MessageAck ackTwo;
    CHECK(ackTwo.ParseFromString(ack2.second) && ackTwo.err_code() == 0, "dup resend failed");
    CHECK(ackTwo.conv_seq() == originalSeq, "resend must reuse original seq (R2)");
    std::cout << "[6] idempotent resend reused seq=" << originalSeq << std::endl;

    std::cout << "M2_FLOW_PASS" << std::endl;
    return 0;
}
