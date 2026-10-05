/**
 * @file ChatSession.cpp
 * @brief 客户端会话实现（M1 骨架：心跳/登录/登出）。
 */
#include "ChatSession.h"

#include "ChatServer.h"
#include "MessageService.h"
#include "lingxi/base/TimeUtil.h"
#include "lingxi/logging/Logger.h"
#include "lingxi/rpc/RpcFrame.h"

#include "lingxi.pb.h"

namespace lingxi {

ChatSession::ChatSession(tcp::socket socket, ChatServer& server, uint64_t sessionId)
    : m_socket(std::move(socket)), m_strand(m_socket.get_executor()), m_server(server),
      m_sessionId(sessionId) {}

void ChatSession::start() {
    doRead();
}

void ChatSession::sendFrame(uint16_t msgId, const std::string& body) {
    const std::string frame = net::encodePacket(msgId, 0, body, net::kFlagPush);
    asio::post(m_strand, [self = shared_from_this(), frame]() mutable {
        self->enqueueWrite(std::move(frame));
    });
}

void ChatSession::sendFrameThenClose(uint16_t msgId, const std::string& body) {
    if (m_closed.exchange(true)) {
        return;
    }
    const std::string frame = net::encodePacket(msgId, 0, body, net::kFlagPush);
    asio::post(m_strand, [self = shared_from_this(), frame]() mutable {
        // 单任务内入队末帧并置关闭标志：写完由完成回调关闭，帧不会丢失
        self->m_writeQueue.push_back(std::move(frame));
        self->m_closePending = true;
        if (!self->m_writing) {
            self->m_writing = true;
            self->doWrite();
        }
    });
}

void ChatSession::close() {
    if (m_closed.exchange(true)) {
        return;
    }
    asio::post(m_strand, [self = shared_from_this()] {
        // 延迟关闭：待写队列刷完再断开（否则顶号通知等末帧会被丢弃）
        self->m_closePending = true;
        if (!self->m_writing && self->m_writeQueue.empty()) {
            boost::system::error_code ec;
            self->m_socket.close(ec);
        }
    });
}

void ChatSession::doRead() {
    auto self = shared_from_this();
    m_socket.async_read_some(
        asio::buffer(m_readBuffer),
        [this, self](boost::system::error_code ec, std::size_t length) {
            if (ec) {
                LX_LOG_INFO("session {} closed (uid={})", m_sessionId, m_uid.load());
                m_server.unbindSession(m_uid.load(), this);
                return;
            }
            m_bufReader.feed(m_readBuffer, length);

            net::DecodedPacket packet;
            while (m_bufReader.tryExtractPacket(packet)) {
                dispatchFrame(packet);
                if (m_closed.load()) {
                    return;
                }
            }
            doRead();
        });
}

void ChatSession::dispatchFrame(const net::DecodedPacket& packet) {
    switch (packet.header.msgId) {
        case 0x0000:
            handleHeartbeat();
            break;
        case 0x0101: {
            // 登录含阻塞 RPC：移交线程池，读循环立即继续
            auto self = shared_from_this();
            const auto captured = packet;
            m_server.handlerPool().submit([this, self, captured] { handleLogin(captured); });
            break;
        }
        case 0x0103:
            handleLogout();
            break;
        case 0x0301:  // 消息发送
        case 0x0304:  // 已读上报
        case 0x0306:  // 撤回
        case 0x0308:  // 漫游同步
        case 0x030A:  // 会话列表
            if (!isBound()) {
                LX_LOG_WARN("session {} business frame before login, msgId={:#06x}",
                            m_sessionId, packet.header.msgId);
                return;
            }
            {
                auto self = shared_from_this();
                const auto captured = packet;
                const uint16_t msgId = packet.header.msgId;
                m_server.handlerPool().submit(
                    [this, self, captured, msgId] { handleBusinessFrame(msgId, captured); });
            }
            break;
        default:
            LX_LOG_WARN("session {} got unexpected msgId={:#06x}", m_sessionId,
                        packet.header.msgId);
            break;
    }
}

void ChatSession::handleBusinessFrame(uint16_t msgId, const net::DecodedPacket& packet) {
    auto& service = *m_server.messageService();
    const int64_t uid = m_uid.load();
    switch (msgId) {
        case 0x0301: {
            // 发送管线：先落库（工作线程），ACK 回发送者，投递路由到接收者
            auto outcome = service.handleSend(uid, packet.body);
            if (outcome.errCode == 0) {
                sendFrame(0x0302, outcome.ackBody);
            } else {
                MessageAck errAck;
                errAck.set_err_code(outcome.errCode);
                errAck.set_err_msg(outcome.errMsg);
                errAck.set_client_msg_id("");
                sendFrame(0x0302, errAck.SerializeAsString());
            }
            for (const auto& delivery : outcome.deliveries) {
                m_server.deliverToUid(delivery.uid, delivery.msgId, delivery.body);
            }
            break;
        }
        case 0x030A:
            sendFrame(0x030A, service.handleConversationList(uid));
            break;
        case 0x0308:
            sendFrame(0x0309, service.handleSync(uid, packet.body));
            break;
        case 0x0304: {
            for (const auto& delivery : service.handleReadAck(uid, packet.body)) {
                m_server.deliverToUid(delivery.uid, delivery.msgId, delivery.body);
            }
            break;
        }
        case 0x0306: {
            for (const auto& delivery : service.handleRecall(uid, packet.body)) {
                m_server.deliverToUid(delivery.uid, delivery.msgId, delivery.body);
            }
            break;
        }
        default:
            break;
    }
}

void ChatSession::handleHeartbeat() {
    // 心跳回执：原样回一个空心跳帧
    enqueueWrite(net::encodePacket(0x0000, 0, "", net::kFlagNone));
}

void ChatSession::handleLogin(const net::DecodedPacket& packet) {
    if (isBound()) {
        LX_LOG_WARN("session {} duplicate login frame", m_sessionId);
        return;
    }

    LoginRequest request;
    if (!request.ParseFromString(packet.body)) {
        LoginResponse bad;
        bad.set_err_code(400);
        bad.set_err_msg("bad login request");
        sendFrame(0x0102, bad.SerializeAsString());
        return;
    }

    // RPC 到 Status 校验 token（docs/01 §5.1）
    VerifyTokenRequest verifyReq;
    verifyReq.set_uid(request.uid());
    verifyReq.set_token(request.token());

    LoginResponse response;
    try {
        VerifyTokenResponse verifyRsp;
        if (!verifyRsp.ParseFromString(m_server.statusRpc()->call(
                rpc::kServiceStatus, 0x03, verifyReq.SerializeAsString()))) {
            response.set_err_code(500);
            response.set_err_msg("status bad response");
        } else if (verifyRsp.err_code() != 0) {
            response.set_err_code(401);
            response.set_err_msg("token invalid");
        }
    } catch (const rpc::RpcError& e) {
        LX_LOG_ERROR("verify token rpc failed: {}", e.what());
        response.set_err_code(503);
        response.set_err_msg("status unavailable");
    }

    if (response.err_code() != 0) {
        sendFrame(0x0102, response.SerializeAsString());
        return;
    }

    response.set_err_code(0);
    response.set_server_time(TimeUtil::nowMs());
    onLoginSuccess(request.uid());
    sendFrame(0x0102, response.SerializeAsString());
}

void ChatSession::handleLogout() {
    LX_LOG_INFO("session {} logout (uid={})", m_sessionId, m_uid.load());
    m_server.unbindSession(m_uid.load(), this);
    m_uid.store(0);
    close();
}

void ChatSession::onLoginSuccess(int64_t uid) {
    m_uid.store(uid);
    m_server.bindSession(uid, shared_from_this());
}

void ChatSession::enqueueWrite(std::string data) {
    // strand 内调用
    m_writeQueue.push_back(std::move(data));
    if (m_writing) {
        return;
    }
    m_writing = true;
    doWrite();
}

void ChatSession::doWrite() {
    auto self = shared_from_this();
    asio::async_write(m_socket, asio::buffer(m_writeQueue.front()),
                      asio::bind_executor(m_strand, [self](boost::system::error_code ec, std::size_t) {
                          if (ec) {
                              LX_LOG_WARN("session write failed: {}", ec.message());
                              return;
                          }
                          self->m_writeQueue.pop_front();
                          if (!self->m_writeQueue.empty()) {
                              self->doWrite();
                          } else {
                              self->m_writing = false;
                              if (self->m_closePending) {
                                  // 优雅关闭：先 shutdown_send 保证末帧先于 FIN 到达，
                                  // 延迟 2s 再 close（避免 Windows close 丢弃未刷出数据/RST）
                                  boost::system::error_code shutEc;
                                  self->m_socket.shutdown(asio::socket_base::shutdown_send, shutEc);
                                  auto closer = std::make_shared<asio::steady_timer>(
                                      self->m_socket.get_executor());
                                  closer->expires_after(std::chrono::seconds(2));
                                  closer->async_wait([self, closer](const boost::system::error_code& ec) {
                                      if (!ec) {
                                          boost::system::error_code closeEc;
                                          self->m_socket.close(closeEc);
                                      }
                                  });
                              }
                          }
                      }));
}

} // namespace lingxi
