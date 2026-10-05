/**
 * @file TcpClient.cpp
 * @brief 客户端 TCP 长连接实现。
 */
#include "TcpClient.h"

#include "lingxi/logging/Logger.h"

namespace lingxi::client {

namespace {

/** 心跳周期（秒，docs/02 §5：30s 一帧，90s 判死） */
constexpr int kHeartbeatSec = 30;

/** 心跳帧 msgId */
constexpr uint16_t kMsgIdHeartbeat = 0x0000;

} // namespace

TcpClient::TcpClient()
    : m_workGuard(asio::make_work_guard(m_io)), m_socket(m_io),
      m_strand(m_socket.get_executor()) {
    m_ioThread = std::thread([this] { m_io.run(); });
}

TcpClient::~TcpClient() {
    close();
}

void TcpClient::setCallbacks(PacketCallback onPacket, StateCallback onState) {
    std::lock_guard<std::mutex> lock(m_callbackMutex);
    m_onPacket = std::move(onPacket);
    m_onState = std::move(onState);
}

void TcpClient::connectAsync(const std::string& host, unsigned short port) {
    auto self = std::shared_ptr<TcpClient>(this, [](TcpClient*) {});  // 非拥有引用
    asio::post(m_io, [this, self, host, port] {
        asio::ip::tcp::resolver resolver(m_io);
        auto endpoints = resolver.resolve(host, std::to_string(port));
        asio::async_connect(
            m_socket, endpoints,
            [this, self](boost::system::error_code ec, const tcp::endpoint&) {
                StateCallback onState;
                {
                    std::lock_guard<std::mutex> lock(m_callbackMutex);
                    onState = m_onState;
                }
                if (ec) {
                    LX_LOG_ERROR("TcpClient connect failed: {}", ec.message());
                    if (onState) {
                        onState(false, ec.message());
                    }
                    return;
                }
                m_connected = true;
                LX_LOG_INFO("TcpClient connected {}:{}", m_socket.remote_endpoint().address().to_string(),
                            m_socket.remote_endpoint().port());
                if (onState) {
                    onState(true, "");
                }
                startHeartbeat();
                doRead();
            });
    });
}

void TcpClient::close() {
    bool expected = false;
    if (!m_closed.compare_exchange_strong(expected, true)) {
        return;
    }
    asio::post(m_strand, [this, self = std::shared_ptr<TcpClient>(this, [](TcpClient*) {})] {
        boost::system::error_code ec;
        if (m_heartbeatTimer) {
            m_heartbeatTimer->cancel();
        }
        m_socket.close(ec);
    });
    m_workGuard.reset();
    if (m_ioThread.joinable()) {
        m_ioThread.join();
    }
}

void TcpClient::send(uint16_t msgId, const std::string& body) {
    const uint32_t seq = m_pktSeq.fetch_add(1);
    const std::string frame = net::encodePacket(msgId, seq, body, net::kFlagNone);
    asio::post(m_strand, [this, self = std::shared_ptr<TcpClient>(this, [](TcpClient*) {}), frame]() mutable {
        m_writeQueue.push_back(std::move(frame));
        if (m_writing) {
            return;
        }
        m_writing = true;
        doWrite();
    });
}

void TcpClient::startHeartbeat() {
    m_heartbeatTimer = std::make_shared<asio::steady_timer>(m_io);
    auto timer = m_heartbeatTimer;
    timer->expires_after(std::chrono::seconds(kHeartbeatSec));
    timer->async_wait([this, self = std::shared_ptr<TcpClient>(this, [](TcpClient*) {})](
                          const boost::system::error_code& ec) {
        if (ec || m_closed.load()) {
            return;
        }
        send(kMsgIdHeartbeat, "");  // 空心跳帧
        startHeartbeat();           // 续期
    });
}

void TcpClient::doRead() {
    auto self = std::shared_ptr<TcpClient>(this, [](TcpClient*) {});
    m_socket.async_read_some(
        asio::buffer(m_readBuffer),
        [this, self](boost::system::error_code ec, std::size_t length) {
            if (ec) {
                m_connected = false;
                LX_LOG_WARN("TcpClient read closed: {}", ec.message());
                PacketCallback onPacket;
                StateCallback onState;
                {
                    std::lock_guard<std::mutex> lock(m_callbackMutex);
                    onPacket = m_onPacket;
                    onState = m_onState;
                }
                if (onState) {
                    onState(false, ec.message());
                }
                return;
            }
            m_bufReader.feed(m_readBuffer, length);

            net::DecodedPacket packet;
            PacketCallback onPacket;
            {
                std::lock_guard<std::mutex> lock(m_callbackMutex);
                onPacket = m_onPacket;
            }
            while (m_bufReader.tryExtractPacket(packet)) {
                if (onPacket) {
                    onPacket(packet.header.msgId, packet.body);
                }
            }
            doRead();
        });
}

void TcpClient::doWrite() {
    auto self = std::shared_ptr<TcpClient>(this, [](TcpClient*) {});
    asio::async_write(m_socket, asio::buffer(m_writeQueue.front()),
                      asio::bind_executor(m_strand, [this, self](boost::system::error_code ec, std::size_t) {
                          if (ec) {
                              LX_LOG_WARN("TcpClient write failed: {}", ec.message());
                              return;
                          }
                          m_writeQueue.pop_front();
                          if (!m_writeQueue.empty()) {
                              doWrite();
                          } else {
                              m_writing = false;
                          }
                      }));
}

} // namespace lingxi::client
