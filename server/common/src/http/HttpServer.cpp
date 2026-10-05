/**
 * @file HttpServer.cpp
 * @brief Beast HTTP 服务端实现。
 */
#include "lingxi/http/HttpServer.h"

#include "lingxi/logging/Logger.h"

namespace lingxi::http {

namespace beast = boost::beast;
namespace http = boost::beast::http;

/** 请求体上限 1MB（JSON 接口足够） */
constexpr std::size_t kMaxBodyBytes = 1024 * 1024;

HttpResponse makeJsonResponse(http::status status, const std::string& jsonBody) {
    HttpResponse response{status, 11};  // HTTP/1.1
    response.set(http::field::server, "lingxi-gate");
    response.set(http::field::content_type, "application/json; charset=utf-8");
    response.set(http::field::connection, "close");
    response.keep_alive(false);
    response.body() = jsonBody;
    response.prepare_payload();
    return response;
}

HttpResponse makeErrorResponse(http::status status, int errCode, const std::string& errMsg) {
    return makeJsonResponse(status, "{\"err_code\":" + std::to_string(errCode) +
                                        ",\"err_msg\":\"" + errMsg + "\"}");
}

std::string routeKey(const std::string& method, const std::string& target) {
    return method + " " + target;
}

HttpServer::HttpServer(asio::io_context& ioContext, unsigned short port, ThreadPool& handlerPool)
    : m_io(ioContext), m_acceptor(ioContext, {asio::ip::tcp::v4(), port}),
      m_handlerPool(handlerPool) {}

void HttpServer::route(const std::string& method, const std::string& target, Handler handler) {
    std::lock_guard<std::mutex> lock(m_routeMutex);
    m_routes[routeKey(method, target)] = std::move(handler);
}

void HttpServer::start() {
    doAccept();
}

void HttpServer::doAccept() {
    m_acceptor.async_accept([this](boost::system::error_code ec, tcp::socket socket) {
        if (ec) {
            LX_LOG_WARN("HttpServer accept failed: {}", ec.message());
        } else {
            std::make_shared<Session>(std::move(socket), *this)->start();
        }
        doAccept();
    });
}

HttpServer::Handler HttpServer::findHandler(const std::string& method, const std::string& target) {
    std::lock_guard<std::mutex> lock(m_routeMutex);
    auto it = m_routes.find(routeKey(method, target));
    return it == m_routes.end() ? nullptr : it->second;
}

/* ==================== Session ==================== */

HttpServer::Session::Session(tcp::socket socket, HttpServer& server)
    : m_socket(std::move(socket)), m_server(server) {}

void HttpServer::Session::start() {
    doRead();
}

void HttpServer::Session::doRead() {
    auto self = shared_from_this();
    auto parser = std::make_shared<http::request_parser<http::string_body>>();
    parser->body_limit(kMaxBodyBytes);

    http::async_read(m_socket, m_buffer, *parser,
                     [this, self, parser](boost::system::error_code ec, std::size_t) {
                         if (ec) {
                             return;  // 客户端断开/读失败：直接结束
                         }
                         handleRequest(parser->release());
                     });
}

void HttpServer::Session::handleRequest(HttpRequest&& request) {
    const std::string peerIp = [&] {
        boost::system::error_code ec;
        const auto peer = m_socket.remote_endpoint(ec);
        return ec ? std::string("unknown") : peer.address().to_string();
    }();

    auto handler = m_server.findHandler(std::string(request.method_string()),
                                        std::string(request.target()));
    if (handler == nullptr) {
        doWrite(makeErrorResponse(http::status::not_found, 404, "route not found"));
        return;
    }

    auto self = shared_from_this();
    LX_LOG_INFO("HTTP {} {} from {}", std::string(request.method_string()),
                std::string(request.target()), peerIp);

    m_server.m_handlerPool.submit(
        [this, self, request = std::move(request), handler = std::move(handler),
         peerIp]() mutable {
            HttpResponse response;
            try {
                response = handler(request, peerIp);
            } catch (const std::exception& e) {
                LX_LOG_ERROR("HTTP handler exception: {}", e.what());
                response = makeErrorResponse(http::status::internal_server_error, 500,
                                             "internal error");
            }
            doWrite(std::move(response));
        });
}

void HttpServer::Session::doWrite(HttpResponse response) {
    auto self = shared_from_this();
    // Beast async_write 只持有消息引用：必须将响应持有到写完（否则 UAF）
    auto sharedResponse = std::make_shared<HttpResponse>(std::move(response));
    const bool keepAlive = sharedResponse->keep_alive();

    http::async_write(
        m_socket, *sharedResponse,
        [this, self, sharedResponse, keepAlive](boost::system::error_code ec, std::size_t) {
            if (ec) {
                LX_LOG_WARN("HttpServer write failed: {}", ec.message());
            }
            if (!keepAlive) {
                boost::system::error_code ignored;
                m_socket.shutdown(tcp::socket::shutdown_send, ignored);
            }
            // keep_alive=false：写完即结束会话（响应中已置 Connection: close）
        });
}

} // namespace lingxi::http
