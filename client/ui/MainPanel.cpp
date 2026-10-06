/**
 * @file MainPanel.cpp
 * @brief 主面板实现（M3 双 Tab：会话聊天 + 联系人）。
 */
#include "MainPanel.h"

#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QSplitter>
#include <QUrl>
#include <QVBoxLayout>

#include <nlohmann/json.hpp>

#include "client/service/AccountService.h"
#include "client/service/ConversationService.h"
#include "client/service/FileService.h"
#include "client/service/SocialService.h"

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
    setFixedSize(920, 600);

    m_tabs = new QTabWidget(this);

    /* ==================== Tab1 会话 ==================== */
    m_convList = new QListWidget(this);
    m_convList->setFixedWidth(240);
    m_newChatBtn = new QPushButton(QStringLiteral("＋ 发起会话(uid)"), this);
    m_newChatBtn->setFixedWidth(240);

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

    auto* chatSplitter = new QSplitter(this);
    chatSplitter->addWidget(leftWidget);
    chatSplitter->addWidget(rightWidget);
    chatSplitter->setStretchFactor(1, 1);
    m_tabs->addTab(chatSplitter, QStringLiteral("会话"));

    /* ==================== Tab2 联系人 ==================== */
    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setPlaceholderText(QStringLiteral("uid 或用户名前缀…"));
    auto* searchBtn = new QPushButton(QStringLiteral("搜索"), this);

    auto* searchRow = new QHBoxLayout();
    searchRow->addWidget(m_searchEdit, 1);
    searchRow->addWidget(searchBtn);

    m_searchResultList = new QListWidget(this);
    m_friendList = new QListWidget(this);
    m_applyList = new QListWidget(this);

    auto* createGroupBtn = new QPushButton(QStringLiteral("建群（群名,uid,uid…）"), this);

    auto* socialLayout = new QVBoxLayout();
    socialLayout->addLayout(searchRow);
    socialLayout->addWidget(new QLabel(QStringLiteral("搜索结果（双击加好友）"), this));
    socialLayout->addWidget(m_searchResultList, 1);
    socialLayout->addWidget(new QLabel(QStringLiteral("待处理申请"), this));
    socialLayout->addWidget(m_applyList, 1);
    socialLayout->addWidget(new QLabel(QStringLiteral("我的好友（双击开聊，右键删除）"), this));
    socialLayout->addWidget(m_friendList, 2);
    socialLayout->addWidget(createGroupBtn);
    auto* socialWidget = new QWidget(this);
    socialWidget->setLayout(socialLayout);
    m_tabs->addTab(socialWidget, QStringLiteral("联系人"));

    auto* rootLayout = new QHBoxLayout(this);
    rootLayout->addWidget(m_tabs);
    setLayout(rootLayout);
    setAcceptDrops(true);  // M4：拖拽文件发送

    connect(m_msgList, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) {
        const long long msgDbId = item->data(Qt::UserRole).toLongLong();
        if (msgDbId != 0) {
            downloadMessage(msgDbId);
        }
    });

    /* ==================== 交互 ==================== */
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
        if (convId == m_currentConvId && m_tabs->currentIndex() == 0) {
            renderMessages(convId);
            ConversationService::instance().markRead(convId);
        }
    });
    connect(&conv, &ConversationService::messageAcked, this,
            [this](const QString&, long long convId, long long) {
                if (convId == m_currentConvId) {
                    renderMessages(convId);
                }
                refreshConversationList();
            });
    connect(&conv, &ConversationService::syncCompleted, this, [this] {
        refreshConversationList();
        if (m_currentConvId != 0) {
            renderMessages(m_currentConvId);
        }
    });
    connect(&conv, &ConversationService::messageRecalled, this,
            [this](long long convId, long long) {
                if (convId == m_currentConvId) {
                    renderMessages(convId);
                }
            });

    auto& social = SocialService::instance();
    connect(searchBtn, &QPushButton::clicked, this, &MainPanel::onSearchClicked);
    connect(m_searchEdit, &QLineEdit::returnPressed, this, &MainPanel::onSearchClicked);
    connect(&social, &SocialService::searchResultArrived, this, [this] {
        m_searchResultList->clear();
        for (const auto& user : SocialService::instance().searchResult().users()) {
            auto* item = new QListWidgetItem(
                QStringLiteral("%1 (%2) %3%4")
                    .arg(QString::fromStdString(user.nickname()),
                         QString::number(user.uid()),
                         QString::fromStdString(user.signature()),
                         user.is_friend() ? QStringLiteral(" [好友]") : QString()),
                m_searchResultList);
            item->setData(Qt::UserRole, static_cast<long long>(user.uid()));
            item->setData(Qt::UserRole + 1, user.is_friend());
        }
    });
    connect(m_searchResultList, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem* item) {
                if (item->data(Qt::UserRole + 1).toBool()) {
                    return;  // 已是好友
                }
                SocialService::instance().apply(item->data(Qt::UserRole).toLongLong(),
                                                QStringLiteral("加个好友吧").toStdString());
            });
    connect(&social, &SocialService::applyReceived, this,
            [this](const QString& from, const QString& msg) {
                const auto choice = QMessageBox::question(
                    this, QStringLiteral("好友申请"),
                    QStringLiteral("%1 请求加你为好友：%2\n是否同意？").arg(from, msg));
                const auto& pending = SocialService::instance().pendingApplies();
                if (!pending.empty()) {
                    SocialService::instance().handleApply(pending.back().apply_id(),
                                                          choice == QMessageBox::Yes);
                }
            });
    connect(&social, &SocialService::socialError, this, [this](const QString& message) {
        QMessageBox::warning(this, QStringLiteral("操作失败"), message);
    });
    connect(m_applyList, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem* item) {
                const int64_t applyId = item->data(Qt::UserRole).toLongLong();
                if (applyId <= 0) {
                    return;
                }
                const auto choice = QMessageBox::question(this, QStringLiteral("处理申请"),
                                                          QStringLiteral("同意该好友申请？"));
                SocialService::instance().handleApply(applyId, choice == QMessageBox::Yes);
            });
    connect(&social, &SocialService::friendListUpdated, this, &MainPanel::refreshFriends);
    connect(m_friendList, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) {
        m_currentPeerUid = item->data(Qt::UserRole).toLongLong();
        m_currentConvId = 0;
        m_tabs->setCurrentIndex(0);  // 切到会话页即可直接发消息（首条消息建会话）
    });
    m_friendList->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_friendList, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        QListWidgetItem* item = m_friendList->itemAt(pos);
        if (item == nullptr) {
            return;
        }
        QMenu menu(this);
        menu.addAction(QStringLiteral("删除好友"), this, [this, item] {
            SocialService::instance().deleteFriend(item->data(Qt::UserRole).toLongLong());
        });
        menu.exec(m_friendList->mapToGlobal(pos));
    });
    connect(createGroupBtn, &QPushButton::clicked, this, &MainPanel::onCreateGroupClicked);
    connect(&AccountService::instance(), &AccountService::tcpLoginFinished, &social,
            [this](int errCode, const QString&) {
                if (errCode == 0) {
                    SocialService::instance().requestFriendList();
                }
            });
}

