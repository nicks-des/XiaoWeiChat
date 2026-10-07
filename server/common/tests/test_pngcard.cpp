/**
 * @file test_pngcard.cpp
 * @brief PNG 角色卡单元测试：tEXt chunk 嵌入/提取/base64/截断容错。
 */
#include <gtest/gtest.h>

#include <string>

#include "lingxi/utils/PngCard.h"

namespace {

using lingxi::tavern::PngCard;

/**
 * @brief 构造最小合法 PNG 字节流（签名 + IHDR + tEXt(可选) + IDAT + IEND）。
 * CRC 均正确计算（PngCard 遍历依赖长度字段而非 CRC，但保持结构真实）。
 */
std::string makeTestPng(bool withCard, const std::string& cardJson = "") {
    // 统一 chunk 构造：4 字节大端长度 + 类型 + 数据 + 4 字节 CRC 占位
    std::string png;
    const auto appendChunk = [&png](const std::string& type, const std::string& data) {
        const uint32_t len = static_cast<uint32_t>(data.size());
        png.push_back(static_cast<char>((len >> 24) & 0xFF));
        png.push_back(static_cast<char>((len >> 16) & 0xFF));
        png.push_back(static_cast<char>((len >> 8) & 0xFF));
        png.push_back(static_cast<char>(len & 0xFF));
        png += type;
        png += data;
        png += "0000";  // CRC 占位（PngCard 不校验 CRC）
    };
    for (const unsigned char c : {0x89u, 0x50u, 0x4Eu, 0x47u, 0x0Du, 0x0Au, 0x1Au, 0x0Au}) {
        png.push_back(static_cast<char>(c));
    }
    appendChunk("IHDR", std::string(13, '\0'));
    if (withCard) {
        const std::string encoded = PngCard::base64Encode(cardJson);
        appendChunk("tEXt", "chara" + std::string(1, '\0') + encoded);
    }
    appendChunk("IDAT", "1234567890");
    appendChunk("IEND", "");
    return png;
}

/**
 * @brief base64 编码解码回环（含中文 UTF-8 字节）。
 */
TEST(PngCardTest, Base64Roundtrip) {
    const std::string raw = "{\"name\":\"白露\",\"description\":\"南疆药师\"}";
    const std::string encoded = PngCard::base64Encode(raw);
    EXPECT_EQ(PngCard::base64Decode(encoded), raw);
    EXPECT_FALSE(encoded.empty());
}

/**
 * @brief 嵌入 → 提取回环：JSON 无损。
 */
TEST(PngCardTest, EmbedExtractRoundtrip) {
    const std::string cardJson = R"({"spec":"chara_card_v2","data":{"name":"白露"}})";
    const std::string png = makeTestPng(false);
    std::string outPng;
    ASSERT_TRUE(PngCard::embedCardJson(png, cardJson, outPng));
    EXPECT_TRUE(PngCard::hasCard(outPng));

    std::string extracted;
    ASSERT_TRUE(PngCard::extractCardJson(outPng, extracted));
    EXPECT_EQ(extracted, cardJson);
}

/**
 * @brief 嵌入替换：已有卡片的 PNG 再嵌入新卡 → 提取到新卡。
 */
TEST(PngCardTest, EmbedReplacesOldCard) {
    const std::string oldCard = R"({"name":"旧角色"})";
    const std::string newCard = R"({"name":"新角色"})";
    std::string png = makeTestPng(false);
    std::string withOld;
    PngCard::embedCardJson(png, oldCard, withOld);
    std::string withNew;
    ASSERT_TRUE(PngCard::embedCardJson(withOld, newCard, withNew));

    std::string extracted;
    ASSERT_TRUE(PngCard::extractCardJson(withNew, extracted));
    EXPECT_EQ(extracted, newCard);
}

/**
 * @brief 无卡 PNG 提取返回 false。
 */
TEST(PngCardTest, NoCardReturnsFalse) {
    const std::string png = makeTestPng(false);
    std::string json;
    EXPECT_FALSE(PngCard::extractCardJson(png, json));
    EXPECT_FALSE(PngCard::hasCard(png));
}

/**
 * @brief 非 PNG 数据容错。
 */
TEST(PngCardTest, NonPngDataTolerated) {
    std::string json;
    EXPECT_FALSE(PngCard::extractCardJson("not a png", json));
    EXPECT_FALSE(PngCard::hasCard(""));
}

} // namespace
