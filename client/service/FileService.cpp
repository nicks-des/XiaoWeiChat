/**
 * @file FileService.cpp
 * @brief 客户端文件服务实现（M4）：分块上传（秒传/进度）+ Range 断点续传下载。
 */
#include "FileService.h"

#include <QTimer>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "client/net/HttpManager.h"
#include "lingxi/config/Config.h"
#include "lingxi/crypto/Crypto.h"
#include "lingxi/logging/Logger.h"

namespace fs = std::filesystem;

namespace lingxi::client {

namespace {

/**
 * @brief 读取整个文件到内存（M4 上限 4GB 内场景；更大文件 M8 改流式）。
 */
bool readAllBytes(const std::string& path, std::string& out) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return false;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    out = buffer.str();
    return true;
}

/**
 * @brief 解析 JSON（失败返回空对象）。
 */
nlohmann::json parseJson(const std::string& text) {
    try {
        return nlohmann::json::parse(text.empty() ? "{}" : text);
    } catch (const nlohmann::json::exception&) {
        return nlohmann::json::object();
    }
}

} // namespace

FileService& FileService::instance() {
    static FileService s_service;
    return s_service;
}

bool FileService::setup() {
    auto& config = Config::instance();
    m_host = config.get<std::string>("fileserver.host", "127.0.0.1");
    m_port = static_cast<unsigned short>(config.get<int>("fileserver.httpPort", 8085));
    m_chunkSize = config.get<int>("fileserver.chunkSize", 4 * 1024 * 1024);
    LX_LOG_INFO("FileService setup done ({}:{})", m_host, m_port);
    return true;
}

/**
 * @brief UI 线程执行辅助（FileService 归属主线程）。
 */
template <typename Fn>
static void emitOnUi(Fn&& fn) {
    QMetaObject::invokeMethod(&FileService::instance(), std::forward<Fn>(fn),
                              Qt::QueuedConnection);
}

void FileService::uploadFile(const QString& filePath, std::function<void(int)> onProgress,
                             std::function<void(QString, QString)> onDone) {
    const std::string path = filePath.toStdString();
    std::thread([this, path, onProgress, onDone] {
        // 1. 读文件 + 计算 MD5
        std::string data;
        if (!readAllBytes(path, data)) {
            emitOnUi([&] { onDone(QString(), QStringLiteral("无法读取文件")); });
            return;
        }
        const std::string md5 = crypto::md5Hex(data);
        const std::string name = fs::path(path).filename().string();

        // 2. 预检（秒传）
        auto pre = httpRequest("POST", m_host, m_port, "/api/file/preflight",
                               "{\"md5\":\"" + md5 + "\",\"size\":" +
                                   std::to_string(data.size()) + ",\"name\":\"" + name +
                                   "\",\"mime\":\"\"}");
        auto preJson = parseJson(pre.body);
        if (preJson.value("err_code", -1) != 0) {
            emitOnUi([&] { onDone(QString(), QStringLiteral("预检失败")); });
            return;
        }
        if (preJson.value("uploaded", false)) {
            emitOnUi([&] { onProgress(100); });
            emitOnUi([&] { onDone(QString::fromStdString(preJson.value("fid", "")), QString()); });
            return;
        }
        const std::string uploadId = preJson.value("uploadId", "");

        // 3. 分块上传（PUT；进度按块）
        const int totalChunks = static_cast<int>((data.size() + m_chunkSize - 1) / m_chunkSize);
        for (int i = 0; i < totalChunks; ++i) {
            const size_t begin = static_cast<size_t>(i) * m_chunkSize;
            const size_t len = std::min(static_cast<size_t>(m_chunkSize), data.size() - begin);
            const auto chunkRsp =
                httpRequest("PUT", m_host, m_port,
                            "/api/file/chunk?uploadId=" + uploadId + "&index=" + std::to_string(i),
                            data.substr(begin, len));
            if (chunkRsp.status != 200) {
                emitOnUi([&] { onDone(QString(), QStringLiteral("分块上传失败")); });
                return;
            }
            emitOnUi([&onProgress, pct = (i + 1) * 90 / totalChunks] { onProgress(pct); });
        }

        // 4. 合并校验
        const auto completeRsp = httpRequest("POST", m_host, m_port, "/api/file/complete",
                                             "{\"uploadId\":\"" + uploadId + "\"}");
        auto completeJson = parseJson(completeRsp.body);
        if (completeJson.value("err_code", -1) != 0) {
            emitOnUi([&] { onDone(QString(), QStringLiteral("合并校验失败")); });
            return;
        }
        emitOnUi([&] { onProgress(100); });
        emitOnUi([&] { onDone(QString::fromStdString(completeJson.value("fid", "")), QString()); });
    }).detach();  // M4：一次性任务；线程池收敛同 AccountService 待办
}

void FileService::downloadFile(const QString& fid, const QString& savePath,
                               std::function<void(int)> onProgress,
                               std::function<void(bool, QString, QString)> onDone) {
    const std::string id = fid.toStdString();
    std::thread([this, id, savePath, onProgress, onDone] {
        const std::string dst = savePath.toStdString();
        // 断点续传：.part 已有长度即起点（04 文档 §4）
        int64_t offset = 0;
        const std::string partPath = dst + ".part";
        std::error_code ec;
        if (fs::exists(partPath, ec)) {
            offset = static_cast<int64_t>(fs::file_size(partPath, ec));
        }
        std::ofstream output(partPath, std::ios::binary | std::ios::app);
        if (!output.is_open()) {
            emitOnUi([&] { onDone(false, savePath, QStringLiteral("无法创建临时文件")); });
            return;
        }
        int64_t total = -1;
        while (total < 0 || offset < total) {
            // Range 分块拉取（服务端 206 响应含 Content-Range 总长，docs/03 §4）
            std::map<std::string, std::string> extra;
            extra["Range"] =
                total > 0 ? "bytes=" + std::to_string(offset) + "-" +
                                std::to_string(std::min<int64_t>(offset + m_chunkSize - 1, total - 1))
                          : "bytes=" + std::to_string(offset) + "-";
            auto rsp = httpRequest("GET", m_host, m_port, "/api/file/" + id, "", 30, extra);
            if (rsp.status != 200 && rsp.status != 206) {
                emitOnUi([&] { onDone(false, savePath, QStringLiteral("下载失败")); });
                return;
            }
            if (total < 0) {
                const auto it = rsp.headers.find("content-range");
                if (it != rsp.headers.end()) {
                    const std::string& value = it->second;
                    const auto slash = value.rfind('/');
                    total = slash == std::string::npos
                                ? static_cast<int64_t>(rsp.body.size())
                                : std::stoll(value.substr(slash + 1));
                } else {
                    total = static_cast<int64_t>(rsp.body.size());
                }
            }
            output.write(rsp.body.data(), static_cast<std::streamsize>(rsp.body.size()));
            offset += static_cast<int64_t>(rsp.body.size());
            if (total > 0) {
                emitOnUi([&onProgress, pct = static_cast<int>(
                                         std::min<int64_t>(100, offset * 100 / total))] {
                    onProgress(pct);
                });
            }
            if (rsp.body.empty()) {
                break;
            }
        }
        output.close();
        fs::rename(partPath, dst, ec);
        emitOnUi([&] { onDone(true, savePath, QString()); });
    }).detach();
}

} // namespace lingxi::client
