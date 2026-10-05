/**
 * @file ChatSession.h
 * @brief ChatServer 客户端会话：帧读写、登录绑定、心跳、登出（M1 骨架）。
 */
#pragma once

#include <boost/asio.hpp>
#include <boost/asio/strand.hpp>

namespace asio = boost::asio;  ///< 项目内简写（见 docs/01：全栈 boost）
using asio::ip::tcp;             ///< TCP 类型简写

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>

#include "lingxi/net/BufReader.h"
#include "lingxi/net/Packet.h"

namespace lingxi {

class ChatServer;

/**
 * @brief 单条客户端 TCP 会话。
 */
class ChatSession : public std::enable_shared_from_this<ChatSession> {
public:
    ChatSession(tcp::socket socket, ChatServer& server, uint64_t sessionId);

    /**
     * @brief 启动读循环（登录前仅处理心跳与登录帧）。
     */
    void start();

    /**
     * @brief 向客户端下发一帧（线程安全，经 strand 串行）。
     * @param msgId IM 帧 msgId
     * @param body  protobuf body
     */
    void sendFrame(uint16_t msgId, const std::string& body);

    /**
     * @brief 下发最后一帧后关闭连接（单 strand 任务内完成，杜绝发帧与关闭的竞态）。
     * @param msgId IM 帧 msgId
     * @param body  protobuf body
     */
    void sendFrameThenClose(uint16_t msgId, const std::string& body);

    /**
     * @brief 是否已绑定登录用户。
     */
    bool isBound() const { return m_uid > 0; }

    /**
     * @brief 绑定的 uid（未绑定为 0）。
     */
    int64_t uid() const { return m_uid; }

    /**
     * @brief 关闭连接（顶号/登出时调用）。
     */
    void close();

private:
    void doRead();
    void dispatchFrame(const net::DecodedPacket& packet);
    void handleHeartbeat();
    void handleLogin(const net::DecodedPacket& packet);
    void handleLogout();
    void onLoginSuccess(int64_t uid);
    void enqueueWrite(std::string data);
    void doWrite();

    tcp::socket m_socket;                                     ///< 客户端套接字
    asio::strand<asio::any_io_executor> m_strand;   ///< 写序列化
    ChatServer& m_server;                                     ///< 所属服务器
    const uint64_t m_sessionId;                               ///< 会话序号（日志）
    net::BufReader m_bufReader;                               ///< 流缓冲
    char m_readBuffer[8192] = {0};                            ///< 读缓冲
    std::atomic<int64_t> m_uid{0};                            ///< 绑定 uid（0=未登录）
    std::deque<std::string> m_writeQueue;                     ///< 写队列（strand 内）
    bool m_writing = false;
    bool m_closePending = false;  ///< 待写刷完后关闭（strand 内访问）
    std::atomic<bool> m_closed{false};
};

} // namespace lingxi
