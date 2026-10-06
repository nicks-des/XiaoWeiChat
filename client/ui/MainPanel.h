/**
 * @file MainPanel.h
 * @brief 主面板（M3）：Tab1 会话聊天（M2）+ Tab2 联系人（好友/申请/搜索/建群）。
 */
#pragma once

#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QTabWidget>
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
     * @brief 设置当前登录用户并刷新各列表。
     */
    void setUserInfo(long long uid, const QString& nickname);

signals:
    /**
     * @brief 用户点击退出登录。
     */
    void logoutRequested();

private:
    /* ---- 会话 Tab（M2） ---- */

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
     * @brief 发送输入框内容（群会话走 conv 路径，单聊未建会话走 to_uid 路径）。
     */
    void onSendClicked();

    /**
     * @brief 发起单聊：输入对端 uid。
     */
    void onStartChatClicked();

    /* ---- 联系人 Tab（M3） ---- */

    /**
     * @brief 刷新好友列表与申请红点。
     */
    void refreshFriends();

    /**
     * @brief 执行搜索（搜索框内容）。
     */
    void onSearchClicked();

    /**
     * @brief 执行建群（弹窗输入群名与成员 uid 逗号分隔）。
     */
    void onCreateGroupClicked();

    /* ---- 成员 ---- */
    long long m_myUid = 0;              ///< 当前登录 uid
    long long m_currentConvId = 0;      ///< 当前打开的会话
    long long m_currentPeerUid = 0;     ///< 当前会话对端 uid

    QTabWidget* m_tabs;                 ///< 页签：会话 / 联系人

    /* 会话 Tab */
    QListWidget* m_convList;            ///< 会话列表
    QListWidget* m_msgList;             ///< 消息流
    QLineEdit* m_inputEdit;             ///< 输入框
    QPushButton* m_sendBtn;             ///< 发送按钮
    QPushButton* m_newChatBtn;          ///< 发起会话按钮

    /* 联系人 Tab */
    QLineEdit* m_searchEdit;            ///< 搜索框
    QListWidget* m_searchResultList;    ///< 搜索结果
    QListWidget* m_friendList;          ///< 好友列表
    QListWidget* m_applyList;           ///< 待处理申请
};

} // namespace lingxi::client
