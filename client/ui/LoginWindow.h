/**
 * @file LoginWindow.h
 * @brief 登录/注册窗口（Qt Widgets）。
 */
#pragma once

#include <QLineEdit>
#include <QPushButton>
#include <QWidget>

class QLabel;  ///< 前向声明（Qt）

namespace lingxi::client {

/**
 * @brief 登录窗口：账号密码输入、注册与登录动作、状态提示。
 */
class LoginWindow : public QWidget {
    Q_OBJECT
public:
    explicit LoginWindow(QWidget* parent = nullptr);

    /**
     * @brief 自动验收入口：填充账号密码并依次触发注册与登录（--autotest 模式用）。
     */
    void autoTestLogin(const QString& username, const QString& password);

signals:
    /**
     * @brief 登录链路全部成功（TCP 校验通过），主窗口应切换。
     */
    void loginSucceeded();

private:
    /**
     * @brief 触发注册动作。
     */
    void onRegisterClicked();

    /**
     * @brief 触发登录动作。
     */
    void onLoginClicked();

    QLineEdit* m_usernameEdit;   ///< 账号输入
    QLineEdit* m_passwordEdit;   ///< 密码输入
    QPushButton* m_registerBtn;  ///< 注册按钮
    QPushButton* m_loginBtn;     ///< 登录按钮
    QLabel* m_statusLabel;       ///< 状态提示
    bool m_autoLoginAfterRegister = false;  ///< 自动验收：注册成功即登录
};

} // namespace lingxi::client
