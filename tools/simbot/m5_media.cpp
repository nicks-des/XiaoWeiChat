/**
 * @file m5_media.cpp
 * @brief T50-03 M5 媒体面系统验证：双端 WebRtcPeer 经信令交换 SDP/ICE，验证 ICE/DTLS/SRTP
 *        真实链路建立（PeerConnection Connected）。
 *
 * 前置：gateserver/chatserver 已启动。音频编解码关闭（withAudio=false，无采集设备依赖），
 * 连接态即验证 ICE/DTLS/SRTP 全链路。
 * 输出 M5_MEDIA_PASS 并 exit 0 表示通过。
 */
#include <boost/asio.hpp>

#include <chrono>
#include <deque>
#include <functional>
#include <iostream>
#include <mutex>
#include <condition_variable>

#include "client/media/WebRtcPeer.h"
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

#define CHECK(cond, msg)                                               \
    do {                                                               \
        if (!(cond)) {                                                 \
            std::cout << "M5_MEDIA_FAIL: " << (msg) << std::endl;      \
            std::exit(1);                                              \
        }                                                              \
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
 * @brief 测试长连接：收帧队列 + 按 msgId 等待。
 */
class TestConn {
public:
    TestConn() {
        m_tcp = std::make_shared<lingxi::client::TcpClient>();
        m_tcp->setCallbacks(
            [this](uint16_t msgId, const std::string& body) {
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_inbox.push_back({msgId, body});
                }
                m_cv.notify_all();
                // ICE 泵：0x0608 候选到达即路由给 WebRtcPeer
                if (m_iceHandler != nullptr && msgId == 0x0608) {
                    try {
                        lingxi::CallIceCandidate ice;
                        if (ice.ParseFromString(body)) {
                            m_iceHandler(ice.candidate(), ice.mid());
                        }
                    } catch (const std::exception& e) {
                        std::cout << "[pump exception] " << e.what() << std::endl;
                    }
                }
            },
            [](bool, const std::string&) {});
    }

    /**
     * @brief 注册 ICE 候选路由（收到 0x0608 即回调）。
     */
    void setIceHandler(std::function<void(const std::string&, const std::string&)> handler) {
        m_iceHandler = std::move(handler);
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

    void close() { m_tcp->close(); }

private:
    std::shared_ptr<lingxi::client::TcpClient> m_tcp;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<std::pair<uint16_t, std::string>> m_inbox;
    std::function<void(const std::string&, const std::string&)> m_iceHandler;  ///< ICE 泵目标
};

} // namespace

/**
 * @brief 媒体连接验证主流程。
 * @return int 0 成功
 */
int main() {
    const std::string stamp = std::to_string(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count() % 100000000);

    const std::string mediaCallId = lingxi::Uuid::generate();
    auto [uidA, tokenA] = registerAndLogin("m5ma" + stamp);
    auto [uidB, tokenB] = registerAndLogin("m5mb" + stamp);
    std::cout << "[1] users: A=" << uidA << " B=" << uidB << std::endl;

    TestConn connA;
    connA.login(uidA, tokenA);
    TestConn connB;
    connB.login(uidB, tokenB);

    /* ---- 主叫：建 Peer + Offer；SDP/ICE 帧直接发到信令通道 ---- */
    auto peerA = std::make_shared<lingxi::client::WebRtcPeer>();
    connA.setIceHandler([&peerA](const std::string& candidate, const std::string& mid) {
        peerA->setRemoteIce(candidate, mid);
    });
    std::string offerSdp;
    std::mutex mutexA;
    peerA->setCallbacks(
        [&](const std::string& sdp, const std::string& type) {
            if (type == "offer") {
                std::lock_guard<std::mutex> lock(mutexA);
                offerSdp = sdp;
            }
        },
        [&](const std::string& candidate, const std::string& mid) {
            lingxi::CallIceCandidate ice;
            ice.set_call_id(mediaCallId);
            ice.set_candidate(candidate);
            ice.set_mid(mid);
            connA.send(0x0608, ice.SerializeAsString());
        },
        [](lingxi::client::WebRtcPeer::PcState) {}, nullptr);
    CHECK(peerA->initAsCaller(false), "peerA init failed");  // 无音频（无采集设备）

    /* ---- 等待 Offer 就绪并邀请 ---- */
    for (int i = 0; i < 100 && offerSdp.empty(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    CHECK(!offerSdp.empty(), "offer not ready");
    lingxi::CallInviteRequest invite;
    invite.set_call_id(mediaCallId);
    invite.set_callee_uid(uidB);
    invite.set_media_type(1);
    invite.set_offer_sdp(offerSdp);
    connA.send(0x0601, invite.SerializeAsString());
    connA.waitFrame(0x0601, 5000);
    const auto ring = connB.waitFrame(0x0602, 5000);
    lingxi::CallRingNotice ringPb;
    CHECK(ringPb.ParseFromString(ring.second), "ring parse");
    std::cout << "[2] offer relayed via signaling" << std::endl;

    /* ---- 被叫：Answer + ICE 交换（双方自动经 0x0608 发送候选） ---- */
    auto peerB = std::make_shared<lingxi::client::WebRtcPeer>();
    connB.setIceHandler([&peerB](const std::string& candidate, const std::string& mid) {
        peerB->setRemoteIce(candidate, mid);
    });
    std::mutex mutexB;
    peerB->setCallbacks(
        [&](const std::string& sdp, const std::string& type) {
            if (type == "answer") {
                lingxi::CallAcceptRequest request;
                request.set_call_id(mediaCallId);
                request.set_answer_sdp(sdp);
                connB.send(0x0603, request.SerializeAsString());
            }
        },
        [&](const std::string& candidate, const std::string& mid) {
            lingxi::CallIceCandidate ice;
            ice.set_call_id(mediaCallId);
            ice.set_candidate(candidate);
            ice.set_mid(mid);
            connB.send(0x0608, ice.SerializeAsString());
        },
        [](lingxi::client::WebRtcPeer::PcState) {}, nullptr);
    CHECK(peerB->initAsCallee(ringPb.offer_sdp(), false), "peerB init failed");

    // B 收 Answer
    const auto acceptNotice = connA.waitFrame(0x0603, 5000);
    lingxi::CallAcceptNotice acceptPb;
    CHECK(acceptPb.ParseFromString(acceptNotice.second), "accept parse");
    peerA->setRemoteAnswer(acceptPb.answer_sdp());
    std::cout << "[3] answer exchanged, ICE negotiation running..." << std::endl;

    /* ---- 等待双方 Connected（ICE/DTLS/SRTP 建链完成） ---- */
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    for (;;) {
        const bool connected = peerA->isConnected() && peerB->isConnected();
        if (connected) {
            break;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            CHECK(false, "peers not connected within 15s (A=" +
                             std::to_string(static_cast<int>(peerA->state())) +
                             " B=" + std::to_string(static_cast<int>(peerB->state())) + ")");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::cout << "[4] both peers Connected (ICE/DTLS/SRTP established)" << std::endl;

    /* ---- 收尾：A 挂断（走完整信令） ---- */
    lingxi::CallHangupRequest hangup;
    hangup.set_call_id(mediaCallId);
    hangup.set_duration_sec(0);
    connA.send(0x0606, hangup.SerializeAsString());
    connA.waitFrame(0x0606, 5000);
    connB.waitFrame(0x0606, 5000);
    peerA->close();
    peerB->close();
    connA.close();
    connB.close();

    std::cout << "M5_MEDIA_PASS" << std::endl;
    return 0;
}
