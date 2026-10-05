/**
 * @file main.cpp
 * @brief 灵犀 IM 客户端入口：登录窗口 ↔ 主面板切换；--autotest 为无人值守验收模式。
 *
 * 自动验收：./lingxi_client.exe --autotest
 * 自动生成账号 → 注册 → 登录 → TCP 长连接校验 → 切主面板，输出 CLIENT_AUTO_LOGIN_PASS。
 */
#include <QApplication>
#include <QTimer>

#include <iostream>
#include <random>

#include "client/service/AccountService.h"
#include "client/ui/LoginWindow.h"
#include "client/ui/MainPanel.h"
#include "lingxi/logging/Logger.h"

namespace {

/**
 * @brief 生成自动验收用的随机用户名。
 */
std::string generateTestUsername() {
    static std::mt19937 generator{std::random_device{}()};
    return "auto" + std::to_string(generator() % 100000000);
}

} // namespace

/**
 * @brief 进程入口。
 * @return int 0 正常；自动验收失败返回 1
 */
int main(int argc, char* argv[]) {
    const bool autoTest = argc > 1 && std::string(argv[1]) == "--autotest";

    QApplication app(argc, argv);
    lingxi::log::init("client", "info", "logs");

    if (!lingxi::client::AccountService::instance().setup()) {
        std::cerr << "[client] config load failed" << std::endl;
        return 1;
    }

    lingxi::client::LoginWindow loginWindow;
    lingxi::client::MainPanel mainPanel;
    mainPanel.hide();

    // 登录链路完成 → 切主面板
    QObject::connect(&lingxi::client::AccountService::instance(),
                     &lingxi::client::AccountService::tcpLoginFinished, &mainPanel,
                     [&](int errCode, const QString&) {
                         if (errCode == 0) {
                             loginWindow.hide();
                             mainPanel.setUserInfo(
                                 lingxi::client::AccountService::instance().uid(),
                                 QStringLiteral("灵犀用户"));
                             mainPanel.show();
                         }
                     });

    if (autoTest) {
        const std::string username = generateTestUsername();
        std::cout << "[autotest] username = " << username << std::endl;

        // 30s 全局兜底：任一环节卡死则判失败
        QTimer::singleShot(30000, &app, [&app] {
            std::cout << "CLIENT_AUTO_LOGIN_FAIL: timeout" << std::endl;
            app.exit(1);
        });

        // 自动填充并触发登录（注册成功后自动登录）
        QTimer::singleShot(500, &app, [&loginWindow, &app, username] {
            loginWindow.autoTestLogin(QString::fromStdString(username),
                                      QStringLiteral("Passw0rd!123"));
        });
        QObject::connect(&lingxi::client::AccountService::instance(),
                         &lingxi::client::AccountService::tcpLoginFinished, &app,
                         [&](int errCode, const QString&) {
                             if (errCode != 0) {
                                 return;
                             }
                             QTimer::singleShot(300, &app, [&app] {
                                 std::cout << "CLIENT_AUTO_LOGIN_PASS" << std::endl;
                                 app.exit(0);
                             });
                         });
    } else {
        loginWindow.show();
    }

    return app.exec();
}
