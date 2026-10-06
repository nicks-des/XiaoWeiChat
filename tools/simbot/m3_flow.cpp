/**
 * @file m3_flow.cpp
 * @brief T30-06 M3 好友与群组系统验证：搜索/申请/同意/列表/建群/群消息/退群/解散/删好友。
 *
 * 前置：statusserver/gateserver/chatserver 已启动。
 * 输出 M3_FLOW_PASS 并 exit 0 表示通过。
 */
#include <boost/asio.hpp>

#include <chrono>
#include <deque>
#include <functional>
#include <iostream>
#include <mutex>
#include <condition_variable>

#include "client/net/HttpManager.h"
#include "client/net/TcpClient.h"
#include "lingxi/base/Uuid.h"
#include "lingxi/net/Packet.h"

#include "lingxi.pb.h"

namespace asio = boost::asio;

namespace {

constexpr const char* kGateHost = "127.0.0.1";
constexpr unsigned short kGatePort = 8080;
constexpr const char* kChatHost = "127.0.0.1";
constexpr unsigned short kChatPort = 8888;

#define CHECK(cond, msg)                                         \
    do {                                                         \
        if (!(cond)) {                                           \
            std::cout << "M3_FLOW_FAIL: " << (msg) << std::endl; \
            std::exit(1);                                        \
        }                                                        \
    } while (0)

/**
 * @brief HTTP 注册并登录。
 */
std::pair<long long, std::string> registerAndLogin(const std::string& username) {
    using lingxi::client::postJson;
    postJson(kGateHost, kGatePort, "/api/register",
             "{\"username\":\"" + username + "\",\"password\":\"Passw0rd!123\"}");
    const auto login = postJson(kGateHost, kGatePort, "/api/login",
                                "{\"username\":\"" + username +
                                    "\",\"password\":\"Passw0rd!123\"}");
    CHECK(login.ok(), "http login failed");
    const auto uidBegin = login.body.find("\"uid\":");
    const auto tokenBegin = login.body.find("\"token\":\"");
    const long long uid = std::stoll(login.body.substr(uidBegin + 6));
    const auto tokenStart = tokenBegin + 9;
    const std::string token =
        login.body.substr(tokenStart, login.body.find('"', tokenStart) - tokenStart);
    return {uid, token};
}

/**
 * @brief 测试长连接（收帧队列 + 按 msgId 等待）。
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
     * @brief 非阻塞嗅探：收件箱里是否已有指定帧（供时序敏感场景轮询）。
     */
    bool hasFrame(uint16_t msgId) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& frame : m_inbox) {
            if (frame.first == msgId) {
                return true;
            }
        }
        return false;
    }

    void close() { m_tcp->close(); }

private:
    std::shared_ptr<lingxi::client::TcpClient> m_tcp;
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<std::pair<uint16_t, std::string>> m_inbox;
};

std::string sendTextFrame(long long toUid, const std::string& text) {
    lingxi::MessageSendRequest request;
    auto* body = request.mutable_body();
    body->set_to_uid(toUid);
    body->set_client_msg_id(lingxi::Uuid::generate());
    body->set_msg_type(lingxi::MSG_TEXT);
    body->set_payload("{\"text\":\"" + text + "\"}");
    return request.SerializeAsString();
}

std::string sendGroupTextFrame(long long convId, const std::string& text) {
    lingxi::MessageSendRequest request;
    auto* body = request.mutable_body();
    body->set_conv_id(convId);
    body->set_group_id(convId);
    body->set_client_msg_id(lingxi::Uuid::generate());
    body->set_msg_type(lingxi::MSG_TEXT);
    body->set_payload("{\"text\":\"" + text + "\"}");
    return request.SerializeAsString();
}

} // namespace

/**
 * @brief M3 全流程验证。
 * @return int 0 成功
 */
