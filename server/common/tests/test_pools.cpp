/**
 * @file test_pools.cpp
 * @brief MySQL/Redis 连接池集成测试（依赖本机开发实例；服务不可用时自动跳过）。
 */
#include <gtest/gtest.h>

#include "lingxi/config/Config.h"
#include "lingxi/db/MySqlPool.h"
#include "lingxi/db/RedisPool.h"

namespace {

/**
 * @brief 懒加载开发配置（工作目录为 build/bin，向上一级即仓库根的 config/dev.json）。
 */
void loadDevConfigOnce() {
    static const bool loaded = [] {
        auto& config = lingxi::Config::instance();
        return config.load("../../config/dev.json") || config.load("config/dev.json");
    }();
    (void)loaded;
}

/**
 * @brief 从 config/dev.json 读取 MySQL 连接参数。
 */
lingxi::db::MySqlOptions loadMySqlOptions() {
    loadDevConfigOnce();
    auto& config = lingxi::Config::instance();
    lingxi::db::MySqlOptions options;
    options.host = config.get<std::string>("mysql.host", "127.0.0.1");
    options.port = config.get<int>("mysql.port", 3316);
    options.user = config.get<std::string>("mysql.user", "root");
    options.password = config.get<std::string>("mysql.password", "");
    options.database = config.get<std::string>("mysql.database", "lingxi");
    return options;
}

/**
 * @brief 从 config/dev.json 读取 Redis 连接参数。
 */
lingxi::db::RedisOptions loadRedisOptions() {
    loadDevConfigOnce();
    auto& config = lingxi::Config::instance();
    lingxi::db::RedisOptions options;
    options.host = config.get<std::string>("redis.host", "127.0.0.1");
    options.port = config.get<int>("redis.port", 6379);
    options.password = config.get<std::string>("redis.password", "");
    return options;
}

/**
 * @brief MySQL 健康检查与基本读写（服务不可用则跳过，不算失败）。
 */
TEST(MySqlPoolTest, PingAndBasicQuery) {
    lingxi::db::MySqlConnectionPool pool(loadMySqlOptions());
    if (!pool.ping()) {
        GTEST_SKIP() << "MySQL 开发实例不可用（跳过集成用例）";
    }

    auto conn = pool.acquire();
    ASSERT_NE(conn, nullptr);

    lingxi::db::MySqlResult result;
    ASSERT_TRUE(conn->query("SELECT 40+2 AS answer", result));
    ASSERT_TRUE(result.next());
    EXPECT_EQ(result.getInt64(0), 42);
}

/**
 * @brief Redis SET/GET 回环与 TTL（服务不可用则跳过）。
 */
TEST(RedisPoolTest, SetGetRoundtrip) {
    lingxi::db::RedisConnectionPool pool(loadRedisOptions());
    if (!pool.ping()) {
        GTEST_SKIP() << "Redis 开发实例不可用（跳过集成用例）";
    }

    auto conn = pool.acquire();
    ASSERT_NE(conn, nullptr);

    const std::string key = "lingxi:test:roundtrip";
    EXPECT_TRUE(conn->exec("SET %s %s EX 60", key.c_str(), "hello").ok());

    auto value = conn->exec("GET %s", key.c_str());
    ASSERT_TRUE(value.ok());
    EXPECT_EQ(value.str(), "hello");

    auto ttl = conn->exec("TTL %s", key.c_str());
    ASSERT_TRUE(ttl.ok());
    EXPECT_GT(ttl.integer(), 0);

    conn->exec("DEL %s", key.c_str());
}

/**
 * @brief 池的借还平衡：并发获取归还后空闲数恢复。
 */
TEST(RedisPoolTest, PoolBalance) {
    lingxi::db::RedisOptions options = loadRedisOptions();
    options.poolSize = 2;
    lingxi::db::RedisConnectionPool pool(options);
    if (!pool.ping()) {
        GTEST_SKIP() << "Redis 开发实例不可用（跳过集成用例）";
    }

    {
        auto first = pool.acquire();
        auto second = pool.acquire();
        EXPECT_EQ(pool.idleCount(), 0u);
    }  // 离开作用域自动归还
    EXPECT_EQ(pool.idleCount(), 2u);
}

} // namespace
