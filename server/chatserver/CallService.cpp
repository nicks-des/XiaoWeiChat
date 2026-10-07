/**
 * @file CallService.cpp
 * @brief M5 通话信令服务实现。
 */
#include "CallService.h"

#include "ChatServer.h"

#include "lingxi/base/TimeUtil.h"
#include "lingxi/config/Config.h"
#include "lingxi/logging/Logger.h"

#include "lingxi.pb.h"

namespace lingxi {

namespace {

/** 单聊会话 member_key 生成（与 MessageService 一致） */
std::string memberKeyOf(int64_t a, int64_t b) {
    return std::to_string(std::min(a, b)) + "_" + std::to_string(std::max(a, b));
}

} // namespace

CallService::CallService(ChatServer& server, db::MySqlConnectionPool* db,
                         SnowflakeIdGenerator* idGen)
    : m_server(server), m_db(db), m_idGen(idGen) {
    m_ringTimeoutSec = Config::instance().get<int>("chatserver.callTimeoutSec", 45);
}

void CallService::startSweeper(asio::io_context& ioContext) {
    // 定时器以 shared_ptr 携带、成员函数递归续期（避免捕获栈上 lambda 引用，M4 教训）
    m_sweepTimer = std::make_shared<asio::steady_timer>(ioContext);
    sweep(*m_sweepTimer);
}

void CallService::sweep(asio::steady_timer& timer) {
    timer.expires_after(std::chrono::seconds(5));
    timer.async_wait([this, &timer](const boost::system::error_code& ec) {
        if (ec) {
            return;
        }
        const int64_t now = TimeUtil::nowMs();
        std::vector<std::string> expired;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (auto& entry : m_sessions) {
                auto& session = entry.second;
                if (session.state == kRinging &&
                    now - session.createdAtMs > static_cast<int64_t>(m_ringTimeoutSec) * 1000) {
                    expired.push_back(entry.first);
                }
            }
        }
        for (const auto& callId : expired) {
            // 超时未接：双方收 CallStateNotice，记录未接
            CallStateNotice notice;
            notice.set_call_id(callId);
            notice.set_state(2);
            notice.set_err_msg("no answer");
            std::vector<Delivery> deliveries;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                auto it = m_sessions.find(callId);
                if (it != m_sessions.end()) {
                    deliveries.push_back({it->second.callerUid, 0x0609,
                                          notice.SerializeAsString()});
                    deliveries.push_back({it->second.calleeUid, 0x0609,
                                          notice.SerializeAsString()});
                    endCall(callId, kRecMissed, 0);
                }
            }
            for (const auto& d : deliveries) {
                m_server.deliverToUid(d.uid, d.msgId, d.body);
            }
        }
        sweep(timer);
    });
}

/* ==================== 信令处理 ==================== */

