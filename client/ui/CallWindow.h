/**
 * @file CallWindow.h
 * @brief M5 通话窗口：来电接听/拒绝 + 呼叫中/通话中计时 + 挂断。
 */
#pragma once

#include <QElapsedTimer>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QWidget>

namespace lingxi::client {

/**
 * @brief 通话窗口（单例式，随 CallManager 状态切换形态）。
 */
class CallWindow : public QWidget {
    Q_OBJECT
public:
    explicit CallWindow(QWidget* parent = nullptr);

    /**
     * @brief 来电形态：显示对方并给接听/拒绝按钮。
     */
    void showIncoming(long long callerUid, const QString& callerName, bool video);

    /**
     * @brief 呼叫中/通话中形态。
     * @param peerName  对端名
     * @param outgoing  是否主叫（决定初始文案）
     */
    void showDialing(const QString& peerName, bool outgoing);

signals:
    /**
     * @brief 用户选择接听。
     */
    void acceptClicked();

    /**
     * @brief 用户选择拒绝/挂断。
     */
    void rejectClicked();

private:
    /**
     * @brief 切换为通话中（启动计时）。
     */
    void markConnected();

    QLabel* m_titleLabel;      ///< 对端名/状态
    QLabel* m_timeLabel;       ///< 通话计时
    QPushButton* m_acceptBtn;  ///< 接听（仅来电态）
    QPushButton* m_hangupBtn;  ///< 拒绝/挂断
    QElapsedTimer m_elapsed;   ///< 计时器
    QTimer m_tickTimer;        ///< 秒级刷新
};

} // namespace lingxi::client
