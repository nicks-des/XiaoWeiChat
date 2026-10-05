/**
 * @file main.cpp
 * @brief ChatServer 进程入口：客户端 TCP 8888 + RPC 9001（docs/01 §6 端口规划）。
 */
#include "ChatServer.h"

#include <boost/asio.hpp>

namespace asio = boost::asio;  ///< 项目内简写（见 docs/01：全栈 boost）

#include <iostream>

#include "lingxi/base/ThreadPool.h"
#include "lingxi/config/Config.h"
#include "lingxi/logging/Logger.h"

/**
 * @brief 进程主流程：配置 → 组件装配 → 注册 Status → 事件循环。
 */
int main() {
    auto& config = lingxi::Config::instance();
    if (!config.load("config/dev.json") && !config.load("../../config/dev.json")) {
        std::cerr << "[chatserver] config load failed" << std::endl;
        return 1;
    }
    lingxi::log::init("chatserver", config.get<std::string>("log.level", "info"),
                      config.get<std::string>("log.dir", "logs"));
    LX_LOG_INFO("ChatServer starting, clientPort={} rpcPort={}",
                config.get<int>("chatserver.clientPort", 8888),
                config.get<int>("chatserver.rpcPort", 9001));

    lingxi::ThreadPool handlerPool(4);
    asio::io_context ioContext;

    lingxi::ChatServer server(ioContext, handlerPool);
    server.start();

    ioContext.run();
    return 0;
}
