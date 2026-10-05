/**
 * @file test_snowflake.cpp
 * @brief 雪花 ID 生成器单元测试：唯一性、趋势递增、机器号合法性。
 */
#include <gtest/gtest.h>

#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

#include "lingxi/base/SnowflakeIdGenerator.h"

namespace {

/**
 * @brief 单线程 10 万个 ID 全局唯一且非降。
 */
TEST(SnowflakeTest, UniqueAndMonotonic) {
    lingxi::SnowflakeIdGenerator generator(1);

    std::set<int64_t> ids;
    int64_t previous = -1;
    for (int i = 0; i < 100000; ++i) {
        const int64_t id = generator.nextId();
        EXPECT_TRUE(ids.insert(id).second) << "重复 ID: " << id;
        EXPECT_GE(id, previous) << "ID 发生回退";
        previous = id;
    }
}

/**
 * @brief 多线程并发生成无重复。
 */
TEST(SnowflakeTest, MultithreadedUnique) {
    lingxi::SnowflakeIdGenerator generator(2);
    std::vector<int64_t> collected(4 * 10000);
    std::vector<std::thread> threads;

    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&generator, &collected, t] {
            for (int i = 0; i < 10000; ++i) {
                collected[static_cast<size_t>(t) * 10000 + i] = generator.nextId();
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    std::set<int64_t> ids(collected.begin(), collected.end());
    EXPECT_EQ(ids.size(), collected.size());
}

/**
 * @brief 非法机器号在构造期直接拒绝。
 */
TEST(SnowflakeTest, InvalidMachineIdRejected) {
    EXPECT_THROW(lingxi::SnowflakeIdGenerator(1024), std::invalid_argument);
    EXPECT_THROW(lingxi::SnowflakeIdGenerator(-1), std::invalid_argument);
}

} // namespace
