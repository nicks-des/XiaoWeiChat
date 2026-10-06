# -*- coding: utf-8 -*-
"""M4 客户端接线脚本：MainPanel 拖拽/渲染/下载。"""
p = 'client/ui/MainPanel.cpp'
s = open(p, encoding='utf-8').read()

s = s.replace('''#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QSplitter>
#include <QVBoxLayout>''',
'''#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QSplitter>
#include <QUrl>
#include <QVBoxLayout>''')
s = s.replace('#include "client/service/ConversationService.h"',
              '#include "client/service/ConversationService.h"\n#include "client/service/FileService.h"')

old = '''    auto* rootLayout = new QHBoxLayout(this);
    rootLayout->addWidget(m_tabs);
    setLayout(rootLayout);'''
new = '''    auto* rootLayout = new QHBoxLayout(this);
    rootLayout->addWidget(m_tabs);
    setLayout(rootLayout);
    setAcceptDrops(true);  // M4：拖拽文件发送

    connect(m_msgList, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) {
        const long long msgDbId = item->data(Qt::UserRole).toLongLong();
        if (msgDbId != 0) {
            downloadMessage(msgDbId);
        }
    });'''
assert old in s, 'root layout anchor'
s = s.replace(old, new)

old = '''void MainPanel::renderMessages(long long convId) {
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
}'''
new = '''void MainPanel::renderMessages(long long convId) {
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
}'''
assert old in s, 'renderMessages anchor'
s = s.replace(old, new)

old = 'void MainPanel::onStartChatClicked() {'
new = '''void MainPanel::dragEnterEvent(QDragEnterEvent* event) {
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

void MainPanel::onStartChatClicked() {'''
assert old in s, 'onStartChat anchor'
s = s.replace(old, new, 1)

# MainPanel.h 声明 extractFileName
h = 'client/ui/MainPanel.h'
sh = open(h, encoding='utf-8').read()
sh = sh.replace('''    /**
     * @brief 下载消息中的文件/图片（双击触发）。
     */
    void downloadMessage(long long msgDbId);''',
'''    /**
     * @brief 下载消息中的文件/图片（双击触发）。
     */
    void downloadMessage(long long msgDbId);

    /**
     * @brief 从消息 payload 提取文件名（文件/图片渲染）。
     */
    static QString extractFileName(const std::string& payload);''')
open(h, 'w', encoding='utf-8', newline='\n').write(sh)
print('header ok')

open(p, 'w', encoding='utf-8', newline='\n').write(s)
print('MainPanel.cpp ok')
