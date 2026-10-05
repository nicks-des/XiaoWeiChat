/**
 * @file RpcServer.h
 * @brief 自研 RPC 服务端：Asio 长连接 + 处理器注册表 + 线程池执行 + 有序写队列。
 *
 * 帧约定见 docs/01 §4：传输帧 msgId=0xFF01，body = RpcHead(9B) + protobuf payload。
 * 请求在线程池中执行处理器，响应经 strand 串行写回（不阻塞 IO 线程）。
 */
#pragma once

#include <boost/asio.hpp>
#include <boost/asio/strand.hpp>

namespace asio = boost::asio;  ///< 项目内简写（见 docs/01：全栈 boost）
using asio::ip::tcp;             ///< TCP 类型简写

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "lingxi/base/NonCopyable.h"
#include "lingxi/base/ThreadPool.h"
#include "lingxi/net/BufReader.h"
#include "lingxi/net/Packet.h"

namespace lingxi::rpc {

/**
 * @brief RPC 服务端。
 */
class RpcServer : private NonCopyable {
public:
    /**
     * @brief 处理器：入参为 protobuf payload 原始字节，返回 protobuf 响应字节。
     */
    using Handler = std::function<std::string(const std::string& payload)>;

    /**
     * @param ioContext    事件循环（由宿主进程驱动）
     * @param port         RPC 监听端口
     * @param handlerPool  处理器执行线程池（宿主进程共享）
     */
    RpcServer(asio::io_context& ioContext, unsigned short port, ThreadPool& handlerPool);

    /**
     * @brief 注册处理器（须在 start() 之前完成注册）。
     */
    void registerHandler(uint16_t serviceId, uint16_t methodId, Handler handler);

    /**
     * @brief 启动接受循环（异步，不阻塞）。
     */
    void start();

private:
    /**
     * @brief 单条 RPC 连接会话。
     */
    class Session : public std::enable_shared_from_this<Session> {
    public:
        Session(tcp::socket socket, RpcServer& server);
        void start();

    private:
        void doRead();
        void handlePacket(const net::DecodedPacket& packet);
        void enqueueWrite(std::string data);
        void doWrite();

        tcp::socket m_socket;                          ///< 会话套接字
        asio::strand<asio::any_io_executor> m_strand; ///< 写序列化
        net::BufReader m_bufReader;                    ///< 流缓冲（读侧单链路，无需加锁）
        char m_readBuffer[8192] = {0};                 ///< 读缓冲
        RpcServer& m_server;                           ///< 所属服务端
        std::deque<std::string> m_writeQueue;          ///< 待写队列（strand 内访问）
        bool m_writing = false;
    };

    /**
     * @brief 异步接受一个连接。
     */
    void doAccept();

    /**
     * @brief 查找处理器；未注册返回 nullptr。
     */
    Handler findHandler(uint16_t serviceId, uint16_t methodId);

    asio::io_context& m_io;
    tcp::acceptor m_acceptor;
    ThreadPool& m_handlerPool;
    std::unordered_map<uint32_t, Handler> m_handlers; ///< key = serviceId<<16 | methodId
    std::mutex m_handlerMutex;                        ///< 保护处理器表
    std::atomic<uint64_t> m_sessionSeq{0};            ///< 会话序号（日志用）
};

} // namespace lingxi::rpc
