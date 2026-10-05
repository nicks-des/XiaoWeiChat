/**
 * @file ThreadPool.h
 * @brief 轻量固定线程池：任务队列 + future 返回值，析构时排空任务（仅头文件实现）。
 */
#pragma once

#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

#include "lingxi/base/NonCopyable.h"

namespace lingxi {

/**
 * @brief 固定大小线程池。
 *
 * 线程模型：N 个工作线程共享一个任务队列；submit() 返回 std::future 供调用方取结果；
 * 析构时先唤醒全部线程，排空剩余任务后退出（graceful shutdown）。
 */
class ThreadPool : private NonCopyable {
public:
    /**
     * @param threadCount 工作线程数（<=0 时取硬件并发数）
     */
    explicit ThreadPool(int threadCount = 0) {
        if (threadCount <= 0) {
            threadCount = static_cast<int>(std::thread::hardware_concurrency());
            if (threadCount <= 0) {
                threadCount = 4;
            }
        }
        for (int i = 0; i < threadCount; ++i) {
            m_workers.emplace_back([this] { workerLoop(); });
        }
    }

    ~ThreadPool() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stopped = true;
        }
        m_condition.notify_all();
        for (auto& worker : m_workers) {
            worker.join();
        }
    }

    /**
     * @brief 提交任务并获取 future。
     * @tparam F 可调用对象类型
     * @param task 任务及其参数（std::bind 或 lambda）
     * @return std::future<任务返回值类型>
     * @throws std::runtime_error 线程池已停止
     */
    template <typename F>
    auto submit(F&& task) -> std::future<std::invoke_result_t<F>> {
        using ResultType = std::invoke_result_t<F>;

        auto packaged = std::make_shared<std::packaged_task<ResultType()>>(
            std::forward<F>(task));
        std::future<ResultType> result = packaged->get_future();
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_stopped) {
                throw std::runtime_error("submit on stopped ThreadPool");
            }
            m_tasks.emplace([packaged] { (*packaged)(); });
        }
        m_condition.notify_one();
        return result;
    }

    /**
     * @brief 当前排队中的任务数（含正在执行的任务）。
     */
    size_t pendingCount() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_tasks.size();
    }

private:
    /**
     * @brief 工作线程主循环：取任务执行，队列空则等待，stop 后排空退出。
     */
    void workerLoop() {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_condition.wait(lock, [this] { return m_stopped || !m_tasks.empty(); });
                if (m_stopped && m_tasks.empty()) {
                    return;
                }
                task = std::move(m_tasks.front());
                m_tasks.pop();
            }
            task();
        }
    }

    std::vector<std::thread> m_workers;          ///< 工作线程
    std::queue<std::function<void()>> m_tasks;   ///< 任务队列
    mutable std::mutex m_mutex;                  ///< 保护队列与停止标志
    std::condition_variable m_condition;         ///< 任务/停止通知
    bool m_stopped = false;                      ///< 停止标志
};

} // namespace lingxi
