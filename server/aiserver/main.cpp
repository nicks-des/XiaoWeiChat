/**
 * @file main.cpp
 * @brief AIServer 进程入口：RPC 9003（docs/01 §6 端口规划）。
 */
#include "TavernService.h"
#include "StoryService.h"

#include "lingxi/config/Config.h"
#include "lingxi/db/MySqlPool.h"
#include "lingxi/http/HttpServer.h"
#include "lingxi/logging/Logger.h"
#include "lingxi/rpc/RpcClient.h"
#include "lingxi/rpc/RpcServer.h"

#include <boost/asio.hpp>

#include <iostream>

namespace asio = boost::asio;

/**
 * @brief 进程主流程：配置 → MySQL → 酒馆服务 → RPC 监听。
 */
int main() {
    auto& config = lingxi::Config::instance();
    if (!config.load("config/dev.json") && !config.load("../../config/dev.json")) {
        std::cerr << "[aiserver] config load failed" << std::endl;
        return 1;
    }
    lingxi::log::init("aiserver", config.get<std::string>("log.level", "info"),
                      config.get<std::string>("log.dir", "logs"));
    const unsigned short rpcPort =
        static_cast<unsigned short>(config.get<int>("aiserver.rpcPort", 9003));
    LX_LOG_INFO("AIServer starting, rpcPort={}", rpcPort);

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
        config.get<int>("aiserver.machineId", 51));

    lingxi::TavernService tavern(dbPool.get(), idGen.get(), handlerPool);

    // 推送通道：指向 ChatServer（流式帧经其 PushToUid 下发）
    const std::string chatHost = config.get<std::string>("chatserver.host", "127.0.0.1");
    const int chatRpcPort = config.get<int>("chatserver.rpcPort", 9001);
    auto pushChannel = std::make_unique<lingxi::rpc::RpcClientPool>(
        chatHost, static_cast<unsigned short>(chatRpcPort), 2);
    tavern.setPushChannel(pushChannel.get(), 1);

    lingxi::StoryService story(dbPool.get(), idGen.get(), handlerPool, tavern.llm());

    lingxi::rpc::RpcServer rpcServer(ioContext, rpcPort, handlerPool);
    tavern.registerHandlers(rpcServer);
    story.registerHandlers(rpcServer);
    tavern.setPushChannel(pushChannel.get(), 1);
    story.setPushChannel(pushChannel.get());
    rpcServer.start();

    ioContext.run();
    return 0;
}