std::pair<std::string, std::vector<CallService::Delivery>> CallService::handleInvite(
    int64_t callerUid, const std::string& payloadBytes) {
    GenericErr response;
    std::vector<Delivery> deliveries;
    CallInviteRequest request;
    if (!request.ParseFromString(payloadBytes) || request.callee_uid() == callerUid) {
        response.set_err_code(400);
        response.set_err_msg("bad invite");
        return {response.SerializeAsString(), deliveries};
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    // 占线校验：主叫或被叫是任一存续会话（calling/ringing/active）的参与者即占线
    // （docs/05 §2 唯一性；振铃中的主叫同样占线）
    const auto isParticipant = [this](const CallSession& session, int64_t uid) {
        return session.callerUid == uid || session.calleeUid == uid;
    };
    for (const auto& entry : m_sessions) {
        if (isParticipant(entry.second, callerUid)) {
            response.set_err_code(409);
            response.set_err_msg("you are in another call");
            return {response.SerializeAsString(), deliveries};
        }
        if (isParticipant(entry.second, request.callee_uid())) {
            response.set_err_code(409);
            response.set_err_msg("callee busy");
            return {response.SerializeAsString(), deliveries};
        }
    }

    CallSession session;
    session.callId = request.call_id();
    session.callerUid = callerUid;
    session.calleeUid = request.callee_uid();
    session.mediaType = request.media_type();
    session.offerSdp = request.offer_sdp();
    session.state = kRinging;
    session.createdAtMs = TimeUtil::nowMs();

    // 通知被叫振铃（0x0602，含主叫预协商 Offer）——必须在 move 之前构造
    CallRingNotice ring;
    ring.set_call_id(session.callId);
    ring.set_caller_uid(callerUid);
    ring.set_caller_nickname("灵犀用户" + std::to_string(callerUid % 100000));
    ring.set_media_type(session.mediaType);
    ring.set_offer_sdp(session.offerSdp);
    deliveries.push_back({session.calleeUid, 0x0602, ring.SerializeAsString()});

    // insert_or_assign：同 call_id 重复邀请覆盖残留会话（客户端重试语义）
    LX_LOG_INFO("call invite: {} -> {} call={} media={}", callerUid, request.callee_uid(),
                session.callId, session.mediaType);
    m_sessions.insert_or_assign(session.callId, std::move(session));
    response.set_err_code(0);
    return {response.SerializeAsString(), deliveries};
}

std::pair<std::string, std::vector<CallService::Delivery>> CallService::handleAccept(
    int64_t calleeUid, const std::string& payloadBytes) {
    GenericErr response;
    std::vector<Delivery> deliveries;
    CallAcceptRequest request;
    if (!request.ParseFromString(payloadBytes)) {
        LX_LOG_WARN("call accept: bad parse");
        response.set_err_code(400);
        return {response.SerializeAsString(), deliveries};
    }
    LX_LOG_INFO("call accept received: call={} from={}", request.call_id(), calleeUid);

    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_sessions.find(request.call_id());
    if (it == m_sessions.end() || it->second.calleeUid != calleeUid ||
        it->second.state != kRinging) {
        LX_LOG_WARN("call accept 404: found={} calleeMatch={} state={}", it != m_sessions.end(),
                    it != m_sessions.end() ? it->second.calleeUid == calleeUid : false,
                    it != m_sessions.end() ? it->second.state : -1);
        response.set_err_code(404);
        response.set_err_msg("call not ringing");
        return {response.SerializeAsString(), deliveries};
    }
    auto& session = it->second;
    session.state = kActive;
    session.connectedAtMs = TimeUtil::nowMs();

    CallAcceptNotice notice;
    notice.set_call_id(session.callId);
    notice.set_answer_sdp(request.answer_sdp());
    deliveries.push_back({session.callerUid, 0x0603, notice.SerializeAsString()});

    LX_LOG_INFO("call accepted: call={}", session.callId);
    response.set_err_code(0);
    return {response.SerializeAsString(), deliveries};
}

std::pair<std::string, std::vector<CallService::Delivery>> CallService::handleReject(
    int64_t calleeUid, const std::string& payloadBytes) {
    GenericErr response;
    std::vector<Delivery> deliveries;
    CallRejectRequest request;
    if (!request.ParseFromString(payloadBytes)) {
        response.set_err_code(400);
        return {response.SerializeAsString(), deliveries};
    }

    int64_t callerUid = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_sessions.find(request.call_id());
        if (it == m_sessions.end() || it->second.calleeUid != calleeUid) {
            response.set_err_code(404);
            return {response.SerializeAsString(), deliveries};
        }
        callerUid = it->second.callerUid;
        endCall(request.call_id(), kRecRejected, 0);
    }
    CallRejectNotice notice;
    notice.set_call_id(request.call_id());
    notice.set_reason(request.reason());
    notice.set_err_msg("rejected");
    deliveries.push_back({callerUid, 0x0604, notice.SerializeAsString()});
    response.set_err_code(0);
    return {response.SerializeAsString(), deliveries};
}

std::pair<std::string, std::vector<CallService::Delivery>> CallService::handleCancel(
    int64_t callerUid, const std::string& payloadBytes) {
    GenericErr response;
    std::vector<Delivery> deliveries;
    CallCancelRequest request;
    if (!request.ParseFromString(payloadBytes)) {
        response.set_err_code(400);
        return {response.SerializeAsString(), deliveries};
    }

    int64_t calleeUid = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_sessions.find(request.call_id());
        if (it == m_sessions.end() || it->second.callerUid != callerUid ||
            it->second.state != kRinging) {
            response.set_err_code(404);
            return {response.SerializeAsString(), deliveries};
        }
        calleeUid = it->second.calleeUid;
        endCall(request.call_id(), kRecCancelled, 0);
    }
    CallStateNotice notice;
    notice.set_call_id(request.call_id());
    notice.set_state(3);
    notice.set_err_msg("caller cancelled");
    deliveries.push_back({calleeUid, 0x0609, notice.SerializeAsString()});
    response.set_err_code(0);
    return {response.SerializeAsString(), deliveries};
}

