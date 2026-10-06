/**
 * @file HttpManager.cpp
 * @brief Beast 同步 HTTP 客户端实现（通用请求 + JSON 快捷封装）。
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

HttpResult httpRequest(const std::string& method, const std::string& host, unsigned short port,
                       const std::string& target, const std::string& body, int timeoutSec,
                       const std::map<std::string, std::string>& extraHeaders) {
    HttpResult result;
    try {
        asio::io_context io;
        tcp::socket socket(io);

        // 连接超时：Windows 下通过套接字选项近似（同步 connect 无超时参数）
        asio::ip::tcp::resolver resolver(io);
        auto endpoints = resolver.resolve(host, std::to_string(port));
        asio::connect(socket, endpoints);

        // 收发超时：Windows 下 SO_RCVTIMEO/SO_SNDTIMEO 为 DWORD 毫秒（Linux 才是 timeval）——
        // 传错结构会被解释成 30ms 导致大响应超时（M4 实测踩坑，见 devlog）
#ifdef _WIN32
        const DWORD timeoutMs = static_cast<DWORD>(timeoutSec) * 1000;
        setsockopt(socket.native_handle(), SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
        setsockopt(socket.native_handle(), SOL_SOCKET, SO_SNDTIMEO,
                   reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
#else
        timeval tv{timeoutSec, 0};
        setsockopt(socket.native_handle(), SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&tv), sizeof(tv));
        setsockopt(socket.native_handle(), SOL_SOCKET, SO_SNDTIMEO,
                   reinterpret_cast<const char*>(&tv), sizeof(tv));
#endif

        http::request<http::string_body> request{http::string_to_verb(method), target, 11};
        request.set(http::field::host, host);
        request.set(http::field::connection, "close");
        for (const auto& extra : extraHeaders) {
            request.set(extra.first, extra.second);
        }
        if (!body.empty()) {
            request.set(http::field::content_type, "application/octet-stream");
            request.body() = body;
            request.prepare_payload();
        }
        http::write(socket, request);

        beast::flat_buffer buffer;
        http::response<http::string_body> response;
        http::read(socket, buffer, response);

        result.status = static_cast<long>(response.result_int());
        result.body = response.body();
        for (const auto& field : response) {
            result.headers[std::string(field.name_string())] = std::string(field.value());
        }
        boost::system::error_code ec;
        socket.shutdown(tcp::socket::shutdown_both, ec);
    } catch (const beast::system_error& e) {
        result.error = e.what();
        LX_LOG_ERROR("HttpManager {} {}:{}{} failed: {}", method, host, port, target, e.what());
    } catch (const std::exception& e) {
        result.error = e.what();
        LX_LOG_ERROR("HttpManager exception: {}", e.what());
    }
    return result;
}

HttpResult postJson(const std::string& host, unsigned short port, const std::string& target,
                    const std::string& body, int timeoutSec) {
    return httpRequest("POST", host, port, target, body, timeoutSec);
}

} // namespace lingxi::client
