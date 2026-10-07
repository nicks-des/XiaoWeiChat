/**
 * @file CallWindow.cpp
 * @brief M5 通话窗口实现。
 */
#include "CallWindow.h"

#include <QVBoxLayout>

namespace lingxi::client {

namespace {

/** 窗口尺寸 */
constexpr int kWindowWidth = 360;
constexpr int kWindowHeight = 200;

} // namespace

CallWindow::CallWindow(QWidget* parent) : QWidget(parent) {
    setWindowTitle(QStringLiteral("灵犀通话"));
    setFixedSize(kWindowWidth, kWindowHeight);

    m_titleLabel = new QLabel(this);
    m_titleLabel->setAlignment(Qt::AlignCenter);
    m_timeLabel = new QLabel(this);
    m_timeLabel->setAlignment(Qt::AlignCenter);

    m_acceptBtn = new QPushButton(QStringLiteral("接听"), this);
    m_hangupBtn = new QPushButton(QStringLiteral("挂断"), this);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->addWidget(m_titleLabel);
    layout->addWidget(m_timeLabel);
    layout->addWidget(m_acceptBtn);
    layout->addWidget(m_hangupBtn);

    m_tickTimer.setInterval(1000);
    connect(&m_tickTimer, &QTimer::timeout, this, [this] {
        const qint64 sec = m_elapsed.elapsed() / 1000;
        m_timeLabel->setText(QStringLiteral("%1:%2")
                                 .arg(sec / 60, 2, 10, QLatin1Char('0'))
                                 .arg(sec % 60, 2, 10, QLatin1Char('0')));
    });
}

void CallWindow::showIncoming(long long callerUid, const QString& callerName, bool video) {
    m_titleLabel->setText(QStringLiteral("%1\n%2 来电").arg(
        callerName, video ? QStringLiteral("[视频]") : QStringLiteral("[音频]")));
    m_timeLabel->setText(QStringLiteral("00:00"));
    m_acceptBtn->show();
    m_hangupBtn->setText(QStringLiteral("拒绝"));
    show();
    raise();
    activateWindow();
}

void CallWindow::showDialing(const QString& peerName, bool outgoing) {
    m_titleLabel->setText(QStringLiteral("%1\n%2").arg(
        peerName, outgoing ? QStringLiteral("呼叫中…") : QStringLiteral("已接听，协商中…")));
    m_timeLabel->setText(QString());
    m_acceptBtn->hide();
    m_hangupBtn->setText(QStringLiteral("挂断"));
    show();
    raise();
}

void CallWindow::markConnected() {
    m_acceptBtn->hide();
    m_elapsed.start();
    m_tickTimer.start();
    m_titleLabel->setText(m_titleLabel->text().section('\n', 0, 0) + QStringLiteral("\n通话中"));
}

} // namespace lingxi::client
