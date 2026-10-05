/**
 * @file test_threadpool.cpp
 * @brief 线程池单元测试：结果返回、并发正确性、异常传递。
 */
#include <gtest/gtest.h>

#include <atomic>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "lingxi/base/ThreadPool.h"

namespace {

using lingxi::ThreadPool;

/**
 * @brief 任务按提交执行并返回正确结果。
 */
TEST(ThreadPoolTest, SubmitReturnsResult) {
    ThreadPool pool(2);
    auto future = pool.submit([] { return 21 * 2; });
    EXPECT_EQ(future.get(), 42);
}

/**
 * @brief 并发任务全部完成（压力 + 正确性）。
 */
TEST(ThreadPoolTest, ConcurrentTasksAllComplete) {
    ThreadPool pool(4);
    std::atomic<int> counter{0};
    std::vector<std::future<void>> futures;
    for (int i = 0; i < 1000; ++i) {
        futures.emplace_back(pool.submit([&counter] {
            counter.fetch_add(1, std::memory_order_relaxed);
        }));
    }
    for (auto& future : futures) {
        future.get();
    }
    EXPECT_EQ(counter.load(), 1000);
}

/**
 * @brief 任务抛出的异常通过 future 传递给调用方。
 */
TEST(ThreadPoolTest, ExceptionPropagates) {
    ThreadPool pool(1);
    auto future = pool.submit([]() -> int { throw std::runtime_error("boom"); });
    EXPECT_THROW(future.get(), std::runtime_error);
}

} // namespace
