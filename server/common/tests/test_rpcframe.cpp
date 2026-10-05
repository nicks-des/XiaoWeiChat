/**
 * @file test_rpcframe.cpp
 * @brief RPC 帧编解码单元测试。
 */
#include <gtest/gtest.h>

#include "lingxi/rpc/RpcFrame.h"

namespace {

using namespace lingxi::rpc;

/**
 * @brief 编码-解码回环：头字段与 payload 无损。
 */
TEST(RpcFrameTest, EncodeDecodeRoundtrip) {
    const std::string payload = "\x0A\x05hello";  // 假 protobuf 字节
    const std::string body = encodeRpcBody(kServiceStatus, 0x04, 777, kRpcFlagNone, payload);

    ASSERT_EQ(body.size(), kRpcHeadSize + payload.size());

    RpcHead head;
    std::string decoded;
    ASSERT_TRUE(decodeRpcBody(body, head, decoded));
    EXPECT_EQ(head.serviceId, kServiceStatus);
    EXPECT_EQ(head.methodId, 0x04);
    EXPECT_EQ(head.requestId, 777u);
    EXPECT_EQ(head.flags, kRpcFlagNone);
    EXPECT_EQ(decoded, payload);
}

/**
 * @brief 不足 9 字节的帧体判定非法。
 */
TEST(RpcFrameTest, TruncatedBodyRejected) {
    RpcHead head;
    std::string payload;
    EXPECT_FALSE(decodeRpcBody(std::string(4, '\x00'), head, payload));
}

/**
 * @brief 标志位（响应/推送）可正确还原。
 */
TEST(RpcFrameTest, FlagsRoundtrip) {
    const std::string body = encodeRpcBody(kServiceChat, 0x01, 1, kRpcFlagPush, "");
    RpcHead head;
    std::string payload;
    ASSERT_TRUE(decodeRpcBody(body, head, payload));
    EXPECT_EQ(head.flags, kRpcFlagPush);
}

} // namespace
