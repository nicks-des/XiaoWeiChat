/**
 * @file RpcServer.cpp
 * @brief RPC 服务端实现。
 */
#include "lingxi/rpc/RpcServer.h"

#include "lingxi/logging/Logger.h"
#include "lingxi/rpc/RpcFrame.h"

namespace lingxi::rpc {

namespace {

/**
 * @brief 计算处理器表 key。
 */
uint32_t handlerKey(uint16_t serviceId, uint16_t methodId) {
    return (static_cast<uint32_t>(serviceId) << 16) | methodId;
}

} // namespace

/* ==================== RpcServer ==================== */

RpcServer::RpcServer(asio::io_context& ioContext, unsigned short port, ThreadPool& handlerPool)
    : m_io(ioContext), m_acceptor(ioContext, {asio::ip::tcp::v4(), port}),
      m_handlerPool(handlerPool) {}

void RpcServer::registerHandler(uint16_t serviceId, uint16_t methodId, Handler handler) {
    std::lock_guard<std::mutex> lock(m_handlerMutex);
    m_handlers[handlerKey(serviceId, methodId)] = std::move(handler);
}

void RpcServer::start() {
    doAccept();
}

void RpcServer::doAccept() {
    m_acceptor.async_accept([this](boost::system::error_code ec, tcp::socket socket) {
        if (ec) {
            LX_LOG_WARN("RpcServer accept failed: {}", ec.message());
        } else {
            const auto peer = socket.remote_endpoint();
            LX_LOG_INFO("RpcServer accepted: {}:{}", peer.address().to_string(), peer.port());
            std::make_shared<Session>(std::move(socket), *this)->start();
        }
        doAccept();
    });
}

RpcServer::Handler RpcServer::findHandler(uint16_t serviceId, uint16_t methodId) {
    std::lock_guard<std::mutex> lock(m_handlerMutex);
    auto it = m_handlers.find(handlerKey(serviceId, methodId));
    return it == m_handlers.end() ? nullptr : it->second;
}

/* ==================== Session ==================== */

RpcServer::Session::Session(tcp::socket socket, RpcServer& server)
    : m_socket(std::move(socket)), m_strand(m_socket.get_executor()), m_server(server) {}

void RpcServer::Session::start() {
    doRead();
}

void RpcServer::Session::doRead() {
    auto self = shared_from_this();
    m_socket.async_read_some(
        asio::buffer(m_readBuffer),
        [this, self](boost::system::error_code ec, std::size_t length) {
            if (ec) {
                LX_LOG_INFO("RpcServer session closed: {}", ec.message());
                return;
            }
            m_bufReader.feed(m_readBuffer, length);

            net::DecodedPacket packet;
            while (m_bufReader.tryExtractPacket(packet)) {
                handlePacket(packet);
            }
            doRead();
        });
}

void RpcServer::Session::handlePacket(const net::DecodedPacket& packet) {
    if (packet.header.msgId != kMsgIdRpcFrame) {
        LX_LOG_WARN("RpcServer got non-RPC frame, msgId={:#06x}", packet.header.msgId);
        return;
    }

    RpcHead head;
    std::string payload;
    if (!decodeRpcBody(packet.body, head, payload)) {
        LX_LOG_ERROR("RpcServer decode rpc body failed, drop frame");
        return;
    }

    auto handler = m_server.findHandler(head.serviceId, head.methodId);
    if (handler == nullptr) {
        LX_LOG_ERROR("RpcServer no handler: service={:#06x} method={:#06x}", head.serviceId,
                     head.methodId);
        if (!(head.flags & kRpcFlagPush)) {
            // 未注册方法回空响应，避免调用方悬挂
            enqueueWrite(net::encodePacket(kMsgIdRpcFrame, packet.header.pktSeq,
                                           encodeRpcBody(head.serviceId, head.methodId,
                                                         head.requestId, kRpcFlagResponse, ""),
                                          net::kFlagNone));
        }
        return;
    }

    // 处理器在共享线程池执行；结果经 strand 串行写回
    auto self = shared_from_this();
    const bool isPush = (head.flags & kRpcFlagPush) != 0;
    const uint32_t pktSeq = packet.header.pktSeq;
    const uint16_t serviceId = head.serviceId;
    const uint16_t methodId = head.methodId;
    const uint32_t requestId = head.requestId;

    m_server.m_handlerPool.submit([self, handler, payload, isPush, pktSeq, serviceId, methodId,
                                   requestId] {
        std::string result;
        try {
            result = handler(payload);
        } catch (const std::exception& e) {
            LX_LOG_ERROR("RpcServer handler exception: {}", e.what());
        }
        if (isPush) {
            return;  // 推送无需响应
        }
        const std::string frame = net::encodePacket(
            kMsgIdRpcFrame, pktSeq,
            encodeRpcBody(serviceId, methodId, requestId, kRpcFlagResponse, result),
            net::kFlagNone);
        asio::post(self->m_strand, [self, frame] { self->enqueueWrite(frame); });
    });
}

void RpcServer::Session::enqueueWrite(std::string data) {
    // strand 内调用：m_writeQueue/m_writing 无需额外加锁
    m_writeQueue.push_back(std::move(data));
    if (m_writing) {
        return;  // 已有写出链路在跑，排队即可
    }
    m_writing = true;
    doWrite();
}

void RpcServer::Session::doWrite() {
    auto self = shared_from_this();
    asio::async_write(m_socket, asio::buffer(m_writeQueue.front()),
                      asio::bind_executor(m_strand, [self](boost::system::error_code ec, std::size_t) {
                          if (ec) {
                              LX_LOG_WARN("RpcServer write failed: {}", ec.message());
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

} // namespace lingxi::rpc
