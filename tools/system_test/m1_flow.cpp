/**
 * @file m1_flow.cpp
 * @brief T10-09 M1 全链路联调：HTTP 注册 → 登录（token+分配）→ TCP 登录校验 → 顶号 → 异常路径。
 *
 * 前置：gateserver(8080) / statusserver(9000) / chatserver(8888) 已启动，MySQL/Redis 就绪。
 * 输出 M1_FLOW_PASS 并 exit 0 表示全链路通过。
 */
#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <chrono>
#include <functional>
#include <iostream>
#include <string>

#include "lingxi/net/BufReader.h"
#include "lingxi/net/Packet.h"

#include "lingxi.pb.h"

namespace beast = boost::beast;
namespace http = beast::http;
namespace asio = boost::asio;
using asio::ip::tcp;

namespace {

/** 服务地址（与 config/dev.json 一致） */
constexpr const char* kGateHost = "127.0.0.1";
constexpr unsigned short kGatePort = 8080;
constexpr const char* kChatHost = "127.0.0.1";
constexpr unsigned short kChatPort = 8888;

/** 检查宏：失败打印并退出 1 */
#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            std::cout << "M1_FLOW_FAIL: " << (msg) << std::endl;            \
            std::exit(1);                                                   \
        }                                                                   \
    } while (0)

/**
 * @brief 同步 HTTP POST JSON 到 Gate。
 * @return std::string 响应 body
 */
std::string httpPost(const std::string& target, const std::string& body) {
    asio::io_context io;
    tcp::socket socket(io);
    socket.connect({asio::ip::make_address(kGateHost), kGatePort});

    http::request<http::string_body> request{http::verb::post, target, 11};
    request.set(http::field::host, kGateHost);
    request.set(http::field::content_type, "application/json");
    request.body() = body;
    request.prepare_payload();
    http::write(socket, request);

    beast::flat_buffer buffer;
    http::response<http::string_body> response;
    http::read(socket, buffer, response);

    boost::system::error_code ec;
    socket.shutdown(tcp::socket::shutdown_both, ec);
    return response.body();
}

/**
 * @brief 从 JSON 字符串中提取简单字符串字段（M1 联调专用，不引 JSON 库）。
 */
std::string extractString(const std::string& json, const std::string& key) {
    const std::string pattern = "\"" + key + "\":\"";
    const auto start = json.find(pattern);
    if (start == std::string::npos) {
        return "";
    }
    const auto valueStart = start + pattern.size();
    const auto valueEnd = json.find('"', valueStart);
    return json.substr(valueStart, valueEnd - valueStart);
}

/**
 * @brief 从 JSON 中提取整数字段。
 */
long long extractInt(const std::string& json, const std::string& key) {
    const std::string pattern = "\"" + key + "\":";
    const auto start = json.find(pattern);
    if (start == std::string::npos) {
        return -1;
    }
    return std::stoll(json.substr(start + pattern.size()));
}

/**
 * @brief 便捷值类型：一条已建立的 TCP 连接。
 */
struct ClientConn {
    asio::io_context io;
    tcp::socket socket{io};

    /**
     * @brief 连接 ChatServer。
     */
    void connect() { socket.connect({asio::ip::make_address(kChatHost), kChatPort}); }

    /**
     * @brief 发送一帧。
     */
    void send(uint16_t msgId, const std::string& body) {
        asio::write(socket, asio::buffer(lingxi::net::encodePacket(msgId, 0, body)));
    }

    /**
     * @brief 带超时读取一帧。
     * @return bool 收到帧为 true（输出 packet）；超时/断开为 false
     */
    bool readWithTimeout(lingxi::net::DecodedPacket& packet, int timeoutMs) {
        // asio 坑：io_context 跑空后处于 stopped 态，复用前必须 restart
        io.restart();
        bool gotPacket = false;
        bool stop = false;
        lingxi::net::BufReader reader;
        char buffer[4096] = {0};

        asio::steady_timer timer(io);
        timer.expires_after(std::chrono::milliseconds(timeoutMs));
        timer.async_wait([&](const boost::system::error_code& ec) {
            if (!ec && !stop) {  // 超时
                boost::system::error_code ignored;
                socket.close(ignored);
                stop = true;
            }
        });

        std::function<void()> readLoop = [&]() {
            socket.async_read_some(
                asio::buffer(buffer),
                [&](const boost::system::error_code& ec, std::size_t length) {
                    if (ec) {
                        std::cout << "[probe] read error: " << ec.message() << std::endl;
                        stop = true;
                        timer.cancel();
                        return;
                    }
                    std::cout << "[probe] read " << length << " bytes" << std::endl;
                    reader.feed(buffer, length);
                    if (reader.tryExtractPacket(packet)) {
                        gotPacket = true;
                        stop = true;
                        timer.cancel();
                        return;
                    }
                    readLoop();
                });
        };
        readLoop();
        io.run();
        std::cout << "[probe] readWithTimeout exit: gotPacket=" << gotPacket << std::endl;
        return gotPacket;
    }
};

} // namespace

