/**
 * @file Logger.h
 * @brief 日志模块：spdlog 封装，控制台 + 滚动文件双输出，全局 default logger。
 *
 * 用法：LX_LOG_INFO("connect to {}:{}", host, port);
 */
#pragma once

#include <string>

#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE  ///< 编译期保留全级别调用，运行期按级别过滤
#include <spdlog/spdlog.h>

namespace lingxi::log {

/**
 * @brief 初始化全局日志（进程内调用一次；重复调用忽略）。
 * @param loggerName 日志器名（也是文件名前缀）
 * @param level      日志级别字符串：trace/debug/info/warn/error/critical
 * @param dir        日志目录（不存在会自动创建）
 */
void init(const std::string& loggerName = "lingxi",
          const std::string& level = "info",
          const std::string& dir = "logs");

/**
 * @brief 获取全局日志器（未显式 init 时按默认参数懒初始化）。
 * @return std::shared_ptr<spdlog::logger> 全局日志器
 */
std::shared_ptr<spdlog::logger> get();

} // namespace lingxi::log

/* ---- 统一日志宏（带源文件与行号） ---- */
#define LX_LOG_TRACE(...) SPDLOG_LOGGER_TRACE(lingxi::log::get(), __VA_ARGS__)
#define LX_LOG_DEBUG(...) SPDLOG_LOGGER_DEBUG(lingxi::log::get(), __VA_ARGS__)
#define LX_LOG_INFO(...) SPDLOG_LOGGER_INFO(lingxi::log::get(), __VA_ARGS__)
#define LX_LOG_WARN(...) SPDLOG_LOGGER_WARN(lingxi::log::get(), __VA_ARGS__)
#define LX_LOG_ERROR(...) SPDLOG_LOGGER_ERROR(lingxi::log::get(), __VA_ARGS__)
#define LX_LOG_CRITICAL(...) SPDLOG_LOGGER_CRITICAL(lingxi::log::get(), __VA_ARGS__)
