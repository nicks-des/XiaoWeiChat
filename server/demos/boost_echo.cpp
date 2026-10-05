/**
 * @file boost_echo.cpp
 * @brief T00-03 编译链验证：Boost.Asio 异步 echo 服务器 + 内嵌同步客户端回环自检。
 *
 * 运行逻辑：同进程内启动异步 echo 服务（127.0.0.1:19090），另一线程以同步 socket
 * 发送一句问候并等待原样回显；校验一致后打印 PASS 并正常退出（exit 0）。
 */
#include <boost/asio.hpp>

#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include "lingxi/logging/Logger.h"

using boost::asio::ip::tcp;

namespace {

/** 回显服务监听端口（本机回环自检专用） */
constexpr unsigned short kEchoPort = 19090;

/**
 * @brief 异步回显会话：读一段写一段，直至对端关闭。
 */
class EchoSession : public std::enable_shared_from_this<EchoSession> {
public:
    explicit EchoSession(tcp::socket socket) : m_socket(std::move(socket)) {}

    /**
     * @brief 启动会话的读循环。
     */
    void start() { doRead(); }

private:
    /**
     * @brief 异步读取客户端数据。
     */
    void doRead() {
        auto self = shared_from_this();
        m_socket.async_read_some(boost::asio::buffer(m_data),
                                 [this, self](boost::system::error_code ec, std::size_t length) {
                                     if (ec) {
                                         return;  // 对端关闭或出错，结束会话
                                     }
                                     doWrite(length);
                                 });
    }

    /**
     * @brief 原样回写数据后继续读。
     */
    void doWrite(std::size_t length) {
        auto self = shared_from_this();
        boost::asio::async_write(m_socket, boost::asio::buffer(m_data, length),
                                 [this, self](boost::system::error_code ec, std::size_t) {
                                     if (ec) {
                                         return;
                                     }
                                     doRead();
                                 });
    }

    tcp::socket m_socket;      ///< 会话套接字
    char m_data[1024] = {0};   ///< 读缓冲
};

/**
 * @brief 异步 echo 服务器：循环接受连接。
 */
void runEchoServer(boost::asio::io_context& ioContext) {
    tcp::acceptor acceptor(ioContext, {tcp::v4(), kEchoPort});
    auto acceptLoop = [&]() {
        acceptor.async_accept([&](boost::system::error_code ec, tcp::socket socket) {
            if (!ec) {
                std::make_shared<EchoSession>(std::move(socket))->start();
            }
            acceptLoop();  // 继续接受下一个连接
        });
    };
    acceptLoop();
}

} // namespace

/**
 * @brief 主流程：服务线程 + 客户端回环校验。
 * @return int 0 成功；1 校验失败
 */
int main() {
    lingxi::log::init("boost_echo", "info", "logs");

    boost::asio::io_context ioContext;
    runEchoServer(ioContext);

    // 服务跑在独立线程；客户端完成后 stop io_context 收尾
    std::thread serverThread([&ioContext] { ioContext.run(); });

    bool passed = false;
    try {
        tcp::socket socket(ioContext);
        socket.connect({boost::asio::ip::make_address("127.0.0.1"), kEchoPort});
        const std::string greeting = "hello lingxi";
        boost::asio::write(socket, boost::asio::buffer(greeting));

        std::string echoed(greeting.size(), '\0');
        socket.read_some(boost::asio::buffer(echoed.data(), echoed.size()));
        passed = (echoed == greeting);
        std::cout << (passed ? "BOOST_ECHO_PASS: echo ok" : "BOOST_ECHO_FAIL: mismatch")
                  << std::endl;
    } catch (const boost::system::system_error& e) {
        std::cout << "BOOST_ECHO_FAIL: " << e.what() << std::endl;
    }

    ioContext.stop();
    serverThread.join();
    return passed ? 0 : 1;
}
