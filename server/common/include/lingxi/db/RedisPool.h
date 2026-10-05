/**
 * @file RedisPool.h
 * @brief Redis 连接池：hiredis 封装（RAII 连接/回复 + 惰性建连 + PING 健康检查）。
 */
#pragma once

#include <winsock2.h>  ///< 必须先于 hiredis 引入，提供 timeval（WIN32_LEAN_AND_MEAN 下不冲突）

#include <hiredis/hiredis.h>

#include <chrono>
#include <cstdarg>
#include <deque>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <string>

#include "lingxi/base/NonCopyable.h"

namespace lingxi::db {

/**
 * @brief Redis 连接配置。
 */
struct RedisOptions {
    std::string host = "127.0.0.1";  ///< 主机
    int port = 6379;                 ///< 端口
    std::string password;            ///< 密码（空表示无认证）
    int poolSize = 8;                ///< 池大小
    int connectTimeoutMs = 2000;     ///< 建连超时（毫秒）
    int commandTimeoutMs = 3000;     ///< 命令超时（毫秒）
};

/**
 * @brief redisReply 的 RAII 封装（析构自动 freeReplyObject）。
 */
class RedisReply {
public:
    explicit RedisReply(redisReply* reply = nullptr) : m_reply(reply) {}
    ~RedisReply() {
        if (m_reply != nullptr) {
            freeReplyObject(m_reply);
        }
    }
    RedisReply(const RedisReply&) = delete;
    RedisReply& operator=(const RedisReply&) = delete;
    RedisReply(RedisReply&& other) noexcept : m_reply(other.m_reply) { other.m_reply = nullptr; }
    RedisReply& operator=(RedisReply&& other) noexcept {
        if (this != &other) {
            if (m_reply != nullptr) {
                freeReplyObject(m_reply);
            }
            m_reply = other.m_reply;
            other.m_reply = nullptr;
        }
        return *this;
    }

    /**
     * @brief 回复是否有效且非错误。
     */
    bool ok() const { return m_reply != nullptr && m_reply->type != REDIS_REPLY_ERROR; }

    /**
     * @brief 取字符串类回复内容（兼容 STRING 与 STATUS 类型，如 PING→PONG；其余返回空串）。
     */
    std::string str() const {
        if (m_reply == nullptr ||
            (m_reply->type != REDIS_REPLY_STRING && m_reply->type != REDIS_REPLY_STATUS &&
             m_reply->type != REDIS_REPLY_ERROR)) {
            return {};
        }
        return std::string(m_reply->str, m_reply->len);
    }

    /**
     * @brief 取整数回复（NIL 返回 -1 便于区分 0 值）。
     */
    long long integer() const {
        return (m_reply != nullptr && m_reply->type == REDIS_REPLY_INTEGER) ? m_reply->integer : -1;
    }

    /**
     * @brief 是否为 NIL 回复。
     */
    bool isNil() const { return m_reply != nullptr && m_reply->type == REDIS_REPLY_NIL; }

    /**
     * @brief 取底层回复指针（只读使用）。
     */
    redisReply* raw() const { return m_reply; }

private:
    redisReply* m_reply;  ///< 所有的 reply 指针（析构释放）
};

/**
 * @brief 单条 Redis 连接（非线程安全：由池保证独占使用）。
 */
class RedisConnection {
public:
    RedisConnection() = default;
    ~RedisConnection();

    RedisConnection(const RedisConnection&) = delete;
    RedisConnection& operator=(const RedisConnection&) = delete;

    /**
     * @brief 建立连接（含超时与可选 AUTH）。
     * @return bool 成功为 true
     */
    bool connect(const RedisOptions& options);

    /**
     * @brief 连接健康检查。
     */
    bool ping();

    /**
     * @brief 执行命令（hiredis 格式化语义，如 "GET %s"）。
     * @param format hiredis 命令格式串
     * @return RedisReply 回复封装（失败时 ok() 为 false）
     */
    template <typename... Args>
    RedisReply exec(const char* format, Args&&... args) {
        if (m_context == nullptr) {
            return RedisReply(nullptr);
        }
        redisReply* reply = reinterpret_cast<redisReply*>(redisCommand(
            m_context, format, std::forward<Args>(args)...));
        return RedisReply(reply);
    }

private:
    redisContext* m_context = nullptr;  ///< hiredis 上下文
};

/**
 * @brief Redis 连接池（互斥队列 + 条件变量；acquire 超时返回空指针）。
 */
class RedisConnectionPool : private NonCopyable {
public:
    explicit RedisConnectionPool(RedisOptions options);
    ~RedisConnectionPool();

    /**
     * @brief 获取一条可用连接（shared_ptr 析构时自动归还池）。
     * @param timeout 等待上限
     * @return std::shared_ptr<RedisConnection> 空指针表示等待超时
     */
    std::shared_ptr<RedisConnection> acquire(
        std::chrono::milliseconds timeout = std::chrono::milliseconds(3000));

    /**
     * @brief 池级健康检查：取一条连接执行 PING。
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
    std::unique_ptr<RedisConnection> createConnection();

    RedisOptions m_options;                              ///< 连接配置
    std::deque<std::unique_ptr<RedisConnection>> m_idle; ///< 空闲连接队列（独占所有权）
    size_t m_borrowed = 0;                               ///< 借出连接数
    mutable std::mutex m_mutex;                          ///< 队列互斥锁
    std::condition_variable m_condition;                 ///< 归还通知
    bool m_stopped = false;                              ///< 池停止标志
};

} // namespace lingxi::db
