/**
 * @file LoginWindow.cpp
 * @brief 登录/注册窗口实现。
 */
#include "LoginWindow.h"

#include <QLabel>
#include <QVBoxLayout>

#include "client/service/AccountService.h"

namespace lingxi::client {

namespace {

/** 窗口固定尺寸 */
constexpr int kWindowWidth = 420;
constexpr int kWindowHeight = 280;

} // namespace

LoginWindow::LoginWindow(QWidget* parent) : QWidget(parent) {
    setWindowTitle(QStringLiteral("灵犀 IM - 登录"));
    setFixedSize(kWindowWidth, kWindowHeight);

    auto* title = new QLabel(QStringLiteral("<h2>灵犀 IM</h2>"), this);
    title->setAlignment(Qt::AlignCenter);

    m_usernameEdit = new QLineEdit(this);
    m_usernameEdit->setPlaceholderText(QStringLiteral("用户名（3-32 位）"));

    m_passwordEdit = new QLineEdit(this);
    m_passwordEdit->setPlaceholderText(QStringLiteral("密码（6-64 位）"));
    m_passwordEdit->setEchoMode(QLineEdit::Password);

    m_registerBtn = new QPushButton(QStringLiteral("注册"), this);
    m_loginBtn = new QPushButton(QStringLiteral("登录"), this);
    m_loginBtn->setDefault(true);

    m_statusLabel = new QLabel(QString(), this);
    m_statusLabel->setAlignment(Qt::AlignCenter);
    m_statusLabel->setStyleSheet("color: #666;");

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(48, 32, 48, 24);
    layout->addWidget(title);
    layout->addWidget(m_usernameEdit);
    layout->addWidget(m_passwordEdit);
    layout->addWidget(m_registerBtn);
    layout->addWidget(m_loginBtn);
    layout->addWidget(m_statusLabel);

    connect(m_registerBtn, &QPushButton::clicked, this, &LoginWindow::onRegisterClicked);
    connect(m_loginBtn, &QPushButton::clicked, this, &LoginWindow::onLoginClicked);

    // 账号服务信号 → 状态展示与登录切换
    auto& account = AccountService::instance();
    connect(&account, &AccountService::registerFinished, this,
            [this](int errCode, const QString& errMsg, long long) {
                m_registerBtn->setEnabled(true);
                if (errCode == 0) {
                    m_statusLabel->setText(QStringLiteral("注册成功，请登录"));
                    // 自动验收模式：注册成功即自动登录
                    if (m_autoLoginAfterRegister && m_usernameEdit->text().size() > 0) {
                        onLoginClicked();
                    }
                } else if (m_autoLoginAfterRegister && errCode == 409) {
                    // 已存在：直接登录（自动验收对同一账号可重复执行）
                    onLoginClicked();
                } else {
                    m_statusLabel->setText(QStringLiteral("注册失败(%1): %2").arg(errCode).arg(errMsg));
                }
            });
    connect(&account, &AccountService::loginFinished, this,
            [this](int errCode, const QString& errMsg, long long) {
                if (errCode != 0) {
                    m_loginBtn->setEnabled(true);
                    m_statusLabel->setText(QStringLiteral("登录失败(%1): %2").arg(errCode).arg(errMsg));
                } else {
                    m_statusLabel->setText(QStringLiteral("登录成功，正在建立长连接…"));
                }
            });
    connect(&account, &AccountService::tcpLoginFinished, this,
            [this](int errCode, const QString& errMsg) {
                if (errCode != 0) {
                    m_loginBtn->setEnabled(true);
                    m_statusLabel->setText(QStringLiteral("长连接失败(%1): %2").arg(errCode).arg(errMsg));
                    return;
                }
                emit loginSucceeded();
            });
    connect(&account, &AccountService::kicked, this, [this](const QString& info) {
        m_statusLabel->setText(QStringLiteral("账号在其他设备登录(%1)，已被下线").arg(info));
        show();
        raise();
    });
}

void LoginWindow::onRegisterClicked() {
    m_registerBtn->setEnabled(false);
    m_statusLabel->setText(QStringLiteral("注册中…"));
    AccountService::instance().registerAccount(m_usernameEdit->text().toStdString(),
                                               m_passwordEdit->text().toStdString());
}

void LoginWindow::onLoginClicked() {
    m_loginBtn->setEnabled(false);
    m_statusLabel->setText(QStringLiteral("登录中…"));
    AccountService::instance().login(m_usernameEdit->text().toStdString(),
                                     m_passwordEdit->text().toStdString());
}

void LoginWindow::autoTestLogin(const QString& username, const QString& password) {
    m_autoLoginAfterRegister = true;
    m_usernameEdit->setText(username);
    m_passwordEdit->setText(password);
    onRegisterClicked();
}

} // namespace lingxi::client
