/**
 * @file test_proto.cpp
 * @brief Protobuf 协议库单元测试：序列化回环与字段兼容性。
 */
#include <gtest/gtest.h>

#include <string>

#include "lingxi.pb.h"

namespace {

/**
 * @brief 消息体序列化-反序列化回环无损。
 */
TEST(ProtoTest, MsgBodyRoundtrip) {
    lingxi::MsgBody original;
    original.set_conv_id(10086);
    original.set_from_uid(1);
    original.set_conv_seq(7);
    original.set_client_msg_id("uuid-1234");
    original.set_msg_type(lingxi::MSG_TEXT);
    original.set_send_time_ms(1759650000123LL);
    original.set_payload("{\"text\":\"你好\"}");
    original.set_status(0);

    const std::string bytes = original.SerializeAsString();
    ASSERT_FALSE(bytes.empty());

    lingxi::MsgBody parsed;
    ASSERT_TRUE(parsed.ParseFromString(bytes));
    EXPECT_EQ(parsed.conv_id(), 10086);
    EXPECT_EQ(parsed.from_uid(), 1);
    EXPECT_EQ(parsed.conv_seq(), 7);
    EXPECT_EQ(parsed.client_msg_id(), "uuid-1234");
    EXPECT_EQ(parsed.msg_type(), lingxi::MSG_TEXT);
    EXPECT_EQ(parsed.payload(), "{\"text\":\"你好\"}");
}

/**
 * @brief 枚举与嵌套消息（SyncRequest/ConvCursor）往返正确。
 */
TEST(ProtoTest, SyncRequestRoundtrip) {
    lingxi::SyncRequest request;
    auto* cursorA = request.add_cursors();
    cursorA->set_conv_id(1);
    cursorA->set_last_seq(99);
    auto* cursorB = request.add_cursors();
    cursorB->set_conv_id(2);
    cursorB->set_last_seq(100);

    lingxi::SyncRequest parsed;
    ASSERT_TRUE(parsed.ParseFromString(request.SerializeAsString()));
    ASSERT_EQ(parsed.cursors_size(), 2);
    EXPECT_EQ(parsed.cursors(0).conv_id(), 1);
    EXPECT_EQ(parsed.cursors(0).last_seq(), 99);
    EXPECT_EQ(parsed.cursors(1).last_seq(), 100);
}

/**
 * @brief 未知字段向前兼容：老消息带未知字段号，新解析器忽略不报错。
 */
TEST(ProtoTest, UnknownFieldTolerated) {
    // 手工构造：field 99（varint 类型）—— tag = (99 << 3) | 0 = 792 → varint 编码为 B0 06
    std::string bytes;
    bytes.push_back(static_cast<char>(0xB0));
    bytes.push_back(static_cast<char>(0x06));
    bytes.push_back('\x2A');  // varint 42

    lingxi::LoginRequest request;
    ASSERT_TRUE(request.ParseFromString(bytes));  // 未知字段不阻断解析
}

} // namespace
