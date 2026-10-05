/**
 * @file ConversationService.cpp
 * @brief 会话业务服务实现（M2）。
 */
#include "ConversationService.h"

#include "client/net/TcpClient.h"

#include <QTimer>

#include <algorithm>
#include <set>

#include <nlohmann/json.hpp>

#include "lingxi/base/TimeUtil.h"
#include "lingxi/base/Uuid.h"
#include "lingxi/logging/Logger.h"
#include "lingxi/net/Packet.h"

#include "lingxi.pb.h"

namespace lingxi::client {

ConversationService& ConversationService::instance() {
    static ConversationService s_service;
    return s_service;
}

bool ConversationService::setup(TcpClient* tcp, const std::string& uidKey) {
    m_tcp = tcp;
    m_uidKey = uidKey;
    return m_store.open("lingxi_client_" + uidKey + ".db");
}

template <typename Fn>
void ConversationService::emitOnUi(Fn&& fn) {
    QMetaObject::invokeMethod(this, std::forward<Fn>(fn), Qt::QueuedConnection);
}

void ConversationService::requestConversationList() {
    ConversationListRequest request;
    m_tcp->send(0x030A, request.SerializeAsString());
}

void ConversationService::requestSync() {
    SyncRequest request;
    for (const auto& conv : m_store.loadConversations()) {
        auto* cursor = request.add_cursors();
        cursor->set_conv_id(conv.convId);
        cursor->set_last_seq(m_store.maxSeq(conv.convId));  // 本地权威游标（02 R4）
    }
    m_tcp->send(0x0308, request.SerializeAsString());
}

std::string ConversationService::sendText(int64_t toUid, const std::string& text) {
    const std::string clientMsgId = Uuid::generate();
    const std::string payload = nlohmann::json({{"text", text}}).dump();

    // 本地先落「发送中」（seq=0, status=-1），ACK 后回填（乐观 UI）
    LocalStore::MsgRow row;
    row.clientMsgId = clientMsgId;
    row.fromUid = -1;  // 发送者 uid 由上层已知；回执与漫游同步会覆盖
    row.msgType = MSG_TEXT;
    row.status = -1;
    row.payload = payload;
    row.sendTimeMs = TimeUtil::nowMs();
    m_store.insertMessage(row);

    MessageSendRequest request;
    auto* body = request.mutable_body();
    body->set_to_uid(toUid);
    body->set_client_msg_id(clientMsgId);
    body->set_msg_type(MSG_TEXT);
    body->set_send_time_ms(row.sendTimeMs);
    body->set_payload(payload);
    m_tcp->send(0x0301, request.SerializeAsString());
    return clientMsgId;
}

void ConversationService::markRead(int64_t convId) {
    const int64_t lastSeq = m_store.maxSeq(convId);
    if (lastSeq <= 0) {
        return;
    }
    m_store.markRead(convId, lastSeq);
    MessageReadAck ack;
    ack.set_conv_id(convId);
    ack.set_last_read_seq(lastSeq);
    m_tcp->send(0x0304, ack.SerializeAsString());
}

void ConversationService::recall(int64_t convId, int64_t msgId) {
    MessageRecallRequest request;
    request.set_conv_id(convId);
    request.set_msg_id(msgId);
    m_tcp->send(0x0306, request.SerializeAsString());
}

namespace {
/**
 * @brief 从消息 JSON 信封取预览文本（与 LocalStore 渲染共用）。
 */
std::string MessageServicePreview(const std::string& payloadJson) {
    try {
        auto parsed = nlohmann::json::parse(payloadJson);
        if (parsed.contains("text") && parsed["text"].is_string()) {
            std::string text = parsed["text"].get<std::string>();
            if (text.size() > 60) {
                text = text.substr(0, 60) + "…";
            }
            return text;
        }
    } catch (const nlohmann::json::exception&) {
    }
    return "[消息]";
}
} // namespace

void ConversationService::onPacket(uint16_t msgId, const std::string& body) {
    // IO 线程：只做解析，落库与信号在 UI 线程
    switch (msgId) {
        case 0x030A: {
            ConversationListResponse response;
            if (!response.ParseFromString(body)) {
                return;
            }
            emitOnUi([this, response] {
                for (const auto& info : response.conversations()) {
                    LocalStore::ConvRow row;
                    row.convId = info.conv_id();
                    row.type = info.type();
                    row.peerUid = info.peer_uid();
                    row.peerName = QString::fromUtf8(info.peer_nickname().c_str()).toStdString();
                    row.lastSeq = info.last_seq();
                    row.lastReadSeq = info.last_read_seq();
                    row.preview = info.last_preview();
                    row.lastMsgTimeMs = info.last_msg_time_ms();
                    m_store.upsertConversation(row);
                }
                emit conversationListUpdated();
                requestSync();  // 列表到位即差量补拉
            });
            break;
        }
        case 0x0309: {
            SyncResponse response;
            if (!response.ParseFromString(body)) {
                return;
            }
            emitOnUi([this, response] {
                std::set<long long> convIds;
                for (const auto& msg : response.messages()) {
                    LocalStore::MsgRow row;
                    row.convId = msg.conv_id();
                    row.seq = msg.conv_seq();
                    row.msgId = msg.msg_id();
                    row.clientMsgId = msg.client_msg_id();
                    row.fromUid = msg.from_uid();
                    row.msgType = msg.msg_type();
                    row.status = msg.status();
                    row.payload = msg.payload();
                    row.sendTimeMs = msg.send_time_ms();
                    m_store.insertMessage(row);
                    m_store.updateConversationTail(row.convId, row.seq,
                                                   MessageServicePreview(row.payload),
                                                   row.sendTimeMs);
                    convIds.insert(row.convId);
                }
                for (long long convId : convIds) {
                    emit messageArrived(convId);
                }
                if (response.has_more()) {
                    QTimer::singleShot(50, this, [this] { requestSync(); });  // 分页续拉
                } else {
                    emit syncCompleted();
                }
            });
            break;
        }
        case 0x0303: {
            MessageNotify notify;
            if (!notify.ParseFromString(body)) {
                return;
            }
            const MsgBody msg = notify.body();
            emitOnUi([this, msg] {
                LocalStore::MsgRow row;
                row.convId = msg.conv_id();
                row.seq = msg.conv_seq();
                row.msgId = msg.msg_id();
                row.clientMsgId = msg.client_msg_id();
                row.fromUid = msg.from_uid();
                row.msgType = msg.msg_type();
                row.status = msg.status();
                row.payload = msg.payload();
                row.sendTimeMs = msg.send_time_ms();
                m_store.insertMessage(row);
                m_store.updateConversationTail(row.convId, row.seq,
                                               MessageServicePreview(row.payload),
                                               row.sendTimeMs);
                emit messageArrived(row.convId);
            });
            break;
        }
        case 0x0302: {
            MessageAck ack;
            if (!ack.ParseFromString(body)) {
                return;
            }
            emitOnUi([this, ack] {
                m_store.confirmMessage(ack.client_msg_id(), ack.conv_id(), ack.conv_seq(),
                                       ack.send_time_ms());
                emit messageAcked(QString::fromStdString(ack.client_msg_id()), ack.conv_id(),
                                  ack.conv_seq());
            });
            break;
        }
        case 0x0305: {
            MessageReadNotice notice;
            if (!notice.ParseFromString(body)) {
                return;
            }
            emitOnUi([this, notice] { emit peerRead(notice.conv_id(), notice.last_read_seq()); });
            break;
        }
        case 0x0307: {
            MessageRecallNotice notice;
            if (!notice.ParseFromString(body)) {
                return;
            }
            emitOnUi([this, notice] {
                m_store.markRecalled(notice.conv_id(), notice.msg_id());
                emit messageRecalled(notice.conv_id(), notice.msg_id());
            });
            break;
        }
        default:
            break;
    }
}


} // namespace lingxi::client
