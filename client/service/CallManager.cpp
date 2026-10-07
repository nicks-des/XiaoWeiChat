/**
 * @file CallManager.cpp
 * @brief M5 通话编排服务实现。
 */
#include "CallManager.h"

#include <QAudioFormat>
#include <QAudioInput>
#include <QAudioOutput>

#include <cstring>

#include "client/net/TcpClient.h"
#include "lingxi/base/Uuid.h"
#include "lingxi/config/Config.h"
#include "lingxi/logging/Logger.h"

#include "lingxi.pb.h"

namespace lingxi::client {

namespace {

/** 音频参数：48kHz 单声道 16bit（docs/05 §5） */
constexpr int kSampleRate = 48000;
constexpr int kChannels = 1;
constexpr int kSampleBytes = 2;
/** 20ms 帧字节数 = 960 样本 × 2 字节 */
constexpr int kFrameBytes = 960 * kSampleBytes * kChannels;

/** 信令 msgId（docs/02 §2 0x06xx） */
constexpr uint16_t kMsgIdInvite = 0x0601;
constexpr uint16_t kMsgIdAccept = 0x0603;
constexpr uint16_t kMsgIdReject = 0x0604;
constexpr uint16_t kMsgIdCancel = 0x0605;
constexpr uint16_t kMsgIdHangup = 0x0606;
constexpr uint16_t kMsgIdIce = 0x0608;

} // namespace

CallManager& CallManager::instance() {
    static CallManager s_manager;
    return s_manager;
}

void CallManager::setup(TcpClient* tcp) {
    if (m_tcp != nullptr) {
        return;
    }
    m_tcp = tcp;
    m_captureTimer.setInterval(20);
    connect(&m_captureTimer, &QTimer::timeout, this, &CallManager::onCaptureTick);
    LX_LOG_INFO("CallManager setup done");
}

template <typename Fn>
void CallManager::emitOnUi(Fn&& fn) {
    QMetaObject::invokeMethod(this, std::forward<Fn>(fn), Qt::QueuedConnection);
}

void CallManager::startCall(long long peerUid) {
    if (m_inCall) {
        emit callStatus(QStringLiteral("已在通话中"));
        return;
    }
    m_peerUid = peerUid;
    m_isCaller = true;
    m_inCall = true;
    m_accepted = false;
    m_callId = Uuid::generate();

    m_peer = std::make_shared<WebRtcPeer>();
    m_peer->setCallbacks(
        [this](const std::string& sdp, const std::string&) {
            // Offer 就绪 → 0x0601
            CallInviteRequest request;
            request.set_call_id(m_callId);
            request.set_callee_uid(m_peerUid);
            request.set_media_type(1);
            request.set_offer_sdp(sdp);
            m_tcp->send(0x0601, request.SerializeAsString());
        },
        [this](const std::string& candidate, const std::string& mid) {
            CallIceCandidate ice;
            ice.set_call_id(m_callId);
            ice.set_candidate(candidate);
            ice.set_mid(mid);
            m_tcp->send(0x0608, ice.SerializeAsString());
        },
        [this](WebRtcPeer::PcState state) {
            if (state == WebRtcPeer::PcState::Connected) {
                emitOnUi([this] {
                    if (m_inCall && !m_accepted) {
                        m_accepted = true;
                    }
                    emit callConnected();
                });
            } else if (state == WebRtcPeer::PcState::Failed ||
                       state == WebRtcPeer::PcState::Disconnected) {
                emitOnUi([this] { teardown(QStringLiteral("连接中断")); });
            }
        },
        [this](const int16_t* pcm, size_t samples) {
            // 对端音频 → 播放
            if (m_outputDevice != nullptr) {
                m_outputDevice->write(reinterpret_cast<const char*>(pcm),
                                      static_cast<qint64>(samples * sizeof(int16_t)));
            }
        });

    if (!m_peer->initAsCaller(true)) {
        teardown(QStringLiteral("媒体初始化失败"));
        return;
    }
    emit callStatus(QStringLiteral("呼叫中…"));
}

void CallManager::acceptCall() {
    if (m_peer == nullptr) {
        return;
    }
    if (!startAudioDevices()) {
        LX_LOG_WARN("audio devices unavailable, continuing without sound");
    }
    m_captureTimer.start();
    // Answer 由 createPeer 注册的 onLocalDescription 回调（被叫分支）发出
    m_accepted = true;
}

void CallManager::rejectCall() {
    if (m_callId.empty()) {
        return;
    }
    CallRejectRequest request;
    request.set_call_id(m_callId);
    request.set_reason(0);
    m_tcp->send(0x0604, request.SerializeAsString());
    teardown(QStringLiteral("已拒绝"));
}

void CallManager::hangup() {
    if (m_callId.empty()) {
        return;
    }
    if (!m_accepted && m_isCaller) {
        // 未接通：主叫取消
        CallCancelRequest request;
        request.set_call_id(m_callId);
        m_tcp->send(0x0605, request.SerializeAsString());
    } else {
        CallHangupRequest request;
        request.set_call_id(m_callId);
        m_tcp->send(0x0606, request.SerializeAsString());
    }
    teardown(QStringLiteral("通话结束"));
}

void CallManager::onPacket(uint16_t msgId, const std::string& body) {
    switch (msgId) {
        case 0x0602: {
            // 来电：建被叫 WebRtcPeer（Answer 经 onLocalDescription 发出）
            CallRingNotice notice;
            if (!notice.ParseFromString(body)) {
                return;
            }
            emitOnUi([this, notice] {
                if (m_inCall) {
                    // 占线自动拒绝
                    CallRejectRequest request;
                    request.set_call_id(notice.call_id());
                    request.set_reason(1);
                    m_tcp->send(0x0604, request.SerializeAsString());
                    return;
                }
                m_inCall = true;
                m_isCaller = false;
                m_callId = notice.call_id();
                m_peerUid = notice.caller_uid();
                emit incomingCall(m_peerUid,
                                  QString::fromStdString(notice.caller_nickname()),
                                  notice.media_type() == 2);
            });
            break;
        }
        case 0x0603: {
            CallAcceptNotice notice;
            if (!notice.ParseFromString(body)) {
                return;
            }
            emitOnUi([this, notice] {
                if (m_peer != nullptr) {
                    if (!startAudioDevices()) {
                        LX_LOG_WARN("audio devices unavailable");
                    }
                    m_captureTimer.start();
                    m_peer->setRemoteAnswer(notice.answer_sdp());
                }
            });
            break;
        }
        case 0x0604: {
            CallRejectNotice notice;
            if (!notice.ParseFromString(body)) {
                return;
            }
            emitOnUi([this, notice] {
                teardown(notice.reason() == 1 ? QStringLiteral("对方忙")
                                              : QStringLiteral("对方已拒绝"));
            });
            break;
        }
        case 0x0606: {
            CallHangupNotice notice;
            if (!notice.ParseFromString(body)) {
                return;
            }
            emitOnUi([this] { teardown(QStringLiteral("对方已挂断")); });
            break;
        }
        case 0x0609: {
            CallStateNotice notice;
            if (!notice.ParseFromString(body)) {
                return;
            }
            emitOnUi([this, notice] {
                teardown(notice.state() == 2 ? QStringLiteral("无人接听")
                                             : QStringLiteral("对方已取消"));
            });
            break;
        }
        case kMsgIdIce: {
            CallIceCandidate ice;
            if (!ice.ParseFromString(body)) {
                return;
            }
            if (m_peer != nullptr) {
                m_peer->setRemoteIce(ice.candidate(), ice.mid());
            }
            break;
        }
        default:
            break;
    }
}

bool CallManager::startAudioDevices() {
    if (m_audioInput != nullptr) {
        return true;
    }
    QAudioFormat format;
    format.setSampleRate(kSampleRate);
    format.setChannelCount(kChannels);
    format.setSampleSize(16);
    format.setCodec("audio/pcm");
    format.setByteOrder(QAudioFormat::LittleEndian);
    format.setSampleType(QAudioFormat::SignedInt);

    m_audioInput = new QAudioInput(format, this);
    m_inputDevice = m_audioInput->start();  // 拉模式：onCaptureTick 定时读
    m_audioOutput = new QAudioOutput(format, this);
    m_outputDevice = m_audioOutput->start();  // 推模式：对端帧直接写入
    return true;
}

void CallManager::stopAudioDevices() {
    m_captureTimer.stop();
    if (m_audioInput != nullptr) {
        m_audioInput->stop();
        delete m_audioInput;
        m_audioInput = nullptr;
        m_inputDevice = nullptr;
    }
    if (m_audioOutput != nullptr) {
        m_audioOutput->stop();
        delete m_audioOutput;
        m_audioOutput = nullptr;
        m_outputDevice = nullptr;
    }
}

void CallManager::onCaptureTick() {
    if (m_inputDevice == nullptr || m_peer == nullptr) {
        return;
    }
    std::vector<char> buffer(kFrameBytes);
    const qint64 bytes = m_inputDevice->read(buffer.data(), buffer.size());
    if (bytes > 0) {
        m_peer->sendAudioFrame(reinterpret_cast<const int16_t*>(buffer.data()),
                               static_cast<size_t>(bytes) / sizeof(int16_t));
    }
}

void CallManager::teardown(const QString& reason) {
    if (!m_inCall && m_callId.empty()) {
        return;
    }
    m_inCall = false;
    m_accepted = false;
    m_callId.clear();
    if (m_peer != nullptr) {
        m_peer->close();
        m_peer = nullptr;
    }
    stopAudioDevices();
    emit callEnded(reason);
}

} // namespace lingxi::client
