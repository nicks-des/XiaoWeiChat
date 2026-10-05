/**
 * @file main.cpp
 * @brief GateServer 进程入口：HTTP 监听 8080（docs/01 §6 端口规划）。
 */
#include "GateServer.h"

#include <boost/asio.hpp>

namespace asio = boost::asio;  ///< 项目内简写（见 docs/01：全栈 boost）

#include <iostream>

#include "lingxi/base/ThreadPool.h"
#include "lingxi/config/Config.h"
#include "lingxi/http/HttpServer.h"
#include "lingxi/logging/Logger.h"

/**
 * @brief 进程主流程：配置 → MySQL/Status 装配 → HTTP 路由 → 事件循环。
 */
int main() {
    auto& config = lingxi::Config::instance();
    if (!config.load("config/dev.json") && !config.load("../../config/dev.json")) {
        std::cerr << "[gateserver] config load failed" << std::endl;
        return 1;
    }
    lingxi::log::init("gateserver", config.get<std::string>("log.level", "info"),
                      config.get<std::string>("log.dir", "logs"));

    const unsigned short httpPort =
        static_cast<unsigned short>(config.get<int>("gateserver.httpPort", 8080));
    LX_LOG_INFO("GateServer starting, httpPort={}", httpPort);

    lingxi::ThreadPool handlerPool(4);
    asio::io_context ioContext;

    lingxi::http::HttpServer httpServer(ioContext, httpPort, handlerPool);
    lingxi::GateServer::instance().setup(httpServer);
    httpServer.start();

    ioContext.run();
    return 0;
}
