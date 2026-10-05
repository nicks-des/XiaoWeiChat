/**
 * @file MainPanel.cpp
 * @brief 主面板实现（M2 聊天窗口 v1）。
 */
#include "MainPanel.h"

#include <QInputDialog>
#include <QSplitter>
#include <QVBoxLayout>

#include <nlohmann/json.hpp>

#include "client/service/AccountService.h"
#include "client/service/ConversationService.h"

namespace lingxi::client {

namespace {

/**
 * @brief 从消息 JSON 信封提取文本。
 */
QString extractText(const std::string& payload) {
    try {
        auto parsed = nlohmann::json::parse(payload);
        if (parsed.contains("text") && parsed["text"].is_string()) {
            return QString::fromStdString(parsed["text"].get<std::string>());
        }
    } catch (const nlohmann::json::exception&) {
    }
    return QStringLiteral("[消息]");
}

} // namespace

MainPanel::MainPanel(QWidget* parent) : QWidget(parent) {
    setWindowTitle(QStringLiteral("灵犀 IM"));
    setFixedSize(860, 560);

    // ---- 左侧会话列表 ----
    m_convList = new QListWidget(this);
    m_convList->setFixedWidth(240);
    m_newChatBtn = new QPushButton(QStringLiteral("＋ 发起会话(uid)"), this);
    m_newChatBtn->setFixedWidth(240);

    // ---- 右侧消息区 ----
    m_msgList = new QListWidget(this);
    m_msgList->setWordWrap(true);
    m_msgList->setSpacing(4);
    m_inputEdit = new QLineEdit(this);
    m_inputEdit->setPlaceholderText(QStringLiteral("输入消息…（Enter 发送）"));
    m_sendBtn = new QPushButton(QStringLiteral("发送"), this);

    auto* inputRow = new QHBoxLayout();
    inputRow->addWidget(m_inputEdit, 1);
    inputRow->addWidget(m_sendBtn);

    auto* rightLayout = new QVBoxLayout();
    rightLayout->addWidget(m_msgList, 1);
    rightLayout->addLayout(inputRow);

    auto* rightWidget = new QWidget(this);
    rightWidget->setLayout(rightLayout);

    auto* leftLayout = new QVBoxLayout();
    leftLayout->addWidget(m_convList, 1);
    leftLayout->addWidget(m_newChatBtn);
    auto* leftWidget = new QWidget(this);
    leftWidget->setLayout(leftLayout);

    auto* splitter = new QSplitter(this);
    splitter->addWidget(leftWidget);
    splitter->addWidget(rightWidget);
    splitter->setStretchFactor(1, 1);

    auto* layout = new QHBoxLayout(this);
    layout->addWidget(splitter);
    setLayout(layout);

    // ---- 交互 ----
    connect(m_sendBtn, &QPushButton::clicked, this, &MainPanel::onSendClicked);
    connect(m_inputEdit, &QLineEdit::returnPressed, this, &MainPanel::onSendClicked);
    connect(m_newChatBtn, &QPushButton::clicked, this, &MainPanel::onStartChatClicked);
    connect(m_convList, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        openConversation(item->data(Qt::UserRole).toLongLong());
    });

    auto& conv = ConversationService::instance();
    connect(&conv, &ConversationService::conversationListUpdated, this,
            &MainPanel::refreshConversationList);
    connect(&conv, &ConversationService::messageArrived, this, [this](long long convId) {
        refreshConversationList();
        if (convId == m_currentConvId) {
            renderMessages(convId);
            ConversationService::instance().markRead(convId);  // 打开中即已读
        }
    });
    connect(&conv, &ConversationService::messageAcked, this,
            [this](const QString& clientMsgId, long long convId, long long) {
                if (convId == m_currentConvId) {
                    renderMessages(convId);  // ACK 回填 seq 后刷新状态
                }
                refreshConversationList();
            });
    connect(&conv, &ConversationService::syncCompleted, this, [this] {
        refreshConversationList();
        if (m_currentConvId != 0) {
            renderMessages(m_currentConvId);
        }
    });
    connect(&conv, &ConversationService::messageRecalled, this, [this](long long convId, long long) {
        if (convId == m_currentConvId) {
            renderMessages(convId);
        }
    });
}

void MainPanel::setUserInfo(long long uid, const QString& nickname) {
    m_myUid = uid;
    setWindowTitle(QStringLiteral("灵犀 IM - %1 (uid:%2)").arg(nickname).arg(uid));
    ConversationService::instance().requestConversationList();
}

void MainPanel::refreshConversationList() {
    const long long selected = m_currentConvId;
    m_convList->clear();
    for (const auto& conv : ConversationService::instance().store().loadConversations()) {
        const int64_t unread = conv.lastSeq - conv.lastReadSeq;
        auto* item = new QListWidgetItem(
            QStringLiteral("%1%2\n%3")
                .arg(conv.peerName.empty() ? QString("用户%1").arg(conv.peerUid)
                                           : QString::fromStdString(conv.peerName),
                     unread > 0 ? QStringLiteral("  [未读 %1]").arg(unread) : QString(),
                     QString::fromStdString(conv.preview)),
            m_convList);
        item->setData(Qt::UserRole, static_cast<long long>(conv.convId));
        if (conv.convId == selected) {
            item->setSelected(true);
        }
    }
}

void MainPanel::openConversation(long long convId) {
    m_currentConvId = convId;
    for (const auto& conv : ConversationService::instance().store().loadConversations()) {
        if (conv.convId == convId) {
            m_currentPeerUid = conv.peerUid;
            break;
        }
    }
    renderMessages(convId);
    ConversationService::instance().markRead(convId);
    refreshConversationList();
}

void MainPanel::renderMessages(long long convId) {
    m_msgList->clear();
    for (const auto& msg : ConversationService::instance().store().loadMessages(convId)) {
        QString line;
        if (msg.status == 1) {
            line = QStringLiteral("--- 消息已撤回 ---");
        } else if (msg.fromUid == m_myUid) {
            line = QStringLiteral("我: %1%2")
                       .arg(extractText(msg.payload),
                            msg.status == -1 ? QStringLiteral("（发送中…）") : QString());
        } else {
            line = QStringLiteral("对方: %1").arg(extractText(msg.payload));
        }
        m_msgList->addItem(line);
    }
    m_msgList->scrollToBottom();
}

void MainPanel::onSendClicked() {
    const QString text = m_inputEdit->text().trimmed();
    if (text.isEmpty() || m_currentPeerUid <= 0) {
        return;
    }
    ConversationService::instance().sendText(m_currentPeerUid, text.toStdString());
    m_inputEdit->clear();
    renderMessages(m_currentConvId);  // 立即展示「发送中」气泡
}

void MainPanel::onStartChatClicked() {
    bool ok = false;
    const int64_t peerUid = QInputDialog::getInt(this, QStringLiteral("发起会话"),
                                                 QStringLiteral("对端 uid："), 0, 1,
                                                 2147483647, 1, &ok);
    if (!ok || peerUid <= 0) {
        return;
    }
    m_currentPeerUid = peerUid;
    m_currentConvId = 0;  // 会话由服务端首条消息创建，ACK/通知后自动出现在列表
    ConversationService::instance().sendText(peerUid, QStringLiteral("你好！").toStdString());
}

} // namespace lingxi::client
