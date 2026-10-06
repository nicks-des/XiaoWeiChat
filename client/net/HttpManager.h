/**
 * @file HttpManager.h
 * @brief HTTP 客户端：Beast 同步 POST JSON（注册/登录用），纯 C++ 无 Qt 依赖。
 *
 * 调用方须在工作线程调用（同步阻塞实现）；AccountService 已负责线程切换。
 */
#pragma once

#include <map>
#include <string>

namespace lingxi::client {

/**
 * @brief HTTP 响应结果。
 */
struct HttpResult {
    long status = 0;       ///< HTTP 状态码（0 表示网络层失败）
    std::string body;      ///< 响应体
    std::string error;     ///< 网络错误描述（status=0 时有效）
    std::map<std::string, std::string> headers;  ///< 小写化的响应头（content-range 等）
    bool ok() const { return status == 200; }
};

/**
 * @brief 通用同步 HTTP 请求（PUT/GET/POST 均可；调用方须在工作线程）。
 * @param extraHeaders 附加请求头（如 Range 断点续传）
 */
HttpResult httpRequest(const std::string& method, const std::string& host, unsigned short port,
                       const std::string& target, const std::string& body, int timeoutSec = 30,
                       const std::map<std::string, std::string>& extraHeaders = {});

/**
 * @brief 同步 POST JSON。
 * @param host    目标主机
 * @param port    目标端口
 * @param target  请求路径（如 /api/login）
 * @param body    JSON 请求体
 * @param timeoutSec 超时（秒）
 * @return HttpResult 响应
 */
HttpResult postJson(const std::string& host, unsigned short port, const std::string& target,
                    const std::string& body, int timeoutSec = 10);

} // namespace lingxi::client
