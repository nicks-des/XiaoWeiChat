/**
 * @file Crypto.h
 * @brief 加密工具：盐值生成、SHA256 密码哈希、HMAC-SHA256 签名（OpenSSL EVP 实现）。
 */
#pragma once

#include <string>

namespace lingxi::crypto {

/**
 * @brief 生成随机盐值（十六进制）。
 * @param bytes 随机字节数（默认 16 → 32 个 hex 字符）
 * @return std::string 十六进制盐值
 */
std::string generateSaltHex(int bytes = 16);

/**
 * @brief 计算密码哈希：SHA256(salt + password) 的十六进制。
 * @param saltHex   十六进制盐值
 * @param password  明文密码
 * @return std::string 64 字符十六进制哈希
 */
std::string hashPassword(const std::string& saltHex, const std::string& password);

/**
 * @brief 校验密码：常量时间比较，防时序侧信道。
 */
bool verifyPassword(const std::string& saltHex, const std::string& password,
                    const std::string& expectedHashHex);

/**
 * @brief 计算 HMAC-SHA256 并输出十六进制（用于 token 签名，docs/02 §6）。
 * @param secretKey 共享密钥（仅 Gate/Status 持有）
 * @param data      签名内容
 * @return std::string 64 字符十六进制签名
 */
std::string hmacSha256Hex(const std::string& secretKey, const std::string& data);

} // namespace lingxi::crypto
