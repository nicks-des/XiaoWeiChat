/**
 * @file TcpClient.h
 * @brief 客户端 TCP 长连接：独立 IO 线程 + 心跳 + 分包 + 线程安全发送（纯 C++，无 Qt 依赖）。
 *
 * 线程模型（docs/01 §7）：IO 在后台线程；回调在 IO 线程触发，由上层（AccountService）
 * 负责切换回 UI 线程。发送经 strand 串行；心跳 30s 一帧空包（docs/02 §5）。
 */
#pragma once

#include <boost/asio.hpp>
#include <boost/asio/strand.hpp>

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "lingxi/base/NonCopyable.h"
#include "lingxi/net/BufReader.h"
#include "lingxi/net/Packet.h"

namespace asio = boost::asio;  ///< 项目内简写（见 docs/01：全栈 boost）
using asio::ip::tcp;

namespace lingxi::client {

/**
 * @brief TCP 长连接客户端（单连接语义：一次承载一个登录会话）。
 */
class TcpClient : private NonCopyable, public std::enable_shared_from_this<TcpClient> {
public:
    /** 收帧回调：IO 线程触发（msgId + protobuf body） */
    using PacketCallback = std::function<void(uint16_t msgId, const std::string& body)>;
    /** 连接状态回调：connected=true 已建立 / false 已断开 */
    using StateCallback = std::function<void(bool connected, const std::string& errMsg)>;

    TcpClient();
    ~TcpClient();

    /**
     * @brief 设置回调（须在 connectAsync 之前）。
     */
    void setCallbacks(PacketCallback onPacket, StateCallback onState);

    /**
     * @brief 异步连接（内部自动启动 IO 线程与心跳）。
     */
    void connectAsync(const std::string& host, unsigned short port);

    /**
     * @brief 主动关闭并停止 IO 线程（可析构后重连：对象支持 reset 复用由上层负责重建）。
     */
    void close();

    /**
     * @brief 发送一帧（线程安全，经 strand 串行）。
     * @param msgId IM 帧 msgId
     * @param body  protobuf body
     */
    void send(uint16_t msgId, const std::string& body);

    /**
     * @brief 连接是否可用。
     */
    bool isConnected() const { return m_connected.load(); }

private:
    void startHeartbeat();
    void doRead();
    void doWrite();

    asio::io_context m_io;         ///< 独立事件循环
    asio::executor_work_guard<asio::io_context::executor_type> m_workGuard;
    std::thread m_ioThread;        ///< IO 线程
    tcp::socket m_socket;          ///< 长连接套接字
    asio::strand<asio::any_io_executor> m_strand;  ///< 发送串行化
    std::shared_ptr<asio::steady_timer> m_heartbeatTimer;  ///< 心跳定时器

    net::BufReader m_bufReader;    ///< 分包缓冲（IO 线程独占）
    char m_readBuffer[8192] = {0}; ///< 读缓冲
    std::deque<std::string> m_writeQueue;  ///< 写队列（strand 内访问）
    bool m_writing = false;

    std::atomic<bool> m_connected{false};
    std::atomic<bool> m_closed{false};
    std::atomic<uint32_t> m_pktSeq{1};     ///< 帧序号（传输层请求匹配）

    PacketCallback m_onPacket;             ///< 收帧回调（连接前设置）
    StateCallback m_onState;               ///< 状态回调
    std::mutex m_callbackMutex;            ///< 回调表锁（防设置与触发并发）
};

} // namespace lingxi::client
