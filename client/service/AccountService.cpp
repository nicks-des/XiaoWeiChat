/**
 * @file AccountService.cpp
 * @brief 账号业务服务实现。
 */
#include "AccountService.h"

#include <QTimer>

#include <nlohmann/json.hpp>

#include "lingxi/config/Config.h"
#include "lingxi/logging/Logger.h"
#include "lingxi/net/Packet.h"

#include "client/net/HttpManager.h"
#include "client/service/ConversationService.h"

#include "lingxi.pb.h"

namespace lingxi::client {

namespace {

/** TCP 登录超时（毫秒） */
constexpr int kTcpLoginTimeoutMs = 5000;

/** 顶号通知 msgId */
constexpr uint16_t kMsgIdKick = 0x0104;

/** 登录响应 msgId */
constexpr uint16_t kMsgIdLoginResponse = 0x0102;

} // namespace

AccountService& AccountService::instance() {
    static AccountService s_service;
    return s_service;
}

bool AccountService::setup() {
    auto& config = Config::instance();
    if (!config.load("config/dev.json") && !config.load("../../config/dev.json") &&
        !config.load("../../../config/dev.json")) {
        return false;
    }
    m_gateHost = config.get<std::string>("gateserver.host", "127.0.0.1");
    m_gatePort = static_cast<unsigned short>(config.get<int>("gateserver.httpPort", 8080));
    m_tcp = std::make_unique<TcpClient>();

    m_tcp->setCallbacks(
        [this](uint16_t msgId, const std::string& body) { onPacket(msgId, body); },
        [this](bool connected, const std::string& err) {
            if (connected) {
                // 连接建立即发送长连接登录帧（docs/01 §5.1）
                LoginRequest request;
                request.set_uid(m_uid);
                request.set_token(m_token);
                request.set_device_id("pc");
                m_tcp->send(0x0101, request.SerializeAsString());
            }
            emitOnUi([this, connected, err] {
                if (!connected) {
                    emit connectionLost(QString::fromStdString(err));
                } else {
                    emit tcpConnected();
                }
            });
        });
    LX_LOG_INFO("AccountService setup done (gate {}:{})", m_gateHost, m_gatePort);
    return true;
}

template <typename Fn>
void AccountService::emitOnUi(Fn&& fn) {
    // 服务对象归属主线程：队列投递保证信号在 UI 线程发射
    QMetaObject::invokeMethod(this, std::forward<Fn>(fn), Qt::QueuedConnection);
}

void AccountService::registerAccount(const std::string& username, const std::string& password) {
    std::thread([this, username, password] {
        const HttpResult result =
            postJson(m_gateHost, m_gatePort, "/api/register",
                     "{\"username\":\"" + username + "\",\"password\":\"" + password + "\"}");
        std::string err = "network error: " + (result.error.empty() ? result.body : result.error);
        int code = static_cast<int>(result.status == 200 ? 0 : result.status);
        long long uid = 0;
        if (result.ok()) {
            try {
                const auto json = nlohmann::json::parse(result.body);
                code = json.value("err_code", -1);
                err = json.value("err_msg", "");
                uid = json.value("uid", 0LL);
            } catch (const nlohmann::json::exception& e) {
                code = 500;
                err = e.what();
            }
        }
        emitOnUi([this, code, err, uid] {
            emit registerFinished(code, QString::fromStdString(err), uid);
        });
    }).detach();  // M1：一次性短任务；M2 引入统一工作线程池后收敛
}

void AccountService::login(const std::string& username, const std::string& password) {
    std::thread([this, username, password] {
        const HttpResult result =
            postJson(m_gateHost, m_gatePort, "/api/login",
                     "{\"username\":\"" + username + "\",\"password\":\"" + password + "\"}");
        int code = static_cast<int>(result.status == 200 ? 0 : result.status);
        std::string err = "network error";
        std::string token;
        std::string chatHost;
        int chatPort = 0;
        if (result.ok()) {
            try {
                const auto json = nlohmann::json::parse(result.body);
                code = json.value("err_code", -1);
                err = json.value("err_msg", "");
                m_uid = json.value("uid", 0LL);
                token = json.value("token", "");
                chatHost = json.value("chat_host", "");
                chatPort = json.value("chat_port", 0);
            } catch (const nlohmann::json::exception& e) {
                code = 500;
                err = e.what();
            }
        }
        emitOnUi([this, code, err, uid = m_uid] { emit loginFinished(code, QString::fromStdString(err), uid); });
        if (code != 0) {
            return;
        }

        // 登录成功：按 uid 打开本地缓存库（M2）
        emitOnUi([this, uid = m_uid] {
            ConversationService::instance().setup(m_tcp.get(), std::to_string(uid));
        });

        // ---- TCP 长连接登录（IO 线程回调 → 切 UI 线程） ----
        m_tcpLoginDone = false;
        m_token = token;
        emitOnUi([this, chatHost, chatPort] {
            // 超时兜底定时器（UI 线程生命周期安全）
            QTimer::singleShot(kTcpLoginTimeoutMs, this, &AccountService::onTcpLoginTimeout);
            m_tcp->connectAsync(chatHost, static_cast<unsigned short>(chatPort));
        });
    }).detach();
}

void AccountService::onTcpLoginTimeout() {
    if (m_tcpLoginDone) {
        return;
    }
    m_tcpLoginDone = true;
    emit tcpLoginFinished(504, QStringLiteral("tcp login timeout"));
}

void AccountService::onPacket(uint16_t msgId, const std::string& body) {
    if (msgId == kMsgIdLoginResponse) {
        LoginResponse response;
        response.ParseFromString(body);
        const int code = response.err_code();
        const std::string msg = response.err_msg();
        emitOnUi([this, code, msg] {
            if (m_tcpLoginDone) {
                return;  // 超时兜底已触发
            }
            m_tcpLoginDone = true;
            emit tcpLoginFinished(code, QString::fromStdString(msg));
        });
        return;
    }
    if (msgId >= 0x0300 && msgId <= 0x030F) {
        ConversationService::instance().onPacket(msgId, body);  // M2 业务帧转发
        return;
    }
    if (msgId == kMsgIdKick) {
        KickNotice notice;
        notice.ParseFromString(body);
        const std::string info = notice.device_id() + "@" + notice.login_ip();
        emitOnUi([this, info] { emit kicked(QString::fromStdString(info)); });
        return;
    }
    // 其余消息类型随 M2 消息内核接入
}

void AccountService::logout() {
    m_uid = 0;
    m_token.clear();
    if (m_tcp) {
        m_tcp->close();
        m_tcp = std::make_unique<TcpClient>();  // 重建连接对象供下次登录
        m_tcp->setCallbacks(
            [this](uint16_t msgId, const std::string& body) { onPacket(msgId, body); },
            [this](bool connected, const std::string& err) {
                emitOnUi([this, connected, err] {
                    if (!connected) {
                        emit connectionLost(QString::fromStdString(err));
                    }
                });
            });
    }
}

} // namespace lingxi::client
