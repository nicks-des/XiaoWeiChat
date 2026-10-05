/**
 * @file Config.h
 * @brief 配置模块：JSON 主配置 + *.local.json 深合并覆盖，点路径取值。
 *
 * 约定：config/<server>.json 入库；敏感信息（如 LLM Key）写 <server>.local.json（gitignore）。
 */
#pragma once

#include <nlohmann/json.hpp>
#include <shared_mutex>
#include <string>

#include "lingxi/base/NonCopyable.h"

namespace lingxi {

/**
 * @brief 进程级配置单例（线程安全）。
 */
class Config : private NonCopyable {
public:
    /**
     * @brief 获取全局配置实例。
     */
    static Config& instance();

    /**
     * @brief 加载主配置文件（整体替换当前内容）。
     * @param path JSON 文件路径
     * @return bool 文件存在且 JSON 合法时为 true
     */
    bool load(const std::string& path);

    /**
     * @brief 加载覆盖配置并与当前内容深合并（local 文件优先级更高）。
     * @param path JSON 文件路径（不存在时静默忽略，返回 false）
     * @return bool 是否实际加载合并
     */
    bool loadOverride(const std::string& path);

    /**
     * @brief 点路径取值。
     * @tparam T       期望类型（int/double/bool/string/嵌套 json 均可）
     * @param dottedKey 点分路径，如 "mysql.port"
     * @param defaultValue 键不存在或类型不匹配时的返回值
     * @return T 配置值或默认值
     */
    template <typename T>
    T get(const std::string& dottedKey, const T& defaultValue) const {
        std::shared_lock<std::shared_mutex> lock(m_mutex);
        const nlohmann::json* node = findNode(dottedKey);
        if (node == nullptr) {
            return defaultValue;
        }
        try {
            return node->get<T>();
        } catch (const nlohmann::json::exception&) {
            return defaultValue;  // 类型不匹配：回退默认值并容忍（由调用方日志兜底）
        }
    }

    /**
     * @brief 判断点路径是否存在。
     * @param dottedKey 点分路径
     * @return bool 存在为 true
     */
    bool contains(const std::string& dottedKey) const;

private:
    Config() = default;

    /**
     * @brief 按点路径查找 JSON 节点。
     * @param dottedKey 点分路径
     * @return const nlohmann::json* 命中的节点指针；不存在返回 nullptr
     */
    const nlohmann::json* findNode(const std::string& dottedKey) const;

    /**
     * @brief 深合并：override 中非对象字段覆盖 base，对象字段递归合并。
     */
    static nlohmann::json mergeJson(const nlohmann::json& base, const nlohmann::json& overrideJson);

    nlohmann::json m_root = nlohmann::json::object();  ///< 配置根节点
    mutable std::shared_mutex m_mutex;                 ///< 读写锁（读多写少）
};

} // namespace lingxi
