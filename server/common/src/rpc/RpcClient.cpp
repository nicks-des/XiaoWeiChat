/**
 * @file RpcClient.cpp
 * @brief RPC 客户端池实现。
 */
#include "lingxi/rpc/RpcClient.h"

#include "lingxi/logging/Logger.h"
#include "lingxi/rpc/RpcFrame.h"

namespace lingxi::rpc {

/* ==================== Connection ==================== */

RpcClientPool::Connection::Connection(asio::io_context& io, std::string host,
                                      unsigned short port)
    : m_io(io), m_socket(io), m_strand(m_socket.get_executor()), m_host(std::move(host)),
      m_port(port) {}

bool RpcClientPool::Connection::connect(std::chrono::milliseconds timeout) {
    std::promise<bool> connectResult;
    auto future = connectResult.get_future();
    auto self = shared_from_this();

    asio::ip::tcp::resolver resolver(m_io);
    asio::async_connect(
        m_socket, resolver.resolve(m_host, std::to_string(m_port)),
        [this, self, &connectResult](boost::system::error_code ec, const asio::ip::tcp::endpoint&) {
            if (ec) {
                LX_LOG_WARN("RpcClient connect {}:{} failed: {}", m_host, m_port, ec.message());
                connectResult.set_value(false);
                return;
            }
            m_alive = true;
            LX_LOG_INFO("RpcClient connected {}:{}", m_host, m_port);
            doRead();
            connectResult.set_value(true);
        });

    if (future.wait_for(timeout) != std::future_status::ready) {
        boost::system::error_code ignored;
        m_socket.close(ignored);
        return false;
    }
    return future.get();
}

std::string RpcClientPool::Connection::call(uint16_t serviceId, uint16_t methodId,
                                            const std::string& payload,
                                            std::chrono::milliseconds timeout,
                                            uint32_t requestId) {
    std::promise<std::string> response;
    auto future = response.get_future();
    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        m_pending.emplace(requestId, std::move(response));
    }

    enqueueWrite(net::encodePacket(
        kMsgIdRpcFrame, requestId, encodeRpcBody(serviceId, methodId, requestId, kRpcFlagNone, payload),
        net::kFlagNone));

    auto status = future.wait_for(timeout);
    if (status == std::future_status::ready) {
        return future.get();  // 可能 rethrow 失败异常
    }
    // 超时：撤销 pending（响应迟到时丢弃）
    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        m_pending.erase(requestId);
    }
    throw RpcError("rpc timeout: service=" + std::to_string(serviceId) +
                   " method=" + std::to_string(methodId));
}

void RpcClientPool::Connection::doRead() {
    auto self = shared_from_this();
    m_socket.async_read_some(
        asio::buffer(m_readBuffer),
        [this, self](boost::system::error_code ec, std::size_t length) {
            if (ec) {
                LX_LOG_WARN("RpcClient session closed: {}", ec.message());
                m_alive = false;
                failAllPending("connection closed");
                return;
            }
            m_bufReader.feed(m_readBuffer, length);

            net::DecodedPacket packet;
            while (m_bufReader.tryExtractPacket(packet)) {
                if (packet.header.msgId != kMsgIdRpcFrame) {
                    continue;
                }
                RpcHead head;
                std::string payload;
                if (!decodeRpcBody(packet.body, head, payload)) {
                    continue;
                }
                std::promise<std::string> promise;
                {
                    std::lock_guard<std::mutex> lock(m_pendingMutex);
                    auto it = m_pending.find(head.requestId);
                    if (it == m_pending.end()) {
                        continue;  // 已超时撤销
                    }
                    promise = std::move(it->second);
                    m_pending.erase(it);
                }
                promise.set_value(payload);
            }
            doRead();
        });
}

void RpcClientPool::Connection::failAllPending(const std::string& reason) {
    std::map<uint32_t, std::promise<std::string>> pending;
    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        pending = std::move(m_pending);
        m_pending.clear();
    }
    for (auto& entry : pending) {
        try {
            entry.second.set_exception(
                std::make_exception_ptr(RpcError("rpc failed: " + reason)));
        } catch (const std::future_error&) {
            // promise 已被消费，忽略
        }
    }
}

void RpcClientPool::Connection::enqueueWrite(std::string data) {
    asio::post(m_strand, [self = shared_from_this(), data = std::move(data)]() mutable {
        self->m_writeQueue.push_back(std::move(data));
        if (self->m_writing) {
            return;
        }
        self->m_writing = true;
        self->doWrite();
    });
}

void RpcClientPool::Connection::doWrite() {
    auto self = shared_from_this();
    asio::async_write(m_socket, asio::buffer(m_writeQueue.front()),
                      asio::bind_executor(m_strand, [self](boost::system::error_code ec, std::size_t) {
                          if (ec) {
                              LX_LOG_WARN("RpcClient write failed: {}", ec.message());
                              self->m_alive = false;
                              self->failAllPending("write failed");
                              boost::system::error_code ignored;
                              self->m_socket.close(ignored);
                              return;
                          }
                          self->m_writeQueue.pop_front();
                          if (!self->m_writeQueue.empty()) {
                              self->doWrite();
                          } else {
                              self->m_writing = false;
                          }
                      }));
}

/* ==================== RpcClientPool ==================== */

RpcClientPool::RpcClientPool(std::string host, unsigned short port, int poolSize)
    : m_workGuard(asio::make_work_guard(m_io)), m_host(std::move(host)), m_port(port),
      m_poolSize(poolSize <= 0 ? 2 : poolSize) {
    m_ioThread = std::thread([this] { m_io.run(); });
}

RpcClientPool::~RpcClientPool() {
    {
        std::lock_guard<std::mutex> lock(m_poolMutex);
        m_idle.clear();
    }
    m_workGuard.reset();
    m_io.stop();
    if (m_ioThread.joinable()) {
        m_ioThread.join();
    }
}

std::string RpcClientPool::call(uint16_t serviceId, uint16_t methodId,
                                const std::string& payload, std::chrono::milliseconds timeout) {
    // 取空闲连接；无则新建（poolSize 仅约束空闲缓存上限，不限制瞬时并发）
    std::shared_ptr<Connection> conn;
    {
        std::lock_guard<std::mutex> lock(m_poolMutex);
        if (!m_idle.empty()) {
            conn = m_idle.back();
            m_idle.pop_back();
            if (!conn->alive()) {
                conn.reset();  // 死连接丢弃，走新建
            }
        }
    }

    if (conn == nullptr) {
        conn = std::make_shared<Connection>(m_io, m_host, m_port);
        if (!conn->connect(std::chrono::milliseconds(2000))) {
            throw RpcError("rpc connect failed: " + m_host + ":" + std::to_string(m_port));
        }
    }

    const uint32_t requestId = m_requestIdGen.fetch_add(1);
    try {
        auto result = conn->call(serviceId, methodId, payload, timeout, requestId);
        std::lock_guard<std::mutex> lock(m_poolMutex);
        if (conn->alive() && m_idle.size() < static_cast<size_t>(m_poolSize)) {
            m_idle.push_back(std::move(conn));  // 归还连接
        }
        return result;
    } catch (...) {
        // 失败连接不回池（可能已半开），下次调用重建
        throw;
    }
}

} // namespace lingxi::rpc
