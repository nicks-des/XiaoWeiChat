/**
 * @file SocialService.cpp
 * @brief 客户端社交服务实现（M3）。
 */
#include "SocialService.h"

#include "client/net/TcpClient.h"

#include "lingxi/logging/Logger.h"
#include "lingxi/net/Packet.h"

namespace lingxi::client {

SocialService& SocialService::instance() {
    static SocialService s_service;
    return s_service;
}

void SocialService::setup(TcpClient* tcp) {
    m_tcp = tcp;
}

template <typename Fn>
void SocialService::emitOnUi(Fn&& fn) {
    QMetaObject::invokeMethod(this, std::forward<Fn>(fn), Qt::QueuedConnection);
}

void SocialService::search(const std::string& keyword) {
    FriendSearchRequest request;
    request.set_keyword(keyword);
    m_tcp->send(0x0201, request.SerializeAsString());
}

void SocialService::apply(int64_t toUid, const std::string& verifyMsg) {
    FriendApplyRequest request;
    request.set_to_uid(toUid);
    request.set_verify_msg(verifyMsg);
    m_tcp->send(0x0202, request.SerializeAsString());
}

void SocialService::handleApply(int64_t applyId, bool agree) {
    FriendApplyHandleRequest request;
    request.set_apply_id(applyId);
    request.set_agree(agree);
    m_tcp->send(0x0204, request.SerializeAsString());
}

void SocialService::deleteFriend(int64_t friendUid) {
    FriendDeleteRequest request;
    request.set_friend_uid(friendUid);
    m_tcp->send(0x0206, request.SerializeAsString());
}

void SocialService::requestFriendList() {
    FriendListRequest request;
    m_tcp->send(0x0207, request.SerializeAsString());
}

void SocialService::createGroup(const std::string& name,
                                const std::vector<long long>& memberUids) {
    GroupCreateRequest request;
    request.set_name(name);
    for (long long uid : memberUids) {
        request.add_member_uids(uid);
    }
    m_tcp->send(0x0401, request.SerializeAsString());
}

void SocialService::inviteGroup(long long convId, const std::vector<long long>& memberUids) {
    GroupInviteRequest request;
    request.set_conv_id(convId);
    for (long long uid : memberUids) {
        request.add_member_uids(uid);
    }
    m_tcp->send(0x0402, request.SerializeAsString());
}

void SocialService::quitGroup(long long convId) {
    GroupQuitRequest request;
    request.set_conv_id(convId);
    m_tcp->send(0x0404, request.SerializeAsString());
}

void SocialService::dissolveGroup(long long convId) {
    GroupDissolveRequest request;
    request.set_conv_id(convId);
    m_tcp->send(0x0406, request.SerializeAsString());
}

void SocialService::onPacket(uint16_t msgId, const std::string& body) {
    switch (msgId) {
        case 0x0201: {
            FriendSearchResponse response;
            if (!response.ParseFromString(body)) {
                return;
            }
            emitOnUi([this, response] {
                m_searchResult = response;
                emit searchResultArrived();
            });
            break;
        }
        case 0x0203: {
            FriendApplyNotice notice;
            if (!notice.ParseFromString(body)) {
                return;
            }
            emitOnUi([this, notice] {
                m_pendingApplies.push_back(notice);
                emit applyReceived(QString::fromStdString(notice.from_nickname()),
                                   QString::fromStdString(notice.verify_msg()));
                requestFriendList();  // 刷新红点
            });
            break;
        }
        case 0x0205: {
            FriendApplyResultNotice notice;
            if (!notice.ParseFromString(body)) {
                return;
            }
            emitOnUi([this, notice] {
                // 处理完成：从待处理列表移除
                for (auto it = m_pendingApplies.begin(); it != m_pendingApplies.end(); ++it) {
                    if (it->apply_id() == notice.apply_id()) {
                        m_pendingApplies.erase(it);
                        break;
                    }
                }
                emit applyResultReceived(notice.agree());
                requestFriendList();
            });
            break;
        }
        case 0x0207: {
            FriendListResponse response;
            if (!response.ParseFromString(body)) {
                return;
            }
            emitOnUi([this, response] {
                m_friends = response;
                emit friendListUpdated(response.pending_applies());
            });
            break;
        }
        case 0x0401: {
            GroupCreateResponse response;
            if (!response.ParseFromString(body)) {
                return;
            }
            emitOnUi([this, response] {
                if (response.err_code() != 0) {
                    emit socialError(QStringLiteral("建群失败(%1): %2")
                                         .arg(response.err_code())
                                         .arg(QString::fromStdString(response.err_msg())));
                }
            });
            break;
        }
        case 0x0402:
        case 0x0404:
        case 0x0405:
        case 0x0406: {
            GenericErr response;
            if (!response.ParseFromString(body)) {
                return;
            }
            emitOnUi([this, response] {
                if (response.err_code() != 0) {
                    emit socialError(QString::fromStdString(response.err_msg()));
                }
            });
            break;
        }
        default:
            break;
    }
}

} // namespace lingxi::client
