/**
 * @file WebRtcPeer.h
 * @brief M5 媒体层：libdatachannel PeerConnection 封装——音频轨 + Opus 编解码 + RTP 打包链。
 *
 * 纯 C++（无 Qt）；音频采集/播放由上层（CallManager/CallWindow 的 Qt Audio）驱动：
 * - 上层每 20ms 送 960 个 16bit@48kHz 单声道采样 → sendAudioFrame()（Opus 编码 → RTP → SRTP）；
 * - 对端 RTP 到达 → 剥头 → Opus 解码 → onRemoteAudio 回调。
 * SDP/ICE 经 CallService 信令通道交换（docs/05 §3）。
 */
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>

#include <rtc/peerconnection.hpp>

#include "lingxi/base/NonCopyable.h"

namespace lingxi::client {

/**
 * @brief 单路 1v1 音频 WebRTC 会话。
 */
class WebRtcPeer : private NonCopyable, public std::enable_shared_from_this<WebRtcPeer> {
public:
    /**
     * @brief PeerConnection 状态（映射 rtc::PeerConnection::State，供 Qt 信号用 int）。
     */
    enum class PcState : int { New = 0, Connecting = 1, Connected = 2, Disconnected = 3, Failed = 4, Closed = 5 };

    /** 本地 SDP 就绪（Offer 或 Answer，经信令通道发给对端） */
    using SdpCallback = std::function<void(const std::string& sdp, const std::string& type)>;
    /** 本地 ICE 候选就绪 */
    using IceCallback = std::function<void(const std::string& candidate, const std::string& mid)>;
    /** 连接状态回调 */
    using StateCallback = std::function<void(PcState state)>;
    /** 对端音频帧（解码后的 16bit@48kHz 单声道 PCM） */
    using AudioCallback = std::function<void(const int16_t* pcm, size_t samples)>;

    WebRtcPeer();
    ~WebRtcPeer();

    /**
     * @brief 设置回调（须在 init 之前）。
     */
    void setCallbacks(SdpCallback onSdp, IceCallback onIce, StateCallback onState,
                      AudioCallback onRemoteAudio);

    /**
     * @brief 以主叫身份初始化：创建音频轨并生成 Offer。
     * @param withAudio 是否初始化 Opus 编解码（信令联调/测试可关）
     * @return bool 成功为 true
     */
    bool initAsCaller(bool withAudio = true);

    /**
     * @brief 以被叫身份初始化：设置远端 Offer，等待 onLocalDescription 产出 Answer。
     */
    bool initAsCallee(const std::string& remoteOfferSdp, bool withAudio = true);

    /**
     * @brief 主叫收到 Answer 后设置远端描述。
     */
    bool setRemoteAnswer(const std::string& answerSdp);

    /**
     * @brief 设置对端 ICE 候选。
     */
    void setRemoteIce(const std::string& candidate, const std::string& mid);

    /**
     * @brief 发送一帧 PCM（960 样本 = 20ms @48kHz 单声道）。
     */
    void sendAudioFrame(const int16_t* pcm, size_t samples);

    /**
     * @brief 关闭连接（析构自动调用）。
     */
    void close();

    /**
     * @brief 是否处于 Connected 状态。
     */
    bool isConnected() const { return m_state == static_cast<int>(PcState::Connected); }

    /**
     * @brief 当前状态。
     */
    PcState state() const { return static_cast<PcState>(m_state.load()); }

private:
    /**
     * @brief 初始化 Opus 编解码器（音频模式）。
     */
    bool initAudioCodecs();

    /**
     * @brief 应用远端候选（远端描述就绪后的统一入口；提前到达的候选在队列缓冲）。
     */
    void applyRemoteIce(const std::string& candidate, const std::string& mid);

    /**
     * @brief 标记远端描述已设置并冲刷缓冲的候选。
     */
    void markRemoteDescriptionSet();

    /**
     * @brief 组装媒体处理链（RtpPacketizer + SR + NACK）。
     */
    void setupMediaChain(const std::shared_ptr<rtc::Track>& track);

    std::shared_ptr<rtc::PeerConnection> m_pc;   ///< PeerConnection
    std::shared_ptr<rtc::Track> m_audioTrack;    ///< 音频轨
    void* m_opusEncoder = nullptr;               ///< OpusEncoder*（void* 隔离头依赖）
    void* m_opusDecoder = nullptr;               ///< OpusDecoder*
    std::atomic<int> m_state{0};                 ///< PcState
    std::atomic<bool> m_closed{false};
    bool m_withAudio = true;                     ///< 是否音频模式

    std::mutex m_iceMutex;                                           ///< 远端候选缓冲锁
    std::vector<std::pair<std::string, std::string>> m_pendingIce;   ///< 早到的远端候选
    bool m_remoteDescriptionSet = false;                             ///< 远端描述是否就绪

    SdpCallback m_onSdp;
    IceCallback m_onIce;
    StateCallback m_onState;
    AudioCallback m_onRemoteAudio;
};

} // namespace lingxi::client
