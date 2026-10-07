/**
 * @file PngCard.cpp
 * @brief PNG 角色卡读写实现（tEXt chunk 级操作，无需完整 PNG 解码库）。
 */
#include "lingxi/utils/PngCard.h"

#include <openssl/evp.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>

#include <cstring>
#include <stdexcept>
#include <vector>

#include "lingxi/logging/Logger.h"

namespace lingxi::tavern {

namespace {

/** PNG 文件签名 */
const unsigned char kPngSignature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};

/** 角色卡 tEXt 关键字（SillyTavern 约定） */
const std::string kCardKeyword = "chara";

/**
 * @brief 计算 PNG chunk CRC32（zlib 多项式，标准表驱动实现）。
 */
uint32_t crc32Of(const unsigned char* data, size_t len) {
    static uint32_t table[256];
    static bool tableReady = false;
    if (!tableReady) {
        for (uint32_t n = 0; n < 256; ++n) {
            uint32_t c = n;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            }
            table[n] = c;
        }
        tableReady = true;
    }
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

/**
 * @brief 4 字节大端写入。
 */
void writeU32(std::string& out, uint32_t value) {
    out.push_back(static_cast<char>((value >> 24) & 0xFF));
    out.push_back(static_cast<char>((value >> 16) & 0xFF));
    out.push_back(static_cast<char>((value >> 8) & 0xFF));
    out.push_back(static_cast<char>(value & 0xFF));
}

/**
 * @brief 4 字节大端读取。
 */
uint32_t readU32(const std::string& data, size_t offset) {
    if (offset + 4 > data.size()) {
        return 0;
    }
    return (static_cast<uint32_t>(static_cast<unsigned char>(data[offset])) << 24) |
           (static_cast<uint32_t>(static_cast<unsigned char>(data[offset + 1])) << 16) |
           (static_cast<uint32_t>(static_cast<unsigned char>(data[offset + 2])) << 8) |
           static_cast<uint32_t>(static_cast<unsigned char>(data[offset + 3]));
}

/**
 * @brief 组装一个完整 chunk（长度+类型+数据+CRC）。
 */
std::string makeChunk(const std::string& type, const std::string& data) {
    std::string chunk;
    writeU32(chunk, static_cast<uint32_t>(data.size()));
    chunk += type;
    chunk += data;
    const uint32_t crc = crc32Of(reinterpret_cast<const unsigned char*>(chunk.c_str() + 4),
                                 type.size() + data.size());
    writeU32(chunk, crc);
    return chunk;
}

} // namespace

bool PngCard::extractCardJson(const std::string& png, std::string& outJson) {
    if (png.size() < 8 || std::memcmp(png.data(), kPngSignature, 8) != 0) {
        return false;
    }
    // 遍历 chunk：跳过签名后 [长度4][类型4][数据N][CRC4]
    size_t offset = 8;
    while (offset + 12 <= png.size()) {
        const uint32_t length = readU32(png, offset);
        const std::string type = png.substr(offset + 4, 4);
        if (offset + 12 + length > png.size()) {
            return false;  // 截断的 chunk
        }
        if (type == "tEXt") {
            const std::string chunkData = png.substr(offset + 8, length);
            // tEXt = keyword\0text
            const auto nullPos = chunkData.find('\0');
            if (nullPos != std::string::npos && chunkData.substr(0, nullPos) == kCardKeyword) {
                outJson = base64Decode(chunkData.substr(nullPos + 1));
                return !outJson.empty();
            }
        }
        offset += 12 + length;
    }
    return false;
}

bool PngCard::embedCardJson(const std::string& png, const std::string& cardJson,
                            std::string& outPng) {
    if (png.size() < 8 || std::memcmp(png.data(), kPngSignature, 8) != 0) {
        return false;
    }
    const std::string encoded = base64Encode(cardJson);
    // SillyTavern tEXt 数据 = keyword\0base64text
    const std::string textData = kCardKeyword + '\0' + encoded;
    const std::string cardChunk = makeChunk("tEXt", textData);

    // 遍历 chunk：跳过已有 chara tEXt，其余原样保留；cardChunk 插在 IHDR 之后
    outPng.assign(png.substr(0, 8));  // 签名
    size_t offset = 8;
    bool cardInserted = false;
    while (offset + 12 <= png.size()) {
        const uint32_t length = readU32(png, offset);
        const std::string type = png.substr(offset + 4, 4);
        if (offset + 12 + length > png.size()) {
            return false;
        }
        const size_t chunkTotal = 12 + length;
        const std::string chunkRaw = png.substr(offset, chunkTotal);
        const bool isCharaText = (type == "tEXt") &&
                                 chunkRaw.find(kCardKeyword + std::string(1, '\0')) != std::string::npos;
        if (type == "IHDR") {
            outPng += chunkRaw;
            outPng += cardChunk;  // 角色卡紧跟 IHDR
            cardInserted = true;
        } else if (!isCharaText) {
            outPng += chunkRaw;  // 旧 chara chunk 跳过（由新卡替换）
        }
        offset += chunkTotal;
    }
    return cardInserted;
}

bool PngCard::hasCard(const std::string& png) {
    std::string json;
    return extractCardJson(png, json);
}

std::string PngCard::base64Encode(const std::string& raw) {
    if (raw.empty()) {
        return {};
    }
    BIO* bio = BIO_new(BIO_s_mem());
    BIO* b64 = BIO_new(BIO_f_base64());
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);  // 无换行（SillyTavern 约定）
    bio = BIO_push(b64, bio);
    if (BIO_write(bio, raw.data(), static_cast<int>(raw.size())) != static_cast<int>(raw.size())) {
        BIO_free_all(bio);
        return {};
    }
    BIO_flush(bio);
    BUF_MEM* mem = nullptr;
    BIO_get_mem_ptr(bio, &mem);
    std::string encoded(mem->data, mem->length);
    BIO_free_all(bio);
    return encoded;
}

std::string PngCard::base64Decode(const std::string& encoded) {
    if (encoded.empty()) {
        return {};
    }
    BIO* bio = BIO_new_mem_buf(encoded.data(), static_cast<int>(encoded.size()));
    BIO* b64 = BIO_new(BIO_f_base64());
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    bio = BIO_push(b64, bio);
    std::vector<char> buffer(encoded.size());
    const int len = BIO_read(bio, buffer.data(), static_cast<int>(buffer.size()));
    BIO_free_all(bio);
    if (len <= 0) {
        return {};
    }
    return std::string(buffer.data(), static_cast<size_t>(len));
}

} // namespace lingxi::tavern