std::pair<std::string, std::vector<CallService::Delivery>> CallService::handleHangup(
    int64_t uid, const std::string& payloadBytes) {
    GenericErr response;
    std::vector<Delivery> deliveries;
    CallHangupRequest request;
    if (!request.ParseFromString(payloadBytes)) {
        response.set_err_code(400);
        return {response.SerializeAsString(), deliveries};
    }

    int64_t peerUid = 0;
    int32_t duration = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_sessions.find(request.call_id());
        if (it == m_sessions.end()) {
            response.set_err_code(404);
            return {response.SerializeAsString(), deliveries};
        }
        auto& session = it->second;
        if (session.callerUid != uid && session.calleeUid != uid) {
            response.set_err_code(403);
            return {response.SerializeAsString(), deliveries};
        }
        peerUid = (uid == session.callerUid) ? session.calleeUid : session.callerUid;
        const bool wasActive = session.state == kActive;
        // 时长取「服务端结算」与「挂断方上报」较大者（容忍时钟/计时误差，docs/02 §6 挂断语义）
        const int32_t serverDuration =
            wasActive ? static_cast<int32_t>((TimeUtil::nowMs() - session.connectedAtMs) / 1000) : 0;
        duration = std::max(request.duration_sec(), serverDuration);
        endCall(request.call_id(), wasActive ? kRecConnected : kRecAbnormal, duration);
    }
    CallHangupNotice notice;
    notice.set_call_id(request.call_id());
    notice.set_duration_sec(duration);
    deliveries.push_back({peerUid, 0x0606, notice.SerializeAsString()});

    LX_LOG_INFO("call hangup: call={} by={} duration={}", request.call_id(), uid, duration);
    response.set_err_code(0);
    return {response.SerializeAsString(), deliveries};
}

std::vector<CallService::Delivery> CallService::handleRelay(
    int64_t uid, uint16_t msgId, const std::string& payloadBytes) {
    std::vector<Delivery> deliveries;
    std::lock_guard<std::mutex> lock(m_mutex);
    // SDP/ICE 帧首字段均为 call_id（proto 字段 1），直接截取转发对端
    CallIceCandidate probe;
    if (!probe.ParseFromString(payloadBytes)) {
        return deliveries;
    }
    auto it = m_sessions.find(probe.call_id());
    if (it == m_sessions.end()) {
        return deliveries;
    }
    auto& session = it->second;
    if (session.callerUid != uid && session.calleeUid != uid) {
        return deliveries;  // 越权防护
    }
    const int64_t peerUid = (uid == session.callerUid) ? session.calleeUid : session.callerUid;
    deliveries.push_back({peerUid, msgId, payloadBytes});
    return deliveries;
}

/* ==================== 内部 ==================== */

CallService::CallSession* CallService::findActiveByUid(int64_t uid) {
    for (auto& entry : m_sessions) {
        if (entry.second.callerUid == uid || entry.second.calleeUid == uid) {
            return &entry.second;
        }
    }
    return nullptr;
}

void CallService::endCall(const std::string& callId, int recordState, int32_t durationSec) {
    auto it = m_sessions.find(callId);
    if (it == m_sessions.end()) {
        return;
    }
    writeRecord(it->second, recordState, durationSec);
    m_sessions.erase(it);
}

void CallService::writeRecord(const CallSession& session, int recordState,
                              int32_t durationSec) {
    auto conn = m_db->acquire();
    if (conn == nullptr) {
        LX_LOG_WARN("call record skipped (db busy): call={}", session.callId);
        return;
    }
    conn->execute("INSERT INTO t_call_record (id, call_id, caller_uid, callee_uid, media_type, "
                  "state, duration_sec) VALUES (" +
                  std::to_string(m_idGen->nextId()) + ", '" + session.callId + "', " +
                  std::to_string(session.callerUid) + ", " +
                  std::to_string(session.calleeUid) + ", " +
                  std::to_string(session.mediaType) + ", " + std::to_string(recordState) +
                  ", " + std::to_string(durationSec) + ")");
}

} // namespace lingxi
