/**
 * @file test_config.cpp
 * @brief 配置模块单元测试：加载、点路径取值、local 深合并覆盖。
 */
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "lingxi/config/Config.h"

namespace {

/**
 * @brief 写一个临时 JSON 文件供测试加载。
 */
std::string writeTempJson(const std::string& content) {
    const std::string path =
        (std::filesystem::temp_directory_path() / "lingxi_config_test.json").string();
    std::ofstream file(path);
    file << content;
    return path;
}

/**
 * @brief 主配置加载与点路径取值（含默认值回退）。
 */
TEST(ConfigTest, LoadAndGetWithDefaults) {
    const std::string path = writeTempJson(R"({
        "mysql": {"host": "10.0.0.1", "port": 3316},
        "log": {"level": "debug"}
    })");

    lingxi::Config config;
    ASSERT_TRUE(config.load(path));

    EXPECT_EQ(config.get<std::string>("mysql.host", ""), "10.0.0.1");
    EXPECT_EQ(config.get<int>("mysql.port", 3306), 3316);
    EXPECT_EQ(config.get<std::string>("log.level", "info"), "debug");
    // 缺失键回退默认值
    EXPECT_EQ(config.get<int>("mysql.poolSize", 8), 8);
    EXPECT_EQ(config.get<std::string>("redis.host", "127.0.0.1"), "127.0.0.1");
    EXPECT_FALSE(config.contains("not.exist.key"));
}

/**
 * @brief local 覆盖文件深合并：对象递归合并，标量直接覆盖。
 */
TEST(ConfigTest, OverrideDeepMerge) {
    const std::string base = writeTempJson(R"({
        "mysql": {"host": "10.0.0.1", "port": 3316, "poolSize": 4},
        "log": {"level": "info"}
    })");
    const std::string local = writeTempJson(R"({
        "mysql": {"poolSize": 16},
        "log": {"dir": "/var/log"}
    })");

    lingxi::Config config;
    ASSERT_TRUE(config.load(base));
    ASSERT_TRUE(config.loadOverride(local));

    EXPECT_EQ(config.get<int>("mysql.poolSize", 0), 16);   // 被覆盖
    EXPECT_EQ(config.get<std::string>("mysql.host", ""), "10.0.0.1");  // 保留
    EXPECT_EQ(config.get<int>("mysql.port", 0), 3316);     // 保留
    EXPECT_EQ(config.get<std::string>("log.dir", ""), "/var/log");     // 新增
}

/**
 * @brief 非法 JSON 拒绝加载。
 */
TEST(ConfigTest, InvalidJsonRejected) {
    const std::string path = writeTempJson("{ not valid json !!!");

    lingxi::Config config;
    EXPECT_FALSE(config.load(path));
}

/**
 * @brief 不存在的文件返回 false 且不崩溃。
 */
TEST(ConfigTest, MissingFileHandled) {
    lingxi::Config config;
    EXPECT_FALSE(config.load("Z:/definitely/not/exists.json"));
    EXPECT_FALSE(config.loadOverride("Z:/definitely/not/exists.local.json"));
}

} // namespace
