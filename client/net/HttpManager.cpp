/**
 * @file HttpManager.cpp
 * @brief Beast 同步 HTTP 客户端实现。
 */
#include "HttpManager.h"

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include "lingxi/logging/Logger.h"

namespace beast = boost::beast;
namespace http = beast::http;
namespace asio = boost::asio;
using asio::ip::tcp;

namespace lingxi::client {

HttpResult postJson(const std::string& host, unsigned short port, const std::string& target,
                    const std::string& body, int timeoutSec) {
    HttpResult result;
    try {
        asio::io_context io;
        tcp::socket socket(io);

        // 连接超时：Windows 下通过套接字选项近似（同步 connect 无超时参数）
        asio::ip::tcp::resolver resolver(io);
        auto endpoints = resolver.resolve(host, std::to_string(port));
        asio::connect(socket, endpoints);

        // 收发超时：借内核级 SO_RCVTIMEO/SO_SNDTIMEO（简化实现，M2 可换 deadline timer）
        timeval tv{timeoutSec, 0};
        setsockopt(socket.native_handle(), SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&tv), sizeof(tv));
        setsockopt(socket.native_handle(), SOL_SOCKET, SO_SNDTIMEO,
                   reinterpret_cast<const char*>(&tv), sizeof(tv));

        http::request<http::string_body> request{http::verb::post, target, 11};
        request.set(http::field::host, host);
        request.set(http::field::content_type, "application/json");
        request.set(http::field::connection, "close");
        request.body() = body;
        request.prepare_payload();
        http::write(socket, request);

        beast::flat_buffer buffer;
        http::response<http::string_body> response;
        http::read(socket, buffer, response);

        result.status = static_cast<long>(response.result_int());
        result.body = response.body();
        boost::system::error_code ec;
        socket.shutdown(tcp::socket::shutdown_both, ec);
    } catch (const beast::system_error& e) {
        result.error = e.what();
        LX_LOG_ERROR("HttpManager {}:{}{} failed: {}", host, port, target, e.what());
    } catch (const std::exception& e) {
        result.error = e.what();
        LX_LOG_ERROR("HttpManager exception: {}", e.what());
    }
    return result;
}

} // namespace lingxi::client
