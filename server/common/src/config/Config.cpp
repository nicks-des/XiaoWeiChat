/**
 * @file Config.cpp
 * @brief 配置模块实现。
 */
#include "lingxi/config/Config.h"

#include <fstream>
#include <sstream>

#include "lingxi/logging/Logger.h"

namespace lingxi {

Config& Config::instance() {
    static Config s_config;
    return s_config;
}

bool Config::load(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return false;
    }

    nlohmann::json parsed;
    try {
        file >> parsed;
    } catch (const nlohmann::json::parse_error& e) {
        LX_LOG_ERROR("config parse failed: {} ({})", path, e.what());
        return false;
    }
    if (!parsed.is_object()) {
        LX_LOG_ERROR("config root must be an object: {}", path);
        return false;
    }

    std::unique_lock<std::shared_mutex> lock(m_mutex);
    m_root = std::move(parsed);
    return true;
}

bool Config::loadOverride(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return false;
    }

    nlohmann::json parsed;
    try {
        file >> parsed;
    } catch (const nlohmann::json::parse_error& e) {
        LX_LOG_ERROR("config override parse failed: {} ({})", path, e.what());
        return false;
    }
    if (!parsed.is_object()) {
        return false;
    }

    std::unique_lock<std::shared_mutex> lock(m_mutex);
    m_root = mergeJson(m_root, parsed);
    return true;
}

bool Config::contains(const std::string& dottedKey) const {
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    return findNode(dottedKey) != nullptr;
}

const nlohmann::json* Config::findNode(const std::string& dottedKey) const {
    const nlohmann::json* node = &m_root;
    std::istringstream stream(dottedKey);
    std::string segment;
    while (std::getline(stream, segment, '.')) {
        if (!node->is_object() || !node->contains(segment)) {
            return nullptr;
        }
        node = &((*node)[segment]);
    }
    return node;
}

nlohmann::json Config::mergeJson(const nlohmann::json& base, const nlohmann::json& overrideJson) {
    nlohmann::json merged = base;
    if (!merged.is_object() || !overrideJson.is_object()) {
        return overrideJson;  // 非对象直接覆盖
    }
    for (auto it = overrideJson.begin(); it != overrideJson.end(); ++it) {
        if (it->is_object() && merged.contains(it.key()) && merged[it.key()].is_object()) {
            merged[it.key()] = mergeJson(merged[it.key()], it.value());
        } else {
            merged[it.key()] = it.value();
        }
    }
    return merged;
}

} // namespace lingxi