/**
 * @brief 全链路主流程。
 * @return int 0 成功
 */
int main() {
    const std::string username = "m1user" + std::to_string(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count() % 100000000);
    const std::string password = "Passw0rd!123";

    /* ---- 1. 注册 ---- */
    const std::string registerRsp = httpPost("/api/register",
                                             "{\"username\":\"" + username +
                                                 "\",\"password\":\"" + password + "\"}");
    std::cout << "[1] register: " << registerRsp << std::endl;
    CHECK(extractInt(registerRsp, "err_code") == 0, "register should succeed");
    const long long uid = extractInt(registerRsp, "uid");
    CHECK(uid > 0, "uid should be positive");

    /* ---- 2. 重复用户名被拒 ---- */
    const std::string dupRsp = httpPost("/api/register",
                                        "{\"username\":\"" + username +
                                            "\",\"password\":\"" + password + "\"}");
    CHECK(extractInt(dupRsp, "err_code") == 409, "duplicate username should be 409");

    /* ---- 3. 登录（错误密码被拒） ---- */
    const std::string badLoginRsp = httpPost("/api/login",
                                             "{\"username\":\"" + username +
                                                 "\",\"password\":\"WrongPass!\"}");
    CHECK(extractInt(badLoginRsp, "err_code") == 401, "wrong password should be 401");

    /* ---- 4. 登录成功：token + ChatServer 分配 ---- */
    const std::string loginRsp = httpPost("/api/login",
                                          "{\"username\":\"" + username +
                                              "\",\"password\":\"" + password + "\"}");
    std::cout << "[4] login: " << loginRsp << std::endl;
    CHECK(extractInt(loginRsp, "err_code") == 0, "login should succeed");
    const std::string token = extractString(loginRsp, "token");
    CHECK(!token.empty(), "token should not be empty");
    CHECK(extractString(loginRsp, "chat_host") == kChatHost, "chat host should match");
    CHECK(extractInt(loginRsp, "chat_port") == kChatPort, "chat port should match");

    /* ---- 5. TCP 登录：合法 token ---- */
    ClientConn connA;
    connA.connect();
    lingxi::LoginRequest loginReq;
    loginReq.set_uid(uid);
    loginReq.set_token(token);
    loginReq.set_device_id("device-A");
    connA.send(0x0101, loginReq.SerializeAsString());

    lingxi::net::DecodedPacket loginAck;
    CHECK(connA.readWithTimeout(loginAck, 5000), "login ack timeout");
    CHECK(loginAck.header.msgId == 0x0102, "expect LoginResponse");
    lingxi::LoginResponse loginRspPb;
    CHECK(loginRspPb.ParseFromString(loginAck.body), "LoginResponse parse");
    CHECK(loginRspPb.err_code() == 0, "tcp login should succeed");
    std::cout << "[5] tcp login ok (uid=" << uid << ")" << std::endl;

    /* ---- 6. 顶号：同 uid 第二处登录，第一处收 KickNotice ---- */
    ClientConn connB;
    connB.connect();
    loginReq.set_device_id("device-B");
    connB.send(0x0101, loginReq.SerializeAsString());
    lingxi::net::DecodedPacket loginAckB;
    CHECK(connB.readWithTimeout(loginAckB, 5000), "second login ack timeout");
    CHECK(loginRspPb.ParseFromString(loginAckB.body) && loginRspPb.err_code() == 0,
          "second tcp login should succeed");

    lingxi::net::DecodedPacket kickFrame;
    CHECK(connA.readWithTimeout(kickFrame, 5000), "kick notice timeout on first conn");
    CHECK(kickFrame.header.msgId == 0x0104, "expect KickNotice");
    std::cout << "[6] kick notice received on first connection" << std::endl;

    /* ---- 7. 异常路径：伪造 token 被拒 ---- */
    ClientConn connC;
    connC.connect();
    loginReq.set_uid(uid);
    loginReq.set_token(token.substr(0, token.size() - 2) + "ff");  // 篡改签名
    connC.send(0x0101, loginReq.SerializeAsString());
    lingxi::net::DecodedPacket rejectAck;
    CHECK(connC.readWithTimeout(rejectAck, 5000), "reject ack timeout");
    lingxi::LoginResponse rejectRsp;
    CHECK(rejectRsp.ParseFromString(rejectAck.body) && rejectRsp.err_code() == 401,
          "forged token should be 401");
    std::cout << "[7] forged token rejected" << std::endl;

    std::cout << "M1_FLOW_PASS" << std::endl;
    return 0;
}
