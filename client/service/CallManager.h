/**
 * @file CallManager.h
 * @brief M5 通话编排服务（QtCore）：信令 ↔ WebRtcPeer ↔ 通话 UI 的桥梁。
 *
 * 职责：0x06xx 信令收发（经 AccountService 转发）、WebRtcPeer 生命周期、
 * 音频采集（QAudioInput 20ms 定时拉取）与播放（QAudioOutput 推式写入）。
 */
#pragma once

#include <QIODevice>
#include <QObject>
#include <QString>
#include <QTimer>

#include <memory>

#include "client/media/WebRtcPeer.h"

class QAudioInput;
class QAudioOutput;

namespace lingxi::client {

/**
 * @brief 通话管理服务（单例，归属 UI 线程）。
 */
class CallManager : public QObject {
    Q_OBJECT
public:
    static CallManager& instance();

    /**
     * @brief 初始化（注入长连接；重复调用忽略）。
     */
    void setup(class TcpClient* tcp);

    /**
     * @brief IO 线程收帧入口（AccountService 转发 0x0600~0x060F）。
     */
    void onPacket(uint16_t msgId, const std::string& body);

    /**
     * @brief 发起音频通话（生成 Offer → 0x0601）。
     */
    void startCall(long long peerUid);

    /**
     * @brief 接听来电（Answer → 0x0603）。
     */
    void acceptCall();

    /**
     * @brief 拒绝来电（0x0604）。
     */
    void rejectCall();

    /**
     * @brief 挂断（0x0606，主叫取消走同一接口按状态路由 0x0605）。
     */
    void hangup();

    /**
     * @brief 当前是否在通话中（含振铃）。
     */
    bool inCall() const { return m_inCall; }

signals:
    /// 来电（被叫侧，UI 弹接听窗）
    void incomingCall(long long callerUid, const QString& callerName, bool video);
    /// 通话接通（双方：WebRTC Connected）
    void callConnected();
    /// 通话结束（reason：对端挂断/拒绝/超时/取消/错误）
    void callEnded(const QString& reason);
    /// 呼叫阶段提示（振铃中/对端忙等）
    void callStatus(const QString& status);

private slots:
    /**
     * @brief 20ms 音频采集节拍（拉取 PCM → 编码发送）。
     */
    void onCaptureTick();

private:
    CallManager() = default;

    /**
     * @brief 在 UI 线程发射信号的便捷封装。
     */
    template <typename Fn>
    void emitOnUi(Fn&& fn);

    /**
     * @brief 创建 WebRtcPeer 并注册角色无关回调（主叫/被叫共用）。
     */
    void createPeer();

    /**
     * @brief 启动音频链路（采集 + 播放设备）。
     * @return bool 成功为 true（失败降级为无声通话）
     */
    bool startAudioDevices();

    /**
     * @brief 停止音频链路。
     */
    void stopAudioDevices();

    /**
     * @brief 结束本地会话状态（UI + 音频 + WebRtcPeer 清理）。
     */
    void teardown(const QString& reason);

    class TcpClient* m_tcp = nullptr;        ///< 长连接（借宿主）
    std::shared_ptr<WebRtcPeer> m_peer;      ///< WebRTC 会话
    std::string m_callId;                    ///< 当前通话 ID
    long long m_peerUid = 0;                 ///< 对端 uid
    bool m_isCaller = false;                 ///< 是否主叫
    bool m_inCall = false;                   ///< 通话中（含振铃）
    bool m_accepted = false;                 ///< 已接通

    QTimer m_captureTimer;                   ///< 20ms 采集节拍
    QAudioInput* m_audioInput = nullptr;     ///< 采集设备
    QIODevice* m_inputDevice = nullptr;      ///< 采集流
    QAudioOutput* m_audioOutput = nullptr;   ///< 播放设备
    QIODevice* m_outputDevice = nullptr;     ///< 播放流
};

} // namespace lingxi::client
