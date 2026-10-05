/**
 * @file MainPanel.h
 * @brief 主面板（M2 聊天窗口 v1）：左会话列表 + 右消息流 + 输入发送 + 发起会话。
 */
#pragma once

#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QWidget>

namespace lingxi::client {

/**
 * @brief 主面板。
 */
class MainPanel : public QWidget {
    Q_OBJECT
public:
    explicit MainPanel(QWidget* parent = nullptr);

    /**
     * @brief 设置当前登录用户并刷新会话列表。
     */
    void setUserInfo(long long uid, const QString& nickname);

signals:
    /**
     * @brief 用户点击退出登录。
     */
    void logoutRequested();

private:
    /**
     * @brief 从本地缓存刷新会话列表。
     */
    void refreshConversationList();

    /**
     * @brief 打开会话：渲染消息并标记已读。
     */
    void openConversation(long long convId);

    /**
     * @brief 从本地缓存渲染当前会话消息流。
     */
    void renderMessages(long long convId);

    /**
     * @brief 发送输入框内容。
     */
    void onSendClicked();

    /**
     * @brief 发起单聊：输入对端 uid，发一条打招呼消息即建会话（好友体系在 M3）。
     */
    void onStartChatClicked();

    long long m_myUid = 0;              ///< 当前登录 uid
    long long m_currentConvId = 0;      ///< 当前打开的会话
    long long m_currentPeerUid = 0;     ///< 当前会话对端 uid
    QListWidget* m_convList;            ///< 会话列表
    QListWidget* m_msgList;             ///< 消息流
    QLineEdit* m_inputEdit;             ///< 输入框
    QPushButton* m_sendBtn;             ///< 发送按钮
    QPushButton* m_newChatBtn;          ///< 发起会话按钮
};

} // namespace lingxi::client
