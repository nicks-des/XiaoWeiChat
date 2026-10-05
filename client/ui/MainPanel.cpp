/**
 * @file MainPanel.cpp
 * @brief 主面板骨架实现。
 */
#include "MainPanel.h"

#include <QVBoxLayout>

namespace lingxi::client {

MainPanel::MainPanel(QWidget* parent) : QWidget(parent) {
    setWindowTitle(QStringLiteral("灵犀 IM"));
    setFixedSize(720, 480);

    m_userLabel = new QLabel(this);
    m_userLabel->setAlignment(Qt::AlignCenter);

    m_logoutBtn = new QPushButton(QStringLiteral("退出登录"), this);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 16, 24, 16);
    layout->addWidget(m_userLabel);
    layout->addStretch(1);
    layout->addWidget(m_logoutBtn);

    connect(m_logoutBtn, &QPushButton::clicked, this, &MainPanel::logoutRequested);
}

void MainPanel::setUserInfo(long long uid, const QString& nickname) {
    m_userLabel->setText(QStringLiteral("<h3>%1</h3><p>uid: %2</p><p>会话列表即将到来（M2 消息内核）</p>")
                             .arg(nickname, QString::number(uid)));
}

} // namespace lingxi::client
