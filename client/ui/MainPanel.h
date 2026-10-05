/**
 * @file MainPanel.h
 * @brief 主面板骨架（M1）：登录后展示当前用户；会话列表随 M2 接入。
 */
#pragma once

#include <QLabel>
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
     * @brief 设置当前登录用户并刷新展示。
     */
    void setUserInfo(long long uid, const QString& nickname);

signals:
    /**
     * @brief 用户点击退出登录。
     */
    void logoutRequested();

private:
    QLabel* m_userLabel;     ///< 当前用户展示
    QPushButton* m_logoutBtn; ///< 退出登录按钮
};

} // namespace lingxi::client
