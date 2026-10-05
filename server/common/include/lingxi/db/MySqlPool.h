/**
 * @file MySqlPool.h
 * @brief MySQL 连接池：libmysqlclient C API 封装（RAII 连接 + 惰性建连 + 健康检查）。
 *
 * 决策说明（ADR-010 修订）：本机 Connector C++ 8.3 发行版缺失 release 库，
 * 故按 ADR 备选方案改用 MySQL Server 自带的 libmysqlclient C API（更稳、运行时仅一个 DLL）。
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <string>

#include "lingxi/base/NonCopyable.h"

struct MYSQL;       ///< libmysqlclient 前向声明（避免在公共头暴露 C API）
struct MYSQL_RES;
typedef char** MYSQL_ROW;  ///< 行指针（与 libmysqlclient 中定义一致，可重复声明）

namespace lingxi::db {

/**
 * @brief MySQL 连接配置。
 */
struct MySqlOptions {
    std::string host = "127.0.0.1";  ///< 主机
    int port = 3306;                 ///< 端口（本项目开发库为 3316，见 config/dev.json）
    std::string user = "root";       ///< 用户名
    std::string password;            ///< 密码
    std::string database = "lingxi"; ///< 默认库
    int poolSize = 8;                ///< 池大小（最大连接数）
    int connectTimeoutSec = 5;       ///< 建连超时（秒）
};

/**
 * @brief 查询结果集轻封装（RAII 释放 MYSQL_RES）。
 *
 * 仅支持 mysql_store_result 一次性取回的模式（IM 消息量级下最简单可靠）。
 */
class MySqlResult {
public:
    MySqlResult() = default;
    MySqlResult(MYSQL_RES* res, uint64_t rowCount);  ///< 接管所有权
    ~MySqlResult();
    MySqlResult(const MySqlResult&) = delete;
    MySqlResult& operator=(const MySqlResult&) = delete;
    MySqlResult(MySqlResult&& other) noexcept;
    MySqlResult& operator=(MySqlResult&& other) noexcept;

    /**
     * @brief 游标推进到下一行。
     * @return bool 还有行为 true（首行需先调用一次）
     */
    bool next();

    /**
     * @brief 取当前行指定下标的字符串值（下标从 0 开始）。
     * @param index 列下标
     * @return std::string 列值（NULL 列返回空串）
     */
    std::string getString(size_t index) const;

    /**
     * @brief 取当前行指定下标的整数值。
     * @param index 列下标
     * @return int64_t 列值（NULL 列返回 0）
     */
    int64_t getInt64(size_t index) const;

    /**
     * @brief 判断当前行指定列是否为 SQL NULL。
     * @param index 列下标
     */
    bool isNull(size_t index) const;

    /**
     * @brief 结果集行数。
     */
    uint64_t rowCount() const { return m_rowCount; }

private:
    MYSQL_RES* m_res = nullptr;   ///< C API 结果集句柄
    MYSQL_ROW m_row = nullptr;    ///< 当前行
    uint64_t m_rowCount = 0;      ///< 行数
};

/**
 * @brief 单条 MySQL 连接（非线程安全：由池保证独占使用）。
 */
class MySqlConnection {
public:
    MySqlConnection();
    ~MySqlConnection();
    MySqlConnection(const MySqlConnection&) = delete;
    MySqlConnection& operator=(const MySqlConnection&) = delete;

    /**
     * @brief 建立连接并设置 utf8mb4 字符集。
     * @return bool 成功为 true
     */
    bool connect(const MySqlOptions& options);

    /**
     * @brief 连接健康检查（失败时内部自动重连一次）。
     */
    bool ping();

    /**
     * @brief 执行写语句（INSERT/UPDATE/DELETE/DDL），不返回结果集。
     * @param sql SQL 文本（参数须由调用方转义/预编译，禁止拼接用户输入）
     * @return bool 成功为 true
     */
    bool execute(const std::string& sql);

    /**
     * @brief 执行查询并取回完整结果集。
     * @param sql SQL 文本
     * @param out 结果集（成功时有效）
     * @return bool 成功为 true
     */
    bool query(const std::string& sql, MySqlResult& out);

    /**
     * @brief 最近一次 INSERT 的自增/赋值 ID。
     */
    uint64_t lastInsertId() const;

    /**
     * @brief 最近一次错误的错误码（0 表示无错误）。
     */
    unsigned int errorCode() const;

    /**
     * @brief 最近一次错误的描述。
     */
    std::string errorMessage() const;

private:
    MYSQL* m_mysql = nullptr;  ///< C API 连接句柄
};

/**
 * @brief MySQL 连接池（互斥队列 + 条件变量；acquire 超时返回空指针）。
 */
class MySqlConnectionPool : private NonCopyable {
public:
    explicit MySqlConnectionPool(MySqlOptions options);
    ~MySqlConnectionPool();

    /**
     * @brief 获取一条可用连接（shared_ptr 析构时自动归还池）。
     * @param timeout 等待上限
     * @return std::shared_ptr<MySqlConnection> 空指针表示等待超时
     */
    std::shared_ptr<MySqlConnection> acquire(
        std::chrono::milliseconds timeout = std::chrono::milliseconds(3000));

    /**
     * @brief 池级健康检查：取一条连接执行 SELECT 1。
     */
    bool ping();

    /**
     * @brief 当前空闲连接数。
     */
    size_t idleCount() const;

private:
    /**
     * @brief 新建一条连接（带日志）。
     */
    std::unique_ptr<MySqlConnection> createConnection();

    MySqlOptions m_options;                              ///< 连接配置
    std::deque<std::unique_ptr<MySqlConnection>> m_idle; ///< 空闲连接队列（独占所有权）
    size_t m_borrowed = 0;                               ///< 借出连接数
    mutable std::mutex m_mutex;                          ///< 队列互斥锁
    std::condition_variable m_condition;                 ///< 归还通知
    bool m_stopped = false;                              ///< 池停止标志
};

} // namespace lingxi::db
