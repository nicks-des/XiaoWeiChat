/**
 * @file MySqlPool.cpp
 * @brief MySQL 连接池实现（libmysqlclient C API）。
 */
#include "lingxi/db/MySqlPool.h"

#include <mysql.h>

#include <cstdlib>
#include <vector>

#include "lingxi/logging/Logger.h"

namespace lingxi::db {

/* ==================== MySqlResult ==================== */

MySqlResult::MySqlResult(MYSQL_RES* res, uint64_t rowCount)
    : m_res(res), m_row(nullptr), m_rowCount(rowCount) {}

MySqlResult::~MySqlResult() {
    if (m_res != nullptr) {
        mysql_free_result(m_res);
        m_res = nullptr;
    }
}

MySqlResult::MySqlResult(MySqlResult&& other) noexcept
    : m_res(other.m_res), m_row(other.m_row), m_rowCount(other.m_rowCount) {
    other.m_res = nullptr;
    other.m_rowCount = 0;
}

MySqlResult& MySqlResult::operator=(MySqlResult&& other) noexcept {
    if (this != &other) {
        if (m_res != nullptr) {
            mysql_free_result(m_res);
        }
        m_res = other.m_res;
        m_row = other.m_row;
        m_rowCount = other.m_rowCount;
        other.m_res = nullptr;
        other.m_rowCount = 0;
    }
    return *this;
}

bool MySqlResult::next() {
    if (m_res == nullptr) {
        return false;
    }
    m_row = mysql_fetch_row(m_res);
    return m_row != nullptr;
}

std::string MySqlResult::getString(size_t index) const {
    if (m_row == nullptr || m_row[index] == nullptr) {
        return {};
    }
    // 注意：此拷贝按字节语义返回（utf8mb4 内容原样传递）
    return std::string(m_row[index]);
}

int64_t MySqlResult::getInt64(size_t index) const {
    if (m_row == nullptr || m_row[index] == nullptr) {
        return 0;
    }
    return std::strtoll(m_row[index], nullptr, 10);
}

bool MySqlResult::isNull(size_t index) const {
    return m_row == nullptr || m_row[index] == nullptr;
}

/* ==================== MySqlConnection ==================== */

MySqlConnection::MySqlConnection() : m_mysql(mysql_init(nullptr)) {
    if (m_mysql != nullptr) {
        // 自动重连关闭：连接健康由池层 ping 保证，避免掩盖半开连接问题
        const bool reconnect = false;
        mysql_options(m_mysql, MYSQL_OPT_RECONNECT, &reconnect);
    }
}

MySqlConnection::~MySqlConnection() {
    if (m_mysql != nullptr) {
        mysql_close(m_mysql);
        m_mysql = nullptr;
    }
}

bool MySqlConnection::connect(const MySqlOptions& options) {
    if (m_mysql == nullptr) {
        return false;
    }
    mysql_options(m_mysql, MYSQL_SET_CHARSET_NAME, "utf8mb4");
    const unsigned int timeoutSec = static_cast<unsigned int>(options.connectTimeoutSec);
    mysql_options(m_mysql, MYSQL_OPT_CONNECT_TIMEOUT, &timeoutSec);

    if (mysql_real_connect(m_mysql, options.host.c_str(), options.user.c_str(),
                           options.password.c_str(), options.database.c_str(),
                           static_cast<unsigned int>(options.port), nullptr, 0) == nullptr) {
        return false;
    }
    // 会话级排序规则统一，避免多节点间隐式转换
    return mysql_query(m_mysql, "SET NAMES utf8mb4") == 0;
}

bool MySqlConnection::ping() {
    return m_mysql != nullptr && mysql_ping(m_mysql) == 0;
}

bool MySqlConnection::execute(const std::string& sql) {
    if (m_mysql == nullptr || mysql_query(m_mysql, sql.c_str()) != 0) {
        LX_LOG_ERROR("mysql execute failed: [{}] {}", errorCode(), errorMessage());
        return false;
    }
    return true;
}

bool MySqlConnection::query(const std::string& sql, MySqlResult& out) {
    if (m_mysql == nullptr || mysql_query(m_mysql, sql.c_str()) != 0) {
        LX_LOG_ERROR("mysql query failed: [{}] {} sql={}", errorCode(), errorMessage(), sql);
        return false;
    }
    MYSQL_RES* res = mysql_store_result(m_mysql);
    if (res == nullptr) {
        if (mysql_field_count(m_mysql) != 0) {
            LX_LOG_ERROR("mysql store_result failed: {}", errorMessage());
            return false;
        }
        out = MySqlResult();  // 无结果集（如 SELECT 1 之外的 DML 误入），返回空集
        return true;
    }
    out = MySqlResult(res, mysql_num_rows(res));
    return true;
}

uint64_t MySqlConnection::lastInsertId() const {
    return m_mysql == nullptr ? 0 : mysql_insert_id(m_mysql);
}

std::string MySqlConnection::escapeString(const std::string& input) const {
    if (m_mysql == nullptr) {
        return input;
    }
    std::vector<char> buffer(input.size() * 2 + 1);
    const unsigned long length = mysql_real_escape_string(
        m_mysql, buffer.data(), input.c_str(), static_cast<unsigned long>(input.size()));
    return std::string(buffer.data(), length);
}

unsigned int MySqlConnection::errorCode() const {
    return m_mysql == nullptr ? 0 : mysql_errno(m_mysql);
}

std::string MySqlConnection::errorMessage() const {
    return m_mysql == nullptr ? "null handle" : mysql_error(m_mysql);
}

/* ==================== MySqlConnectionPool ==================== */

MySqlConnectionPool::MySqlConnectionPool(MySqlOptions options) : m_options(std::move(options)) {
    LX_LOG_INFO("MySqlConnectionPool created: {}@{}:{}/{} poolSize={}", m_options.user,
                m_options.host, m_options.port, m_options.database, m_options.poolSize);
}

MySqlConnectionPool::~MySqlConnectionPool() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_stopped = true;
    m_idle.clear();
    m_condition.notify_all();
}

std::unique_ptr<MySqlConnection> MySqlConnectionPool::createConnection() {
    auto conn = std::make_unique<MySqlConnection>();
    if (!conn->connect(m_options)) {
        LX_LOG_ERROR("mysql connect failed: [{}] {}", conn->errorCode(), conn->errorMessage());
        return nullptr;
    }
    return conn;
}

std::shared_ptr<MySqlConnection> MySqlConnectionPool::acquire(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(m_mutex);
    if (!m_condition.wait_for(lock, timeout, [this] {
            return m_stopped || !m_idle.empty() ||
                   m_idle.size() + m_borrowed < static_cast<size_t>(m_options.poolSize);
        })) {
        return nullptr;  // 池满且等待超时
    }
    if (m_stopped) {
        return nullptr;
    }

    std::unique_ptr<MySqlConnection> conn;
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

    // 归还闭包：捕获 this，析构时回池（池需保证生命周期长于归还动作）
    auto* pool = this;
    auto* raw = conn.release();
    return std::shared_ptr<MySqlConnection>(raw, [pool](MySqlConnection* c) {
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

bool MySqlConnectionPool::ping() {
    auto conn = acquire(std::chrono::milliseconds(2000));
    if (conn == nullptr) {
        return false;
    }
    if (!conn->ping()) {
        return false;
    }
    MySqlResult result;
    return conn->query("SELECT 1", result);
}

size_t MySqlConnectionPool::idleCount() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_idle.size();
}

} // namespace lingxi::db
