/**
 * @file HttpServer.h
 * @brief Boost.Beast 轻量 HTTP 服务端：异步监听 + 路由表 + 线程池执行处理器。
 *
 * M1 仅支持 application/json 请求响应、单请求单连接（Connection: close），
 * 满足注册/登录类低频接口；文件分块等场景在 M4 扩展。
 */
#pragma once

#include <boost/asio.hpp>
#include <boost/asio/strand.hpp>

namespace asio = boost::asio;  ///< 项目内简写（见 docs/01：全栈 boost）
using asio::ip::tcp;             ///< TCP 类型简写
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "lingxi/base/NonCopyable.h"
#include "lingxi/base/ThreadPool.h"

namespace lingxi::http {

/** HTTP 请求类型（字符串体） */
using HttpRequest = boost::beast::http::request<boost::beast::http::string_body>;
/** HTTP 响应类型 */
using HttpResponse = boost::beast::http::response<boost::beast::http::string_body>;

/**
 * @brief 构造 JSON 响应（统一 Content-Type 与 CORS 头）。
 * @param status HTTP 状态码
 * @param jsonBody 响应 JSON 字符串
 * @return HttpResponse
 */
HttpResponse makeJsonResponse(boost::beast::http::status status, const std::string& jsonBody);

/**
 * @brief 构造错误响应（统一错误结构 {"err_code":x,"err_msg":"..."}）。
 */
HttpResponse makeErrorResponse(boost::beast::http::status status, int errCode,
                               const std::string& errMsg);

/**
 * @brief 路由 key：method + " " + target（如 "POST /api/login"）。
 */
std::string routeKey(const std::string& method, const std::string& target);

/**
 * @brief Beast HTTP 服务端。
 */
class HttpServer : private NonCopyable {
public:
    /** 处理器：入参为原始请求与客户端 IP，返回响应 */
    using Handler = std::function<HttpResponse(const HttpRequest&, const std::string& clientIp)>;

    /**
     * @param ioContext   事件循环
     * @param port        监听端口
     * @param handlerPool 处理器执行线程池（阻塞处理器不占用 IO 线程）
     */
    HttpServer(asio::io_context& ioContext, unsigned short port, ThreadPool& handlerPool);

    /**
     * @brief 注册路由（须在 start() 前完成）。
     * @param method HTTP 方法（"GET"/"POST"）
     * @param target 精确路径（如 "/api/login"）
     */
    void route(const std::string& method, const std::string& target, Handler handler);

    /**
     * @brief 启动接受循环（异步，不阻塞）。
     */
    void start();

private:
    /**
     * @brief 单条 HTTP 会话（一请求一连接）。
     */
    class Session : public std::enable_shared_from_this<Session> {
    public:
        Session(tcp::socket socket, HttpServer& server);
        void start();

    private:
        void doRead();
        void handleRequest(HttpRequest&& request);
        void doWrite(HttpResponse response);

        tcp::socket m_socket;
        HttpServer& m_server;
        boost::beast::flat_buffer m_buffer;  ///< 请求解析缓冲
    };

    void doAccept();
    Handler findHandler(const std::string& method, const std::string& target);

    asio::io_context& m_io;
    tcp::acceptor m_acceptor;
    ThreadPool& m_handlerPool;
    std::map<std::string, Handler> m_routes;  ///< 路由表（start 前注册，读多写少）
    std::mutex m_routeMutex;
};

} // namespace lingxi::http
