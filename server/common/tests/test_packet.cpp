/**
 * @file test_packet.cpp
 * @brief 传输帧与 BufReader 单元测试：编码回环、粘包、半包、多包、脏数据处理。
 */
#include <gtest/gtest.h>

#include <string>

#include "lingxi/net/BufReader.h"
#include "lingxi/net/Packet.h"

namespace {

using namespace lingxi::net;

/**
 * @brief 编码后可直接通过 parseHeader 还原帧头字段。
 */
TEST(PacketTest, EncodeParseRoundtrip) {
    const std::string body = "\x01\x02\x03hello";
    const std::string frame = encodePacket(0x0301, 42, body, kFlagPush);

    ASSERT_EQ(frame.size(), kHeaderSize + body.size());

    PacketHeader header;
    ASSERT_TRUE(parseHeader(frame.data(), frame.size(), header));
    EXPECT_EQ(header.totalLength, kHeaderSize + body.size());
    EXPECT_EQ(header.msgId, 0x0301);
    EXPECT_EQ(header.version, kVersion);
    EXPECT_EQ(header.flags, kFlagPush);
    EXPECT_EQ(header.pktSeq, 42u);
}

/**
 * @brief 完整帧一次性喂入可立即提取。
 */
TEST(BufReaderTest, WholePacketInOneFeed) {
    const std::string frame = encodePacket(0x0000, 1, "ping");
    BufReader reader;
    reader.feed(frame.data(), frame.size());

    DecodedPacket packet;
    ASSERT_TRUE(reader.tryExtractPacket(packet));
    EXPECT_EQ(packet.header.msgId, 0x0000);
    EXPECT_EQ(packet.body, "ping");
    EXPECT_TRUE(reader.empty());
}

/**
 * @brief 半包：逐字节喂入，凑齐前不产出，凑齐后正确提取。
 */
TEST(BufReaderTest, ByteByByteFeed) {
    const std::string frame = encodePacket(0x0101, 7, "login-body");
    BufReader reader;
    DecodedPacket packet;

    for (size_t i = 0; i + 1 < frame.size(); ++i) {
        reader.feed(frame.data() + i, 1);
        EXPECT_FALSE(reader.tryExtractPacket(packet)) << "在第 " << i << " 字节不应成包";
    }
    reader.feed(frame.data() + frame.size() - 1, 1);
    ASSERT_TRUE(reader.tryExtractPacket(packet));
    EXPECT_EQ(packet.body, "login-body");
}

/**
 * @brief 粘包：两帧连发一次喂入，可连续提取两帧。
 */
TEST(BufReaderTest, TwoPacketsInOneFeed) {
    const std::string first = encodePacket(0x0301, 1, "msg-1");
    const std::string second = encodePacket(0x0302, 2, "ack-2");
    std::string glued = first + second;

    BufReader reader;
    reader.feed(glued.data(), glued.size());

    DecodedPacket packet;
    ASSERT_TRUE(reader.tryExtractPacket(packet));
    EXPECT_EQ(packet.body, "msg-1");
    ASSERT_TRUE(reader.tryExtractPacket(packet));
    EXPECT_EQ(packet.body, "ack-2");
    EXPECT_TRUE(reader.empty());
}

/**
 * @brief 半包+粘包混合：1.5 帧喂入，先取一帧，剩余等待补齐。
 */
TEST(BufReaderTest, HalfPlusWholeMixed) {
    const std::string first = encodePacket(0x0301, 1, "aaa");
    const std::string second = encodePacket(0x0301, 2, "bbb");
    const std::string half = second.substr(0, 5);

    BufReader reader;
    reader.feed(first.data(), first.size());
    reader.feed(half.data(), half.size());

    DecodedPacket packet;
    ASSERT_TRUE(reader.tryExtractPacket(packet));
    EXPECT_EQ(packet.body, "aaa");
    EXPECT_FALSE(reader.tryExtractPacket(packet));  // 第二帧不完整

    reader.feed(second.data() + 5, second.size() - 5);
    ASSERT_TRUE(reader.tryExtractPacket(packet));
    EXPECT_EQ(packet.body, "bbb");
}

/**
 * @brief 脏数据：长度字段越界时丢弃缓冲并返回 false。
 */
TEST(BufReaderTest, GarbageLengthDropped) {
    BufReader reader;
    std::string garbage = "\xFF\xFF\xFF\xFF\x00\x00";  // totalLength 越界
    reader.feed(garbage.data(), garbage.size());

    DecodedPacket packet;
    EXPECT_FALSE(reader.tryExtractPacket(packet));
    EXPECT_TRUE(reader.empty());  // 已丢弃
}

/**
 * @brief 长度不足帧头时不误判脏数据。
 */
TEST(BufReaderTest, ShortHeaderWaits) {
    BufReader reader;
    reader.feed("\x00\x00", 2);

    DecodedPacket packet;
    EXPECT_FALSE(reader.tryExtractPacket(packet));
    EXPECT_EQ(reader.size(), 2u);
}

} // namespace
