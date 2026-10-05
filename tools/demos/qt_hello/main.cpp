/**
 * @file main.cpp
 * @brief T00-03 编译链验证：Qt 5.12 Widgets 最小程序，展示标签后 2 秒自动退出。
 *
 * 自动退出设计使该演示可直接用于无人值守验证（CI 友好）。
 */
#include <QApplication>
#include <QLabel>
#include <QTimer>

#include <iostream>

/**
 * @brief 创建标签窗口，2 秒后自动退出。
 * @return int QApplication 退出码
 */
int main(int argc, char* argv[]) {
    QApplication app(argc, argv);

    QLabel label;
    label.setWindowTitle(QString::fromUtf8("灵犀 IM 环境验证"));
    label.setText(QString::fromUtf8(
        "<h2>灵犀 IM</h2><p>Qt %1 · 编译链验证通过</p>").arg(QT_VERSION_STR));
    label.setAlignment(Qt::AlignCenter);
    label.resize(480, 180);
    label.show();

    std::cout << "QT_HELLO_SHOW" << std::endl;
    QTimer::singleShot(2000, &app, &QApplication::quit);
    return app.exec();
}
