/**
 * @file PngCard.h
 * @brief SillyTavern 兼容角色卡 PNG 读写：tEXt chunk("chara", base64 JSON) 解析与嵌入。
 *
 * PNG 结构：签名(8B) + chunk[长度(4B) + 类型(4B) + 数据 + CRC(4B)]。
 * tEXt 数据 = keyword\0text（keyword="chara"，text=base64(JSON)，docs/04 §2.1）。
 */
#pragma once

#include <string>

#include "lingxi/base/NonCopyable.h"

namespace lingxi::tavern {

/**
 * @brief PNG 角色卡工具（全部静态方法）。
 */
class PngCard : private NonCopyable {
public:
    /**
     * @brief 从 PNG 字节流提取角色卡 JSON（tEXt "chara"，base64 解码）。
     * @param png    PNG 文件字节
     * @param outJson 输出：角色卡 JSON 字符串
     * @return bool 提取成功为 true
     */
    static bool extractCardJson(const std::string& png, std::string& outJson);

    /**
     * @brief 把角色卡 JSON 嵌入 PNG（新增/替换 tEXt "chara" chunk，其余 chunk 原样保留）。
     * @param png    原始 PNG 字节
     * @param cardJson 角色卡 JSON 字符串
     * @param outPng 输出：嵌入后的 PNG 字节
     * @return bool 成功为 true
     */
    static bool embedCardJson(const std::string& png, const std::string& cardJson,
                              std::string& outPng);

    /**
     * @brief 是否包含角色卡。
     */
    static bool hasCard(const std::string& png);

    /**
     * @brief base64 编码/解码（OpenSSL EVP 实现）。
     */
    static std::string base64Encode(const std::string& raw);
    static std::string base64Decode(const std::string& encoded);
};

} // namespace lingxi::tavern