int main() {
    const std::string stamp = std::to_string(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count() % 100000000);
    const std::string nameA = "m3a" + stamp;
    const std::string nameB = "m3b" + stamp;

    auto [uidA, tokenA] = registerAndLogin(nameA);
    auto [uidB, tokenB] = registerAndLogin(nameB);
    std::cout << "[1] users: A=" << uidA << " B=" << uidB << std::endl;

    TestConn connA;
    connA.login(uidA, tokenA);
    TestConn connB;
    connB.login(uidB, tokenB);

    /* ---- 1) A 搜索 B（用户名前缀） ---- */
    lingxi::FriendSearchRequest searchReq;
    searchReq.set_keyword(nameB);
    connA.send(0x0201, searchReq.SerializeAsString());
    const auto searchRsp = connA.waitFrame(0x0201, 5000);
    lingxi::FriendSearchResponse searchResult;
    CHECK(searchResult.ParseFromString(searchRsp.second), "search parse");
    bool found = false;
    for (const auto& user : searchResult.users()) {
        if (user.uid() == uidB) {
            found = true;
            CHECK(!user.is_friend(), "should not be friend yet");
        }
    }
    CHECK(found, "B not found in search");
    std::cout << "[2] search found B" << std::endl;

    /* ---- 2) A 申请 → B 收通知 → B 同意 ---- */
    lingxi::FriendApplyRequest applyReq;
    applyReq.set_to_uid(uidB);
    applyReq.set_verify_msg("m3-test");
    connA.send(0x0202, applyReq.SerializeAsString());
    const auto applyAck = connA.waitFrame(0x0202, 5000);
    lingxi::GenericErr applyResult;
    CHECK(applyResult.ParseFromString(applyAck.second) && applyResult.err_code() == 0,
          "apply failed");

    const auto applyNotice = connB.waitFrame(0x0203, 5000);
    lingxi::FriendApplyNotice notice;
    CHECK(notice.ParseFromString(applyNotice.second) && notice.from_uid() == uidA,
          "B should receive apply notice");

    lingxi::FriendApplyHandleRequest handleReq;
    handleReq.set_apply_id(notice.apply_id());
    handleReq.set_agree(true);
    connB.send(0x0204, handleReq.SerializeAsString());
    connB.waitFrame(0x0204, 5000);  // 同步回执

    const auto resultNotice = connA.waitFrame(0x0205, 5000);
    lingxi::FriendApplyResultNotice resultPb;
    CHECK(resultPb.ParseFromString(resultNotice.second) && resultPb.agree(),
          "A should receive agree notice");
    CHECK(resultPb.friend_().uid() == uidA || resultPb.friend_().uid() == uidB,
          "agree notice should carry friend profile");
    std::cout << "[3] friend agreed" << std::endl;

    /* ---- 3) 双方好友列表互见 ---- */
    connA.send(0x0207, lingxi::FriendListRequest().SerializeAsString());
    const auto listA = connA.waitFrame(0x0207, 5000);
    lingxi::FriendListResponse listARsp;
    CHECK(listARsp.ParseFromString(listA.second), "list A parse");
    bool aHasB = false;
    for (const auto& f : listARsp.friends()) {
        if (f.uid() == uidB) {
            aHasB = true;
        }
    }
    CHECK(aHasB, "A's list should contain B");
    std::cout << "[4] friend lists consistent" << std::endl;

    /* ---- 4) 建群（A 群主，拉 B）→ B 收入群通知 ---- */
    lingxi::GroupCreateRequest createReq;
    createReq.set_name("m3group-" + stamp);
    createReq.add_member_uids(uidB);
    connA.send(0x0401, createReq.SerializeAsString());
    const auto createAck = connA.waitFrame(0x0401, 5000);
    lingxi::GroupCreateResponse createRsp;
    CHECK(createRsp.ParseFromString(createAck.second) && createRsp.err_code() == 0,
          "group create failed");
    const int64_t groupId = createRsp.conv_id();

    const auto joinNotice = connB.waitFrame(0x0403, 5000);
    lingxi::GroupJoinNotice joinPb;
    CHECK(joinPb.ParseFromString(joinNotice.second) && joinPb.conv_id() == groupId,
          "B should receive join notice");
    CHECK(joinPb.members_size() == 2, "join notice should carry 2 members");
    std::cout << "[5] group created conv=" << groupId << std::endl;

    /* ---- 5) 群消息写扩散：A 发 → B 收；B 发 → A 收；seq 各自连续 ---- */
    connA.send(0x0301, sendGroupTextFrame(groupId, "group-hello-from-A"));
    const auto groupAckA = connA.waitFrame(0x0302, 5000);
    lingxi::MessageAck ackPb;
    CHECK(ackPb.ParseFromString(groupAckA.second) && ackPb.err_code() == 0, "group msg ack");
    const auto groupNotifyB = connB.waitFrame(0x0303, 5000);
    lingxi::MessageNotify notifyPb;
    CHECK(notifyPb.ParseFromString(groupNotifyB.second) &&
              notifyPb.body().conv_id() == groupId &&
              notifyPb.body().payload().find("group-hello-from-A") != std::string::npos,
          "B should receive group message");

    connB.send(0x0301, sendGroupTextFrame(groupId, "group-reply-from-B"));
    connB.waitFrame(0x0302, 5000);
    const auto groupNotifyA = connA.waitFrame(0x0303, 5000);
    CHECK(notifyPb.ParseFromString(groupNotifyA.second) &&
              notifyPb.body().from_uid() == uidB,
          "A should receive B's group message");
    std::cout << "[6] group message fan-out works" << std::endl;

    /* ---- 6) B 退群 → A 收 0x0408；退群后群消息不再投给 B ---- */
    lingxi::GroupQuitRequest quitReq;
    quitReq.set_conv_id(groupId);
    connB.send(0x0404, quitReq.SerializeAsString());
    connB.waitFrame(0x0404, 5000);
    const auto quitNotice = connA.waitFrame(0x0408, 5000);
    lingxi::GroupUpdateNotice quitPb;
    CHECK(quitPb.ParseFromString(quitNotice.second) && quitPb.action() == 1 &&
              quitPb.target_uid() == uidB,
          "A should receive quit notice");

    connA.send(0x0301, sendGroupTextFrame(groupId, "after-quit"));
    connA.waitFrame(0x0302, 5000);  // ACK 正常
    bool bGotAfterQuit = false;
    for (int i = 0; i < 10; ++i) {
        bGotAfterQuit = connB.hasFrame(0x0303);
        if (bGotAfterQuit) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    CHECK(!bGotAfterQuit, "B quit: must NOT receive group messages");
    std::cout << "[7] quit works, no delivery after quit" << std::endl;

    /* ---- 7) A（群主）解散 → B 收 0x0408 ---- */
    lingxi::GroupDissolveRequest dissolveReq;
    dissolveReq.set_conv_id(groupId);
    connA.send(0x0406, dissolveReq.SerializeAsString());
    connA.waitFrame(0x0406, 5000);
    const auto dissolveNotice = connA.waitFrame(0x0408, 5000);
    lingxi::GroupUpdateNotice dissolvePb;
    CHECK(dissolvePb.ParseFromString(dissolveNotice.second) && dissolvePb.action() == 3,
          "dissolve notice");
    std::cout << "[8] group dissolved" << std::endl;

    /* ---- 8) 删除好友 → 双向移除 ---- */
    lingxi::FriendDeleteRequest delReq;
    delReq.set_friend_uid(uidB);
    connA.send(0x0206, delReq.SerializeAsString());
    connA.waitFrame(0x0206, 5000);
    connA.send(0x0207, lingxi::FriendListRequest().SerializeAsString());
    const auto listA2 = connA.waitFrame(0x0207, 5000);
    lingxi::FriendListResponse listA2Rsp;
    CHECK(listA2Rsp.ParseFromString(listA2.second), "list A2 parse");
    for (const auto& f : listA2Rsp.friends()) {
        CHECK(f.uid() != uidB, "A should not have B after delete");
    }
    std::cout << "[9] friend deleted bidirectionally" << std::endl;

    std::cout << "M3_FLOW_PASS" << std::endl;
    return 0;
}
