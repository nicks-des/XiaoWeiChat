/**
 * @file main.cpp
 * @brief FileServer 进程入口：HTTP 8085（docs/01 §6 端口规划）。
 */
#include "FileService.h"

#include <boost/asio.hpp>

#include <iostream>

namespace asio = boost::asio;

#include "lingxi/base/ThreadPool.h"
#include "lingxi/config/Config.h"
#include "lingxi/db/MySqlPool.h"
#include "lingxi/http/HttpServer.h"
#include "lingxi/logging/Logger.h"

/**
 * @brief 进程主流程：配置 → MySQL 池 → 文件路由 → 事件循环。
 */
int main() {
    auto& config = lingxi::Config::instance();
    if (!config.load("config/dev.json") && !config.load("../../config/dev.json")) {
        std::cerr << "[fileserver] config load failed" << std::endl;
        return 1;
    }
    lingxi::log::init("fileserver", config.get<std::string>("log.level", "info"),
                      config.get<std::string>("log.dir", "logs"));

    const unsigned short httpPort =
        static_cast<unsigned short>(config.get<int>("fileserver.httpPort", 8085));
    LX_LOG_INFO("FileServer starting, httpPort={}", httpPort);

    lingxi::ThreadPool handlerPool(4);
    asio::io_context ioContext;

    lingxi::db::MySqlOptions dbOptions;
    dbOptions.host = config.get<std::string>("mysql.host", "127.0.0.1");
    dbOptions.port = config.get<int>("mysql.port", 3316);
    dbOptions.user = config.get<std::string>("mysql.user", "root");
    dbOptions.password = config.get<std::string>("mysql.password", "");
    dbOptions.database = config.get<std::string>("mysql.database", "lingxi");
    auto dbPool = std::make_unique<lingxi::db::MySqlConnectionPool>(dbOptions);
    auto idGen = std::make_unique<lingxi::SnowflakeIdGenerator>(
        config.get<int>("fileserver.machineId", 41));

    lingxi::http::HttpServer server(ioContext, httpPort, handlerPool);
    lingxi::FileService fileService(dbPool.get(),
                                    config.get<std::string>("fileserver.storageDir", "data/files"),
                                    config.get<int>("fileserver.chunkSize", 4 * 1024 * 1024),
                                    idGen.get());
    fileService.setup(server);
    server.start();

    ioContext.run();
    return 0;
}
