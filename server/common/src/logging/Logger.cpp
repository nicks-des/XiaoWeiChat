/**
 * @file Logger.cpp
 * @brief 日志模块实现：rotating 文件 + 控制台双 sink。
 */
#include "lingxi/logging/Logger.h"

#include <spdlog/async.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <filesystem>
#include <mutex>

namespace lingxi::log {

namespace {

/** 全局懒初始化保护锁 */
std::once_flag g_initOnce;

/**
 * @brief 实际执行初始化（确保仅执行一次）。
 */
void initOnce(const std::string& loggerName, const std::string& level, const std::string& dir) {
    std::filesystem::create_directories(dir);

    // 控制台彩色输出 + 5MB×5 个滚动文件，兼顾开发体验与磁盘占用
    auto consoleSink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    auto fileSink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
        dir + "/" + loggerName + ".log", 5 * 1024 * 1024, 5);

    auto logger = std::make_shared<spdlog::logger>(loggerName,
                                                   spdlog::sinks_init_list{consoleSink, fileSink});
    // [%s:%#] 源文件与行号仅在 SPDLOG_ACTIVE_LEVEL 启用时填充
    logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e][%l][%t][%s:%#] %v");
    logger->set_level(spdlog::level::from_str(level));
    logger->flush_on(spdlog::level::warn);  // warn 及以上立即落盘
    spdlog::set_default_logger(logger);
    spdlog::set_automatic_registration(false);
}

} // namespace

void init(const std::string& loggerName, const std::string& level, const std::string& dir) {
    std::call_once(g_initOnce, [&] { initOnce(loggerName, level, dir); });
}

std::shared_ptr<spdlog::logger> get() {
    // 默认日志器由 spdlog 注册表持有；未 init 时兜底初始化，保证任何入口可用
    auto defaultLogger = spdlog::default_logger();
    if (defaultLogger->name().empty()) {
        std::call_once(g_initOnce, [] { initOnce("lingxi", "info", "logs"); });
        defaultLogger = spdlog::default_logger();
    }
    return defaultLogger;
}

} // namespace lingxi::log