void MainPanel::setUserInfo(long long uid, const QString& nickname) {
    m_myUid = uid;
    setWindowTitle(QStringLiteral("灵犀 IM - %1 (uid:%2)").arg(nickname).arg(uid));
    ConversationService::instance().requestConversationList();
    SocialService::instance().requestFriendList();
}

/* ==================== 会话 Tab（M2） ==================== */

void MainPanel::refreshConversationList() {
    const long long selected = m_currentConvId;
    m_convList->clear();
    for (const auto& conv : ConversationService::instance().store().loadConversations()) {
        const int64_t unread = conv.lastSeq - conv.lastReadSeq;
        auto* item = new QListWidgetItem(
            QStringLiteral("%1%2\n%3")
                .arg(conv.peerName.empty() ? QString("会话%1").arg(conv.convId % 100000)
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
    m_currentPeerUid = 0;
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
    auto& store = ConversationService::instance().store();
    for (const auto& msg : store.loadMessages(convId)) {
        QString line;
        if (msg.status == 1) {
            line = QStringLiteral("--- 消息已撤回 ---");
        } else {
            QString body = extractText(msg.payload);
            if (msg.msgType == static_cast<int>(lingxi::MSG_FILE)) {
                body = QStringLiteral("[文件] ") + extractFileName(msg.payload);
            } else if (msg.msgType == static_cast<int>(lingxi::MSG_IMAGE)) {
                body = QStringLiteral("[图片] ") + extractFileName(msg.payload);
            } else if (msg.msgType == static_cast<int>(lingxi::MSG_VOICE)) {
                body = QStringLiteral("[语音] ") + extractFileName(msg.payload);
            }
            if (msg.fromUid == m_myUid) {
                line = QStringLiteral("我: %1%2")
                           .arg(body, msg.status == -1 ? QStringLiteral("（发送中…）") : QString());
            } else {
                line = QStringLiteral("对方: %1").arg(body);
            }
        }
        auto* item = new QListWidgetItem(line, m_msgList);
        try {
            auto parsed = nlohmann::json::parse(msg.payload);
            if (parsed.contains("msgId")) {
                item->setData(Qt::UserRole, static_cast<long long>(parsed["msgId"]));
            }
        } catch (const nlohmann::json::exception&) {
        }
    }
    m_msgList->scrollToBottom();
}

QString MainPanel::extractFileName(const std::string& payload) {
    try {
        auto parsed = nlohmann::json::parse(payload);
        if (parsed.contains("name") && parsed["name"].is_string()) {
            return QString::fromStdString(parsed["name"].get<std::string>());
        }
    } catch (const nlohmann::json::exception&) {
    }
    return QString();
}

void MainPanel::onSendClicked() {
    const QString text = m_inputEdit->text().trimmed();
    if (text.isEmpty()) {
        return;
    }
    if (m_currentConvId > 0) {
        ConversationService::instance().sendTextToConv(m_currentConvId, text.toStdString());
    } else if (m_currentPeerUid > 0) {
        ConversationService::instance().sendText(m_currentPeerUid, text.toStdString());
    } else {
        return;
    }
    m_inputEdit->clear();
    if (m_currentConvId > 0) {
        renderMessages(m_currentConvId);
    }
}

void MainPanel::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
    }
}

void MainPanel::dropEvent(QDropEvent* event) {
    for (const QUrl& url : event->mimeData()->urls()) {
        if (url.isLocalFile()) {
            uploadAndSend(url.toLocalFile());
        }
    }
    event->acceptProposedAction();
}

void MainPanel::uploadAndSend(const QString& filePath) {
    if (m_currentConvId <= 0 && m_currentPeerUid <= 0) {
        QMessageBox::information(this, QStringLiteral("提示"),
                                 QStringLiteral("先打开一个会话再发送文件"));
        return;
    }
    const int64_t convId = m_currentConvId;
    const int64_t peerUid = m_currentPeerUid;
    const QString name = QFileInfo(filePath).fileName();
    const qint64 size = QFileInfo(filePath).size();
    const bool isImage = name.endsWith(".png", Qt::CaseInsensitive) ||
                         name.endsWith(".jpg", Qt::CaseInsensitive) ||
                         name.endsWith(".jpeg", Qt::CaseInsensitive);
    m_msgList->addItem(QStringLiteral("正在上传 %1 …").arg(name));

    FileService::instance().uploadFile(
        filePath,
        [this, name](int pct) {
            m_msgList->addItem(QStringLiteral("… %1 %2%%").arg(name).arg(pct));
            m_msgList->scrollToBottom();
        },
        [this, convId, peerUid, name, size, isImage](const QString& fid, const QString& err) {
            if (fid.isEmpty()) {
                QMessageBox::warning(this, QStringLiteral("上传失败"), err);
                return;
            }
            ConversationService::instance().sendFileMessage(
                convId, peerUid,
                isImage ? static_cast<int>(lingxi::MSG_IMAGE)
                        : static_cast<int>(lingxi::MSG_FILE),
                fid.toStdString(), name.toStdString(), size);
            if (convId > 0) {
                renderMessages(convId);
            }
            refreshConversationList();
        });
}

void MainPanel::downloadMessage(long long msgDbId) {
    auto& store = ConversationService::instance().store();
    for (const auto& msg : store.loadMessages(m_currentConvId)) {
        try {
            auto parsed = nlohmann::json::parse(msg.payload);
            if (!parsed.contains("msgId") ||
                static_cast<long long>(parsed["msgId"]) != msgDbId || !parsed.contains("fid")) {
                continue;
            }
            const QString fid = QString::fromStdString(parsed["fid"].get<std::string>());
            const QString name =
                parsed.contains("name")
                    ? QString::fromStdString(parsed["name"].get<std::string>())
                    : fid + ".bin";
            const QString saveDir = QDir::homePath() + "/Downloads/lingxi";
            QDir().mkpath(saveDir);
            const QString savePath = saveDir + "/" + name;
            auto* progressItem =
                new QListWidgetItem(QStringLiteral("下载中 %1 …").arg(name), m_msgList);
            FileService::instance().downloadFile(
                fid, savePath,
                [this, progressItem](int pct) {
                    progressItem->setText(QStringLiteral("下载 %1%%").arg(pct));
                },
                [this, progressItem, savePath,
                 name](bool ok, const QString& path, const QString& err) {
                    if (!ok) {
                        progressItem->setText(QStringLiteral("下载失败：%1").arg(err));
                        return;
                    }
                    progressItem->setText(QStringLiteral("已保存：%1").arg(path));
                    if (name.endsWith(".png", Qt::CaseInsensitive) ||
                        name.endsWith(".jpg", Qt::CaseInsensitive) ||
                        name.endsWith(".jpeg", Qt::CaseInsensitive)) {
                        QMessageBox preview(this);
                        preview.setWindowTitle(name);
                        QPixmap pixmap(path);
                        preview.setIconPixmap(pixmap.scaled(480, 480, Qt::KeepAspectRatio));
                        preview.exec();
                    }
                });
            return;
        } catch (const nlohmann::json::exception&) {
        }
    }
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
    m_currentConvId = 0;
    ConversationService::instance().sendText(peerUid, QStringLiteral("你好！").toStdString());
}

/* ==================== 联系人 Tab（M3） ==================== */

void MainPanel::refreshFriends() {
    m_friendList->clear();
    m_applyList->clear();
    for (const auto& friendItem : SocialService::instance().friends().friends()) {
        const QString name = friendItem.remark().empty()
                                 ? QString::fromStdString(friendItem.nickname())
                                 : QString::fromStdString(friendItem.remark());
        auto* item = new QListWidgetItem(
            QStringLiteral("%1 (%2)%3")
                .arg(name, QString::number(friendItem.uid()),
                     friendItem.online() ? QStringLiteral("  [在线]") : QStringLiteral("  [离线]")),
            m_friendList);
        item->setData(Qt::UserRole, static_cast<long long>(friendItem.uid()));
    }
    // 待处理申请渲染（双击 → 弹窗选择同意/拒绝）
    const auto& pending = SocialService::instance().pendingApplies();
    for (const auto& apply : pending) {
        auto* item = new QListWidgetItem(
            QStringLiteral("#%1 %2：%3（双击处理）")
                .arg(apply.apply_id() % 100000)
                .arg(QString::fromStdString(apply.from_nickname()),
                     QString::fromStdString(apply.verify_msg())),
            m_applyList);
        item->setData(Qt::UserRole, static_cast<long long>(apply.apply_id()));
    }
    if (pending.empty() && SocialService::instance().friends().pending_applies() > 0) {
        m_applyList->addItem(QStringLiteral("待处理申请 %1 条（本会话启动前收到，随通知弹窗处理）")
                                 .arg(SocialService::instance().friends().pending_applies()));
    }
}

void MainPanel::onSearchClicked() {
    const QString keyword = m_searchEdit->text().trimmed();
    if (keyword.isEmpty()) {
        return;
    }
    SocialService::instance().search(keyword.toStdString());
}

void MainPanel::onCreateGroupClicked() {
    bool ok = false;
    const QString input = QInputDialog::getText(this, QStringLiteral("建群"),
                                                QStringLiteral("群名,成员uid,成员uid…："),
                                                QLineEdit::Normal, QString(), &ok);
    if (!ok || input.isEmpty()) {
        return;
    }
    const QStringList parts = input.split(',');
    if (parts.isEmpty()) {
        return;
    }
    std::vector<long long> memberUids;
    for (int i = 1; i < parts.size(); ++i) {
        memberUids.push_back(parts[i].toLongLong());
    }
    SocialService::instance().createGroup(parts[0].toStdString(), memberUids);
}

} // namespace lingxi::client
