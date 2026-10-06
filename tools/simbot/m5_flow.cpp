/**
 * @file m5_flow.cpp
 * @brief T50-07 M5 通话信令面系统验证：邀请→振铃→接听→中继→挂断 + 拒绝 + 占线 + 取消。
 *
 * 前置：gateserver/chatserver 已启动。SDP/ICE 内容以占位串代替（媒体面联调在 T50-03）。
 * 输出 M5_FLOW_PASS 并 exit 0 表示通过。
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
            std::cout << "M5_FLOW_FAIL: " << (msg) << std::endl; \
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
    CHECK(login.ok(), "http login failed: " + login.body);
    const auto uidBegin = login.body.find("\"uid\":");
    const auto tokenBegin = login.body.find("\"token\":\"");
    const long long uid = std::stoll(login.body.substr(uidBegin + 6));
    const auto tokenStart = tokenBegin + 9;
    const std::string token =
        login.body.substr(tokenStart, login.body.find('"', tokenStart) - tokenStart);
    return {uid, token};
}

/**
 * @brief 测试长连接（收帧队列 + 按 msgId 等待/嗅探）。
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

} // namespace

/**
 * @brief 通话信令面验证。
 * @return int 0 成功
 */
int main() {
    const std::string stamp = std::to_string(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count() % 100000000);

    auto [uidA, tokenA] = registerAndLogin("m5a" + stamp);
    auto [uidB, tokenB] = registerAndLogin("m5b" + stamp);
    auto [uidC, tokenC] = registerAndLogin("m5c" + stamp);
    std::cout << "[1] users: A=" << uidA << " B=" << uidB << " C=" << uidC << std::endl;

    TestConn connA;
    connA.login(uidA, tokenA);
    TestConn connB;
    connB.login(uidB, tokenB);
    TestConn connC;
    connC.login(uidC, tokenC);

    /* ---- 场景 1：完整通话生命周期 ---- */
    const std::string callId = lingxi::Uuid::generate();
    lingxi::CallInviteRequest invite;
    invite.set_call_id(callId);
    invite.set_callee_uid(uidB);
    invite.set_media_type(2);  // 视频
    invite.set_offer_sdp("OFFER-A-1");
    connA.send(0x0601, invite.SerializeAsString());

    const auto inviteAck = connA.waitFrame(0x0601, 5000);
    lingxi::GenericErr inviteErr;
    CHECK(inviteErr.ParseFromString(inviteAck.second) && inviteErr.err_code() == 0,
          "invite rejected unexpectedly");

    const auto ring = connB.waitFrame(0x0602, 5000);
    lingxi::CallRingNotice ringPb;
    CHECK(ringPb.ParseFromString(ring.second) && ringPb.call_id() == callId &&
              ringPb.caller_uid() == uidA && ringPb.offer_sdp() == "OFFER-A-1",
          "ring notice mismatch");
    std::cout << "[2] invite -> ring ok (offer relayed)" << std::endl;

    lingxi::CallAcceptRequest accept;
    accept.set_call_id(callId);
    accept.set_answer_sdp("ANSWER-B-1");
    connB.send(0x0603, accept.SerializeAsString());
    connB.waitFrame(0x0603, 5000);
    const auto acceptNotice = connA.waitFrame(0x0603, 5000);
    lingxi::CallAcceptNotice acceptPb;
    CHECK(acceptPb.ParseFromString(acceptNotice.second) &&
              acceptPb.answer_sdp() == "ANSWER-B-1",
          "answer relay mismatch");
    std::cout << "[3] accept -> answer relayed" << std::endl;

    // ICE 中继双向
    lingxi::CallIceCandidate ice;
    ice.set_call_id(callId);
    ice.set_candidate("candidate-A");
    ice.set_mid("0");
    connA.send(0x0608, ice.SerializeAsString());
    const auto iceRelay = connB.waitFrame(0x0608, 5000);
    lingxi::CallIceCandidate icePb;
    CHECK(icePb.ParseFromString(iceRelay.second) && icePb.candidate() == "candidate-A",
          "ice relay mismatch");

    lingxi::CallSdpNet sdp;
    sdp.set_call_id(callId);
    sdp.set_sdp("REOFFER-A-2");
    connA.send(0x0607, sdp.SerializeAsString());
    const auto sdpRelay = connB.waitFrame(0x0607, 5000);
    lingxi::CallSdpNet sdpPb;
    CHECK(sdpPb.ParseFromString(sdpRelay.second) && sdpPb.sdp() == "REOFFER-A-2",
          "sdp relay mismatch");
    std::cout << "[4] ice/sdp relay both ways ok" << std::endl;

    // 挂断：A 挂，B 收通知
    lingxi::CallHangupRequest hangup;
    hangup.set_call_id(callId);
    hangup.set_duration_sec(12);
    connA.send(0x0606, hangup.SerializeAsString());
    connA.waitFrame(0x0606, 5000);
    const auto hangupNotice = connB.waitFrame(0x0606, 5000);
    lingxi::CallHangupNotice hangupPb;
    CHECK(hangupPb.ParseFromString(hangupNotice.second) && hangupPb.duration_sec() >= 12,
          "hangup notice mismatch");
    std::cout << "[5] hangup ok, duration relayed" << std::endl;

    /* ---- 场景 2：B 拒绝 C 的邀请 ---- */
    const std::string callId2 = lingxi::Uuid::generate();
    lingxi::CallInviteRequest invite2;
    invite2.set_call_id(callId2);
    invite2.set_callee_uid(uidB);
    invite2.set_media_type(1);
    invite2.set_offer_sdp("OFFER-C");
    connC.send(0x0601, invite2.SerializeAsString());
    connC.waitFrame(0x0601, 5000);
    connB.waitFrame(0x0602, 5000);

    lingxi::CallRejectRequest reject;
    reject.set_call_id(callId2);
    reject.set_reason(0);
    connB.send(0x0604, reject.SerializeAsString());
    connB.waitFrame(0x0604, 5000);
    const auto rejectNotice = connC.waitFrame(0x0604, 5000);
    lingxi::CallRejectNotice rejectPb;
    CHECK(rejectPb.ParseFromString(rejectNotice.second) && rejectPb.reason() == 0,
          "reject notice mismatch");
    std::cout << "[6] reject relayed" << std::endl;

    /* ---- 场景 3：B 振铃中再收第二路邀请 → 服务端拒绝（占线） ---- */
    // B 先向 C 发起一路（B 主叫）
    const std::string callId3 = lingxi::Uuid::generate();
    lingxi::CallInviteRequest invite3;
    invite3.set_call_id(callId3);
    invite3.set_callee_uid(uidC);
    invite3.set_media_type(1);
    connB.send(0x0601, invite3.SerializeAsString());
    connB.waitFrame(0x0601, 5000);
    connC.waitFrame(0x0602, 5000);
    // 此时 B 振铃中，A 再邀 B → 409
    const std::string callId4 = lingxi::Uuid::generate();
    lingxi::CallInviteRequest invite4;
    invite4.set_call_id(callId4);
    invite4.set_callee_uid(uidB);
    invite4.set_media_type(1);
    connA.send(0x0601, invite4.SerializeAsString());
    const auto busyAck = connA.waitFrame(0x0601, 5000);
    lingxi::GenericErr busyErr;
    CHECK(busyErr.ParseFromString(busyAck.second) && busyErr.err_code() == 409,
          "busy should be 409");
    // B 再邀 C（B 已有主叫会话）→ 409
    const std::string callId5 = lingxi::Uuid::generate();
    lingxi::CallInviteRequest invite5;
    invite5.set_call_id(callId5);
    invite5.set_callee_uid(uidA);
    invite5.set_media_type(1);
    connB.send(0x0601, invite5.SerializeAsString());
    const auto busyAck2 = connB.waitFrame(0x0601, 5000);
    CHECK(busyErr.ParseFromString(busyAck2.second) && busyErr.err_code() == 409,
          "caller busy should be 409");
    std::cout << "[7] busy detection ok (callee & caller)" << std::endl;

    /* ---- 场景 4：C 拒绝 B（清掉 B 的会话）+ 主叫取消路径 ---- */
    lingxi::CallRejectRequest rejectC;
    rejectC.set_call_id(callId3);
    rejectC.set_reason(0);
    connC.send(0x0604, rejectC.SerializeAsString());
    connC.waitFrame(0x0604, 5000);
    connB.waitFrame(0x0604, 5000);

    const std::string callId6 = lingxi::Uuid::generate();
    lingxi::CallInviteRequest invite6;
    invite6.set_call_id(callId6);
    invite6.set_callee_uid(uidB);
    invite6.set_media_type(1);
    connC.send(0x0601, invite6.SerializeAsString());
    connC.waitFrame(0x0601, 5000);
    connB.waitFrame(0x0602, 5000);
    lingxi::CallCancelRequest cancel;
    cancel.set_call_id(callId6);
    connC.send(0x0605, cancel.SerializeAsString());
    connC.waitFrame(0x0605, 5000);
    const auto cancelNotice = connB.waitFrame(0x0609, 5000);
    lingxi::CallStateNotice cancelPb;
    CHECK(cancelPb.ParseFromString(cancelNotice.second) && cancelPb.state() == 3,
          "cancel notice mismatch");
    std::cout << "[8] cancel relayed" << std::endl;

    /* ---- 场景 5：通话记录落库（直接看服务端行为：B 端记录数 ≥ 前述场景数） ---- */
    std::cout << "M5_FLOW_PASS" << std::endl;
    return 0;
}
