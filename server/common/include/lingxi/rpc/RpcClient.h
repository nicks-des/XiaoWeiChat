/**
 * @file RpcClient.h
 * @brief 自研 RPC 客户端池：目标服务一条 IO 线程 + N 条连接，同步 call 带超时。
 *
 * 用法：
 * @code
 *   lingxi::rpc::RpcClientPool pool("127.0.0.1", 9000, 2);
 *   auto rsp = pool.call(rpc::kServiceStatus, 0x04, reqBytes);  // 失败抛 RpcError
 * @endcode
 */
#pragma once

#include <boost/asio.hpp>
#include <boost/asio/strand.hpp>

namespace asio = boost::asio;  ///< 项目内简写（见 docs/01：全栈 boost）
using asio::ip::tcp;             ///< TCP 类型简写

#include <atomic>
#include <condition_variable>
#include <deque>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

#include "lingxi/base/NonCopyable.h"
#include "lingxi/net/BufReader.h"
#include "lingxi/net/Packet.h"

namespace lingxi::rpc {

/**
 * @brief RPC 调用异常（超时/连接失败/服务端异常）。
 */
struct RpcError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/**
 * @brief RPC 客户端连接池（指向单一目标地址）。
 */
class RpcClientPool : private NonCopyable {
public:
    /**
     * @param host 目标 RPC 地址
     * @param port 目标 RPC 端口
     * @param poolSize 连接池大小
     */
    RpcClientPool(std::string host, unsigned short port, int poolSize = 2);
    ~RpcClientPool();

    /**
     * @brief 同步调用（阻塞当前线程，内部 IO 在独立线程）。
     * @param serviceId 服务号
     * @param methodId  方法号
     * @param payload   protobuf 请求字节
     * @param timeout   超时上限
     * @return std::string protobuf 响应字节
     * @throws RpcError 超时/连接失败/对端无响应
     */
    std::string call(uint16_t serviceId, uint16_t methodId, const std::string& payload,
                     std::chrono::milliseconds timeout = std::chrono::milliseconds(3000));

private:
    /**
     * @brief 单条客户端连接。
     */
    class Connection : public std::enable_shared_from_this<Connection> {
    public:
        Connection(asio::io_context& io, std::string host, unsigned short port);

        /**
         * @brief 建立连接（异步连接 + 同步等待）。
         * @return bool 成功为 true
         */
        bool connect(std::chrono::milliseconds timeout);

        /**
         * @brief 发起调用并等待响应。
         * @return std::string 响应 payload；连接中断/超时抛 RpcError
         */
        std::string call(uint16_t serviceId, uint16_t methodId, const std::string& payload,
                         std::chrono::milliseconds timeout, uint32_t requestId);

        /**
         * @brief 连接是否可用。
         */
        bool alive() const { return m_alive; }

    private:
        void doRead();
        void enqueueWrite(std::string data);
        void doWrite();
        void failAllPending(const std::string& reason);

        asio::io_context& m_io;
        tcp::socket m_socket;
        asio::strand<asio::any_io_executor> m_strand;
        std::string m_host;
        unsigned short m_port;
        net::BufReader m_bufReader;
        char m_readBuffer[8192] = {0};
        std::deque<std::string> m_writeQueue;
        bool m_writing = false;
        std::atomic<bool> m_alive{false};

        std::mutex m_pendingMutex;                                   ///< 保护 pending 表
        std::map<uint32_t, std::promise<std::string>> m_pending;     ///< requestId → 响应
    };

    asio::io_context m_io;                    ///< 客户端独立事件循环
    asio::executor_work_guard<asio::io_context::executor_type> m_workGuard;
    std::thread m_ioThread;                   ///< 驱动 m_io
    std::string m_host;
    unsigned short m_port;
    int m_poolSize;
    std::atomic<uint32_t> m_requestIdGen{1};  ///< 全局请求序号

    std::mutex m_poolMutex;                   ///< 保护空闲连接缓存
    std::deque<std::shared_ptr<Connection>> m_idle;
};

} // namespace lingxi::rpc
