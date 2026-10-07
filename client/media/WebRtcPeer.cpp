/**
 * @file WebRtcPeer.cpp
 * @brief M5 媒体层实现：libdatachannel + libopus。
 */
#include "WebRtcPeer.h"

#include <opus/opus.h>

#include <rtc/rtcpnackresponder.hpp>
#include <rtc/rtcpsrreporter.hpp>
#include <rtc/rtppacketizationconfig.hpp>
#include <rtc/rtppacketizer.hpp>

#include <cstring>
#include <mutex>
#include <utility>
#include <vector>
#include <random>

#include "lingxi/logging/Logger.h"

namespace rtc {

/** 前置声明链所需类型均在 rtc 命名空间（libdatachannel 头文件） */

} // namespace rtc

namespace lingxi::client {

namespace {

/** Opus 参数：48kHz 单声道，20ms 帧 = 960 样本（docs/05 §5） */
constexpr int kSampleRate = 48000;
constexpr int kChannels = 1;
constexpr int kFrameSamples = 960;
constexpr int kMaxOpusBytes = 400;

/** 动态 payload type（SDP 中 addOpusCodec 同步使用） */
constexpr int kOpusPayloadType = 111;

} // namespace

WebRtcPeer::WebRtcPeer() = default;

WebRtcPeer::~WebRtcPeer() {
    close();
}

void WebRtcPeer::setCallbacks(SdpCallback onSdp, IceCallback onIce, StateCallback onState,
                              AudioCallback onRemoteAudio) {
    m_onSdp = std::move(onSdp);
    m_onIce = std::move(onIce);
    m_onState = std::move(onState);
    m_onRemoteAudio = std::move(onRemoteAudio);
}

bool WebRtcPeer::initAudioCodecs() {
    int error = 0;
    m_opusEncoder = opus_encoder_create(kSampleRate, kChannels, OPUS_APPLICATION_VOIP, &error);
    if (error != OPUS_OK || m_opusEncoder == nullptr) {
        LX_LOG_ERROR("opus_encoder_create failed: {}", error);
        return false;
    }
    m_opusDecoder = opus_decoder_create(kSampleRate, kChannels, &error);
    if (error != OPUS_OK || m_opusDecoder == nullptr) {
        LX_LOG_ERROR("opus_decoder_create failed: {}", error);
        return false;
    }
    return true;
}

void WebRtcPeer::setupMediaChain(const std::shared_ptr<rtc::Track>& track) {
    // RTP 打包链：RtpPacketizer（建 RTP 头）→ SR 报告 → NACK 重传缓存（docs/05 §6 弱网基线）
    const uint32_t ssrc = std::random_device{}();
    auto config = std::make_shared<rtc::RtpPacketizationConfig>(
        ssrc, "lingxi", kOpusPayloadType, static_cast<uint32_t>(kSampleRate));
    // AudioRtpPacketizer：单帧单包（Opus 帧远小于 MTU），OpusRtpPacketizer = 48kHz 别名
    auto packetizer = std::make_shared<rtc::OpusRtpPacketizer>(config);  // 48kHz 单声道别名
    auto srReporter = std::make_shared<rtc::RtcpSrReporter>(config);
    packetizer->addToChain(srReporter);
    packetizer->addToChain(std::make_shared<rtc::RtcpNackResponder>());
    track->setMediaHandler(packetizer);
}

bool WebRtcPeer::initAsCaller(bool withAudio) {
    m_withAudio = withAudio;
    rtc::Configuration config;
    m_pc = std::make_shared<rtc::PeerConnection>(config);

    m_pc->onStateChange([this](auto state) {
        m_state = static_cast<int>(state);
        LX_LOG_INFO("WebRtcPeer state: {}", static_cast<int>(state));
        if (m_onState) {
            m_onState(static_cast<PcState>(state));
        }
    });
    m_pc->onLocalDescription([this](rtc::Description description) {
        if (m_onSdp) {
            m_onSdp(std::string(description), description.typeString());
        }
    });
    m_pc->onLocalCandidate([this](rtc::Candidate candidate) {
        if (m_onIce) {
            m_onIce(candidate.candidate(), candidate.mid());
        }
    });

    try {
        // 音轨始终创建（无媒体的 PC 无法生成 SDP）；withAudio 仅控制 Opus 编解码器
        rtc::Description::Audio media("audio", rtc::Description::Direction::SendRecv);
        media.addOpusCodec(kOpusPayloadType);
        m_audioTrack = m_pc->addTrack(media);
        setupMediaChain(m_audioTrack);
        if (withAudio && !initAudioCodecs()) {
            return false;
        }
        m_pc->setLocalDescription();  // 触发 onLocalDescription(Offer)
    } catch (const std::exception& e) {
        LX_LOG_ERROR("initAsCaller exception: {}", e.what());
        return false;
    }
    return true;
}

bool WebRtcPeer::initAsCallee(const std::string& remoteOfferSdp, bool withAudio) {
    m_withAudio = withAudio;
    rtc::Configuration config;
    m_pc = std::make_shared<rtc::PeerConnection>(config);

    m_pc->onStateChange([this](auto state) {
        m_state = static_cast<int>(state);
        LX_LOG_INFO("WebRtcPeer state: {}", static_cast<int>(state));
        if (m_onState) {
            m_onState(static_cast<PcState>(state));
        }
    });
    m_pc->onLocalDescription([this](rtc::Description description) {
        LX_LOG_INFO("WebRtcPeer local description: type={}", description.typeString());
        try {
            if (m_onSdp) {
                m_onSdp(std::string(description), description.typeString());
            }
        } catch (const std::exception& e) {
            LX_LOG_ERROR("onLocalDescription callback exception: {}", e.what());
        }
    });
    m_pc->onLocalCandidate([this](rtc::Candidate candidate) {
        LX_LOG_INFO("WebRtcPeer local candidate: {}", candidate.candidate().substr(0, 40));
        try {
            if (m_onIce) {
                m_onIce(candidate.candidate(), candidate.mid());
            }
        } catch (const std::exception& e) {
            LX_LOG_ERROR("onLocalCandidate callback exception: {}", e.what());
        }
    });

    m_pc->onTrack([this](std::shared_ptr<rtc::Track> track) {
        LX_LOG_INFO("WebRtcPeer onTrack: mid={}", track->mid());
        m_audioTrack = track;
        if (m_withAudio) {
            if (!initAudioCodecs()) {
                return;
            }
            setupMediaChain(track);
            // 对端 RTP → 剥头 → Opus 解码 → 上层
            track->onMessage(
                [this](rtc::message_variant message) {
                    if (!std::holds_alternative<rtc::binary>(message) || m_opusDecoder == nullptr) {
                        return;
                    }
                    const auto& packet = std::get<rtc::binary>(message);
                    if (packet.size() <= 12) {
                        return;
                    }
                    // 剥 RTP 头（固定 12B + CSRC 扩展，无扩展头场景）
                    const uint8_t cc = std::to_integer<uint8_t>(packet[0]) & 0x0F;
                    const size_t headerLen = 12 + 4 * cc;
                    if (packet.size() <= headerLen) {
                        return;
                    }
                    std::vector<int16_t> pcm(kFrameSamples * 4);
                    const int samples = opus_decode(
                        static_cast<OpusDecoder*>(m_opusDecoder),
                        reinterpret_cast<const unsigned char*>(packet.data() + headerLen),
                        static_cast<opus_int32>(packet.size() - headerLen), pcm.data(),
                        static_cast<int>(pcm.size()), 0);
                    if (samples > 0 && m_onRemoteAudio) {
                        m_onRemoteAudio(pcm.data(), static_cast<size_t>(samples));
                    }
                },
                nullptr);
        }
    });

    try {
        if (withAudio && !initAudioCodecs()) {
            return false;
        }
        rtc::Description offer(remoteOfferSdp, "offer");
        m_pc->setRemoteDescription(offer);
        markRemoteDescriptionSet();
        m_pc->setLocalDescription();  // 触发 onLocalDescription(Answer)
    } catch (const std::exception& e) {
        LX_LOG_ERROR("initAsCallee exception: {}", e.what());
        return false;
    }
    return true;
}

bool WebRtcPeer::setRemoteAnswer(const std::string& answerSdp) {
    if (m_pc == nullptr) {
        return false;
    }
    rtc::Description answer(answerSdp, "answer");
    m_pc->setRemoteDescription(answer);
    markRemoteDescriptionSet();
    return true;
}

void WebRtcPeer::setRemoteIce(const std::string& candidate, const std::string& mid) {
    std::lock_guard<std::mutex> lock(m_iceMutex);
    if (m_pc == nullptr || !m_remoteDescriptionSet) {
        // 远端描述未就绪：候选缓冲（WebRTC 标准模式，防早到候选被丢弃）
        m_pendingIce.emplace_back(candidate, mid);
        return;
    }
    try {
        m_pc->addRemoteCandidate(rtc::Candidate(candidate, mid));
    } catch (const std::exception& e) {
        // 非法候选/时机不当（如连接已建立后 Late 候选）：忽略不致命
        LX_LOG_WARN("addRemoteCandidate ignored: {}", e.what());
    }
}

void WebRtcPeer::applyRemoteIce(const std::string& candidate, const std::string& mid) {
    try {
        m_pc->addRemoteCandidate(rtc::Candidate(candidate, mid));
    } catch (const std::exception& e) {
        LX_LOG_WARN("addRemoteCandidate ignored: {}", e.what());
    }
}

void WebRtcPeer::markRemoteDescriptionSet() {
    std::lock_guard<std::mutex> lock(m_iceMutex);
    m_remoteDescriptionSet = true;
    for (const auto& pending : m_pendingIce) {
        applyRemoteIce(pending.first, pending.second);
    }
    m_pendingIce.clear();
}

void WebRtcPeer::sendAudioFrame(const int16_t* pcm, size_t samples) {
    if (m_audioTrack == nullptr || m_opusEncoder == nullptr || pcm == nullptr ||
        !isConnected()) {
        return;
    }
    std::vector<unsigned char> encoded(kMaxOpusBytes);
    const opus_int32 nbytes =
        opus_encode(static_cast<OpusEncoder*>(m_opusEncoder), pcm,
                    static_cast<int>(samples), encoded.data(), kMaxOpusBytes);
    if (nbytes <= 0) {
        return;
    }
    m_audioTrack->send(reinterpret_cast<const std::byte*>(encoded.data()),
                       static_cast<size_t>(nbytes));
}

void WebRtcPeer::close() {
    if (m_closed.exchange(true)) {
        return;
    }
    if (m_opusEncoder != nullptr) {
        opus_encoder_destroy(static_cast<OpusEncoder*>(m_opusEncoder));
        m_opusEncoder = nullptr;
    }
    if (m_opusDecoder != nullptr) {
        opus_decoder_destroy(static_cast<OpusDecoder*>(m_opusDecoder));
        m_opusDecoder = nullptr;
    }
    if (m_pc != nullptr) {
        m_pc->close();
        m_pc = nullptr;
    }
    m_audioTrack = nullptr;
}

} // namespace lingxi::client
