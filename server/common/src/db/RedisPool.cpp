/**
 * @file RedisPool.cpp
 * @brief Redis 连接池实现（hiredis）。
 */
#include "lingxi/db/RedisPool.h"

#include "lingxi/logging/Logger.h"

namespace lingxi::db {

/* ==================== RedisConnection ==================== */

RedisConnection::~RedisConnection() {
    if (m_context != nullptr) {
        redisFree(m_context);
        m_context = nullptr;
    }
}

bool RedisConnection::connect(const RedisOptions& options) {
    if (m_context != nullptr) {
        redisFree(m_context);
        m_context = nullptr;
    }

    timeval connectTimeout{options.connectTimeoutMs / 1000,
                           (options.connectTimeoutMs % 1000) * 1000};
    m_context = redisConnectWithTimeout(options.host.c_str(), options.port, connectTimeout);
    if (m_context == nullptr || m_context->err) {
        if (m_context != nullptr) {
            LX_LOG_ERROR("redis connect failed: {} ({}:{})", m_context->errstr, options.host,
                         options.port);
            redisFree(m_context);
            m_context = nullptr;
        }
        return false;
    }

    timeval commandTimeout{options.commandTimeoutMs / 1000,
                           (options.commandTimeoutMs % 1000) * 1000};
    redisSetTimeout(m_context, commandTimeout);

    if (!options.password.empty()) {
        RedisReply reply = exec("AUTH %s", options.password.c_str());
        if (!reply.ok()) {
            LX_LOG_ERROR("redis auth failed: {}", options.host);
            redisFree(m_context);
            m_context = nullptr;
            return false;
        }
    }
    return true;
}

bool RedisConnection::ping() {
    if (m_context == nullptr) {
        return false;
    }
    RedisReply reply = exec("PING");
    return reply.ok() && reply.str() == "PONG";
}

/* ==================== RedisConnectionPool ==================== */

RedisConnectionPool::RedisConnectionPool(RedisOptions options) : m_options(std::move(options)) {
    LX_LOG_INFO("RedisConnectionPool created: {}:{} poolSize={}", m_options.host, m_options.port,
                m_options.poolSize);
}

RedisConnectionPool::~RedisConnectionPool() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_stopped = true;
    m_idle.clear();
    m_condition.notify_all();
}

std::unique_ptr<RedisConnection> RedisConnectionPool::createConnection() {
    auto conn = std::make_unique<RedisConnection>();
    if (!conn->connect(m_options)) {
        return nullptr;
    }
    return conn;
}

std::shared_ptr<RedisConnection> RedisConnectionPool::acquire(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(m_mutex);
    if (!m_condition.wait_for(lock, timeout, [this] {
            return m_stopped || !m_idle.empty() ||
                   m_idle.size() + m_borrowed < static_cast<size_t>(m_options.poolSize);
        })) {
        return nullptr;
    }
    if (m_stopped) {
        return nullptr;
    }

    std::unique_ptr<RedisConnection> conn;
    if (!m_idle.empty()) {
        conn = std::move(m_idle.back());
        m_idle.pop_back();
        ++m_borrowed;
    } else {
        // 预占名额后解锁建连：防止并发建连超发突破池上限
        ++m_borrowed;
        lock.unlock();
        conn = createConnection();
        lock.lock();
        if (conn == nullptr) {
            --m_borrowed;
            m_condition.notify_one();
            return nullptr;
        }
    }

    auto* pool = this;
    auto* raw = conn.release();
    return std::shared_ptr<RedisConnection>(raw, [pool](RedisConnection* c) {
        std::lock_guard<std::mutex> guard(pool->m_mutex);
        if (pool->m_stopped) {
            delete c;
        } else {
            pool->m_idle.emplace_back(c);
            --pool->m_borrowed;
            pool->m_condition.notify_one();
        }
    });
}

bool RedisConnectionPool::ping() {
    auto conn = acquire(std::chrono::milliseconds(2000));
    return conn != nullptr && conn->ping();
}

size_t RedisConnectionPool::idleCount() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_idle.size();
}

} // namespace lingxi::db
