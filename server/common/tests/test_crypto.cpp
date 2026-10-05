/**
 * @file test_crypto.cpp
 * @brief 加密工具单元测试：盐/密码哈希/常量时间校验/HMAC。
 */
#include <gtest/gtest.h>

#include "lingxi/crypto/Crypto.h"

namespace {

/**
 * @brief 同密码不同盐产生不同哈希；同盐同密码结果一致。
 */
TEST(CryptoTest, PasswordHashDeterministicWithSalt) {
    const std::string saltA = lingxi::crypto::generateSaltHex();
    const std::string saltB = lingxi::crypto::generateSaltHex();

    const std::string hashA1 = lingxi::crypto::hashPassword(saltA, "Passw0rd!123");
    const std::string hashA2 = lingxi::crypto::hashPassword(saltA, "Passw0rd!123");
    const std::string hashB = lingxi::crypto::hashPassword(saltB, "Passw0rd!123");

    EXPECT_EQ(hashA1, hashA2);          // 同盐同密码 → 一致
    EXPECT_NE(hashA1, hashB);           // 不同盐 → 不同
    EXPECT_EQ(hashA1.size(), 64u);      // SHA256 hex = 64 字符
}

/**
 * @brief 校验通过/失败路径。
 */
TEST(CryptoTest, VerifyPassword) {
    const std::string salt = lingxi::crypto::generateSaltHex();
    const std::string hash = lingxi::crypto::hashPassword(salt, "correct-horse");

    EXPECT_TRUE(lingxi::crypto::verifyPassword(salt, "correct-horse", hash));
    EXPECT_FALSE(lingxi::crypto::verifyPassword(salt, "wrong-battery", hash));
}

/**
 * @brief HMAC-SHA256 已知向量（RFC 4231 Test Case 2：key="Jefe"）。
 */
TEST(CryptoTest, HmacKnownVector) {
    const std::string signature =
        lingxi::crypto::hmacSha256Hex("Jefe", "what do ya want for nothing?");
    EXPECT_EQ(signature,
              "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
}

/**
 * @brief 盐值长度正确且随机。
 */
TEST(CryptoTest, SaltLengthAndRandomness) {
    const std::string salt1 = lingxi::crypto::generateSaltHex();
    const std::string salt2 = lingxi::crypto::generateSaltHex();
    EXPECT_EQ(salt1.size(), 32u);  // 16 字节 → 32 hex
    EXPECT_NE(salt1, salt2);
}

} // namespace
