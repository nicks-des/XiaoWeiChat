/**
 * @file main.cpp
 * @brief StatusServer 进程入口：RPC 监听 9000（docs/01 §6 端口规划）。
 */
#include "StatusService.h"

#include <boost/asio.hpp>

namespace asio = boost::asio;  ///< 项目内简写（见 docs/01：全栈 boost）

#include <iostream>

#include "lingxi/base/ThreadPool.h"
#include "lingxi/config/Config.h"
#include "lingxi/logging/Logger.h"
#include "lingxi/rpc/RpcServer.h"

/**
 * @brief 进程主流程：加载配置 → 注册处理器 → 驱动事件循环。
 */
int main() {
    auto& config = lingxi::Config::instance();
    if (!config.load("config/dev.json") && !config.load("../../config/dev.json")) {
        std::cerr << "[statusserver] config load failed" << std::endl;
        return 1;
    }
    lingxi::log::init("statusserver", config.get<std::string>("log.level", "info"),
                      config.get<std::string>("log.dir", "logs"));

    const unsigned short rpcPort =
        static_cast<unsigned short>(config.get<int>("statusserver.rpcPort", 9000));
    LX_LOG_INFO("StatusServer starting, rpcPort={}", rpcPort);

    lingxi::ThreadPool handlerPool(4);
    asio::io_context ioContext;

    lingxi::rpc::RpcServer rpcServer(ioContext, rpcPort, handlerPool);
    lingxi::StatusService::instance().registerHandlers(rpcServer);
    rpcServer.start();

    ioContext.run();  // 阻塞驱动（单 IO 线程模型，M1 足够）
    return 0;
}
