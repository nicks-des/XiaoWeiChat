/**
 * @file Crypto.cpp
 * @brief 加密工具实现（OpenSSL EVP / HMAC）。
 */
#include "lingxi/crypto/Crypto.h"

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace lingxi::crypto {

namespace {

/**
 * @brief 字节串转十六进制。
 */
std::string toHex(const unsigned char* data, size_t len) {
    static const char kDigits[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        hex.push_back(kDigits[data[i] >> 4]);
        hex.push_back(kDigits[data[i] & 0x0F]);
    }
    return hex;
}

/**
 * @brief 计算原始 SHA256 摘要。
 */
std::vector<unsigned char> sha256Raw(const std::string& data) {
    std::vector<unsigned char> digest(EVP_MAX_MD_SIZE);
    unsigned int digestLen = 0;
    if (EVP_Digest(data.data(), data.size(), digest.data(), &digestLen, EVP_sha256(),
                   nullptr) != 1) {
        throw std::runtime_error("EVP_Digest(SHA256) failed");
    }
    digest.resize(digestLen);
    return digest;
}

/**
 * @brief 常量时间比较两个等长十六进制串（长度不同直接返回 false）。
 */
bool constantTimeEquals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) {
        return false;
    }
    volatile unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
    }
    return diff == 0;
}

} // namespace

std::string generateSaltHex(int bytes) {
    std::vector<unsigned char> buffer(static_cast<size_t>(bytes));
    if (RAND_bytes(buffer.data(), bytes) != 1) {
        throw std::runtime_error("RAND_bytes failed");
    }
    return toHex(buffer.data(), buffer.size());
}

std::string hashPassword(const std::string& saltHex, const std::string& password) {
    // 存储结构：SHA256(salt || password)，盐前置保证不同盐同密码哈希不同
    return toHex(sha256Raw(saltHex + password).data(), 32);
}

bool verifyPassword(const std::string& saltHex, const std::string& password,
                    const std::string& expectedHashHex) {
    return constantTimeEquals(hashPassword(saltHex, password), expectedHashHex);
}

std::string hmacSha256Hex(const std::string& secretKey, const std::string& data) {
    unsigned char digest[EVP_MAX_MD_SIZE] = {0};
    unsigned int digestLen = 0;
    if (HMAC(EVP_sha256(), secretKey.data(), static_cast<int>(secretKey.size()),
             reinterpret_cast<const unsigned char*>(data.data()), data.size(), digest,
             &digestLen) == nullptr) {
        throw std::runtime_error("HMAC(SHA256) failed");
    }
    return toHex(digest, digestLen);
}

} // namespace lingxi::crypto
