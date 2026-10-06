/**
 * @file FileService.cpp
 * @brief M4 文件服务实现。
 */
#include "FileService.h"

#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <sstream>
#include <algorithm>

#include <nlohmann/json.hpp>

#include "lingxi/base/TimeUtil.h"
#include "lingxi/base/Uuid.h"
#include "lingxi/crypto/Crypto.h"
#include "lingxi/logging/Logger.h"

namespace fs = std::filesystem;

namespace bhttp = boost::beast::http;  ///< boost 原生命名（避免与 lingxi::http 冲突）

namespace lingxi {

namespace {

/** 上传会话过期时间（毫秒）：超时未完成的会话在预检时惰性清理 */
constexpr int64_t kUploadSessionTtlMs = 24LL * 3600 * 1000;

/**
 * @brief 构造 JSON HTTP 响应。
 */
lingxi::http::HttpResponse makeJson(bhttp::status status, const std::string& body) {
    lingxi::http::HttpResponse response{status, 11};
    response.set(bhttp::field::content_type, "application/json; charset=utf-8");
    response.keep_alive(false);
    response.body() = body;
    response.prepare_payload();
    return response;
}

/**
 * @brief 读取文件全部字节（分块文件合并用；单块 ≤4MB 内存可控）。
 */
bool readFile(const std::string& path, std::string& out) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return false;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    out = buffer.str();
    return true;
}

} // namespace

FileService::FileService(db::MySqlConnectionPool* db, std::string storageDir, int chunkSize,
                         SnowflakeIdGenerator* idGen)
    : m_db(db), m_storageDir(std::move(storageDir)),
      m_chunkSize(chunkSize > 0 ? chunkSize : 4 * 1024 * 1024), m_idGen(idGen) {
    fs::create_directories(m_storageDir + "/blobs");
    fs::create_directories(m_storageDir + "/chunks");
    LX_LOG_INFO("FileService ready: dir={} chunkSize={}", m_storageDir, m_chunkSize);
}

void FileService::setup(lingxi::http::HttpServer& server) {
    server.route("POST", "/api/file/preflight",
                 [this](const lingxi::http::HttpRequest& req, const std::string&) {
                     return handlePreflight(req, "");
                 });
    server.route("PUT", "/api/file/chunk",
                 [this](const lingxi::http::HttpRequest& req, const std::string&) {
                     return handleChunk(req, "");
                 });
    server.route("POST", "/api/file/complete",
                 [this](const lingxi::http::HttpRequest& req, const std::string&) {
                     return handleComplete(req, "");
                 });
    server.routePrefix("GET", "/api/file/",
                       [this](const lingxi::http::HttpRequest& req,
                              const std::string& target) { return handleDownload(req, target); });
}

/* ==================== 预检（秒传） ==================== */

lingxi::http::HttpResponse FileService::handlePreflight(
    const lingxi::http::HttpRequest& request, const std::string&) {
    nlohmann::json requestJson;
    try {
        requestJson = nlohmann::json::parse(request.body());
    } catch (const nlohmann::json::exception&) {
        return makeJson(bhttp::status::bad_request, "{\"err_code\":400,\"err_msg\":\"bad json\"}");
    }
    const std::string md5 = requestJson.value("md5", "");
    const int64_t size = requestJson.value("size", 0LL);
    const std::string name = requestJson.value("name", "");
    const std::string mime = requestJson.value("mime", "");
    if (md5.size() != 32 || size <= 0) {
        return makeJson(bhttp::status::bad_request, "{\"err_code\":400,\"err_msg\":\"bad args\"}");
    }

    // 秒传：同 md5 且同 size（docs/03 §2.4 唯一可信条件）
    auto conn = m_db->acquire();
    if (conn != nullptr) {
        db::MySqlResult row;
        if (conn->query("SELECT fid FROM t_file WHERE md5='" + md5 + "' AND size=" +
                            std::to_string(size),
                        row) && row.next()) {
            const std::string fid = row.getString(0);
            LX_LOG_INFO("instant upload hit: md5={} fid={}", md5, fid);
            conn->execute("UPDATE t_file SET ref_count=ref_count+1 WHERE fid='" + fid + "'");
            return makeJson(bhttp::status::ok,
                            "{\"err_code\":0,\"uploaded\":true,\"fid\":\"" + fid + "\"}");
        }
    }

    // 新上传会话（惰性清理过期会话）
    const int64_t now = TimeUtil::nowMs();
    {
        std::lock_guard<std::mutex> lock(m_uploadsMutex);
        for (auto it = m_uploads.begin(); it != m_uploads.end();) {
            if (now - it->second.createdAtMs > kUploadSessionTtlMs) {
                std::error_code ec;
                fs::remove_all(m_storageDir + "/chunks/" + it->first, ec);
                it = m_uploads.erase(it);
            } else {
                ++it;
            }
        }
    }
    const std::string uploadId = Uuid::generate();
    {
        std::lock_guard<std::mutex> lock(m_uploadsMutex);
        UploadSession session;
        session.md5 = md5;
        session.name = name;
        session.mime = mime;
        session.size = size;
        session.createdAtMs = now;
        m_uploads.emplace(uploadId, std::move(session));
    }
    return makeJson(bhttp::status::ok,
                    "{\"err_code\":0,\"uploaded\":false,\"uploadId\":\"" + uploadId +
                        "\",\"chunkSize\":" + std::to_string(m_chunkSize) + "}");
}

/* ==================== 分块上传 ==================== */

lingxi::http::HttpResponse FileService::handleChunk(
    const lingxi::http::HttpRequest& request, const std::string&) {
    const std::string uploadId = queryParam(std::string(request.target()), "uploadId");
    const int index = std::atoi(queryParam(std::string(request.target()), "index").c_str());

    std::lock_guard<std::mutex> lock(m_uploadsMutex);
    auto it = m_uploads.find(uploadId);
    if (it == m_uploads.end()) {
        return makeJson(bhttp::status::not_found, "{\"err_code\":404,\"err_msg\":\"no session\"}");
    }
    // 块序号与大小校验（末块允许小于 chunkSize）
    const int64_t expected = (static_cast<int64_t>(index) + 1) * m_chunkSize <= it->second.size
                                 ? m_chunkSize
                                 : it->second.size - static_cast<int64_t>(index) * m_chunkSize;
    if (index < 0 || expected <= 0 ||
        static_cast<int64_t>(request.body().size()) != expected) {
        return makeJson(bhttp::status::bad_request, "{\"err_code\":400,\"err_msg\":\"bad chunk\"}");
    }
    if (!writeFile(chunkPath(uploadId, index), request.body())) {
        return makeJson(bhttp::status::internal_server_error,
                        "{\"err_code\":500,\"err_msg\":\"write failed\"}");
    }
    it->second.received.insert(index);
    return makeJson(bhttp::status::ok, "{\"err_code\":0,\"received\":" +
                                          std::to_string(it->second.received.size()) + "}");
}

/* ==================== 完成合并 ==================== */

lingxi::http::HttpResponse FileService::handleComplete(
    const lingxi::http::HttpRequest& request, const std::string&) {
    nlohmann::json requestJson;
    try {
        requestJson = nlohmann::json::parse(request.body());
    } catch (const nlohmann::json::exception&) {
        return makeJson(bhttp::status::bad_request, "{\"err_code\":400,\"err_msg\":\"bad json\"}");
    }
    const std::string uploadId = requestJson.value("uploadId", "");

    std::string md5;
    std::string name;
    std::string mime;
    int64_t size = 0;
    int totalChunks = 0;
    std::set<int> received;
    {
        std::lock_guard<std::mutex> lock(m_uploadsMutex);
        auto it = m_uploads.find(uploadId);
        if (it == m_uploads.end()) {
            return makeJson(bhttp::status::not_found, "{\"err_code\":404,\"err_msg\":\"no session\"}");
        }
        md5 = it->second.md5;
        name = it->second.name;
        mime = it->second.mime;
        size = it->second.size;
        received = it->second.received;
        totalChunks = static_cast<int>((size + m_chunkSize - 1) / m_chunkSize);
    }

    // 完整性：块数齐了才允许合并
    if (static_cast<int>(received.size()) != totalChunks) {
        return makeJson(bhttp::status::bad_request,
                        "{\"err_code\":400,\"err_msg\":\"chunks incomplete\",\"received\":" +
                            std::to_string(received.size()) + ",\"total\":" +
                            std::to_string(totalChunks) + "}");
    }

    // 流式合并 + MD5 校验（ADR-019：complete 必须以服务端实测摘要为准）
    const std::string fid = [&] {
        std::stringstream stream;
        stream << std::hex << m_idGen->nextId();
        return stream.str();
    }();
    const std::string finalPath = blobPath(fid);
    fs::create_directories(fs::path(finalPath).parent_path().string());

    crypto::StreamingMd5 streaming;
    std::ofstream output(finalPath, std::ios::binary);
    if (!output.is_open()) {
        return makeJson(bhttp::status::internal_server_error,
                        "{\"err_code\":500,\"err_msg\":\"open output failed\"}");
    }
    for (int i = 0; i < totalChunks; ++i) {
        std::string block;
        if (!readFile(chunkPath(uploadId, i), block)) {
            output.close();
            fs::remove(finalPath);
            return makeJson(bhttp::status::internal_server_error,
                            "{\"err_code\":500,\"err_msg\":\"chunk missing\"}");
        }
        streaming.update(block.data(), block.size());
        output.write(block.data(), static_cast<std::streamsize>(block.size()));
    }
    output.close();

    if (streaming.finalHex() != md5) {
        fs::remove(finalPath);
        std::lock_guard<std::mutex> lock(m_uploadsMutex);
        m_uploads.erase(uploadId);
        return makeJson(bhttp::status::bad_request,
                        "{\"err_code\":400,\"err_msg\":\"md5 mismatch\"}");
    }

    // 元数据入库
    auto conn = m_db->acquire();
    if (conn == nullptr) {
        return makeJson(bhttp::status::service_unavailable,
                        "{\"err_code\":503,\"err_msg\":\"db busy\"}");
    }
    if (!conn->execute("INSERT INTO t_file (fid, md5, file_name, size, mime) VALUES ('" + fid +
                       "', '" + md5 + "', '" + conn->escapeString(name) + "', " +
                       std::to_string(size) + ", '" + conn->escapeString(mime) + "')")) {
        fs::remove(finalPath);
        return makeJson(bhttp::status::internal_server_error,
                        "{\"err_code\":500,\"err_msg\":\"db failed\"}");
    }

    // 清理暂存块
    std::error_code ec;
    fs::remove_all(m_storageDir + "/chunks/" + uploadId, ec);
    {
        std::lock_guard<std::mutex> lock(m_uploadsMutex);
        m_uploads.erase(uploadId);
    }
    LX_LOG_INFO("file uploaded: fid={} name={} size={}", fid, name, size);
    return makeJson(bhttp::status::ok, "{\"err_code\":0,\"fid\":\"" + fid + "\"}");
}

/* ==================== 下载（Range 断点续传） ==================== */

lingxi::http::HttpResponse FileService::handleDownload(
    const lingxi::http::HttpRequest& request, const std::string&) {
    // 从请求行自取路径（前缀路由的第二参数是 clientIp）：/api/file/{fid}
    std::string target(request.target());
    const auto queryPos = target.find('?');
    if (queryPos != std::string::npos) {
        target.resize(queryPos);
    }
    const std::string prefix = "/api/file/";
    if (target.compare(0, prefix.size(), prefix) != 0 || target.size() <= prefix.size()) {
        return makeJson(bhttp::status::not_found, "{\"err_code\":404}");
    }
    const std::string fid = target.substr(prefix.size());

    auto conn = m_db->acquire();
    if (conn == nullptr) {
        return makeJson(bhttp::status::service_unavailable, "{\"err_code\":503}");
    }
    db::MySqlResult row;
    if (!conn->query("SELECT file_name, size FROM t_file WHERE fid='" + fid + "'", row) ||
        !row.next()) {
        return makeJson(bhttp::status::not_found, "{\"err_code\":404}");
    }
    const std::string fileName = row.getString(0);
    const int64_t fileSize = row.getInt64(1);

    std::ifstream file(blobPath(fid), std::ios::binary);
    if (!file.is_open()) {
        return makeJson(bhttp::status::not_found, "{\"err_code\":404}");
    }

    // Range 解析：bytes=N-[M]（M 可省略）
    int64_t offset = 0;
    int64_t length = fileSize;
    bool partial = false;
    const auto rangeIt = request.find(bhttp::field::range);
    if (rangeIt != request.end()) {
        const std::string range(rangeIt->value());
        const auto prefix = std::string("bytes=");
        if (range.compare(0, prefix.size(), prefix) == 0) {
            const auto dash = range.find('-');
            offset = std::stoll(range.substr(prefix.size(), dash - prefix.size()));
            if (dash != std::string::npos && range.size() > dash + 1) {
                length = std::stoll(range.substr(dash + 1)) - offset + 1;
            } else {
                length = fileSize - offset;
            }
            if (offset < 0 || offset >= fileSize || length <= 0) {
                lingxi::http::HttpResponse response{bhttp::status::range_not_satisfiable, 11};
                response.prepare_payload();
                return response;
            }
            partial = true;
            length = std::min(length, fileSize - offset);
        }
    }

    // 读取指定区间（分块拉取模式下 ≤4MB，内存可控；整文件下载大文件场景 M8 优化）
    file.seekg(static_cast<std::streamoff>(offset));
    std::string body(length, '\0');
    file.read(body.data(), static_cast<std::streamsize>(length));
    body.resize(static_cast<size_t>(file.gcount()));

    lingxi::http::HttpResponse response{partial ? bhttp::status::partial_content
                                                : bhttp::status::ok,
                                        11};
    response.set(bhttp::field::content_type, "application/octet-stream");
    response.set(bhttp::field::accept_ranges, "bytes");
    if (partial) {
        response.set(bhttp::field::content_range,
                     "bytes " + std::to_string(offset) + "-" +
                         std::to_string(offset + static_cast<int64_t>(body.size()) - 1) + "/" +
                         std::to_string(fileSize));
    } else {
        response.set(bhttp::field::content_disposition,
                     "attachment; filename*=UTF-8''" + fileName);
    }
    response.body() = std::move(body);
    response.prepare_payload();
    return response;
}

/* ==================== 工具 ==================== */

std::string FileService::queryParam(const std::string& target, const std::string& key) {
    const auto queryBegin = target.find('?');
    if (queryBegin == std::string::npos) {
        return "";
    }
    const std::string query = target.substr(queryBegin + 1);
    size_t start = 0;
    while (start < query.size()) {
        const size_t end = query.find('&', start);
        const std::string pair = query.substr(start, end == std::string::npos ? end : end - start);
        const auto eq = pair.find('=');
        if (eq != std::string::npos && pair.substr(0, eq) == key) {
            return pair.substr(eq + 1);
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return "";
}

std::string FileService::chunkPath(const std::string& uploadId, int index) const {
    return m_storageDir + "/chunks/" + uploadId + "/" + std::to_string(index);
}

std::string FileService::blobPath(const std::string& fid) const {
    return m_storageDir + "/blobs/" + fid.substr(0, 2) + "/" + fid.substr(2, 2) + "/" + fid;
}

bool FileService::writeFile(const std::string& path, const std::string& data) {
    fs::create_directories(fs::path(path).parent_path().string());
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        return false;
    }
    file.write(data.data(), static_cast<std::streamsize>(data.size()));
    return file.good();
}

} // namespace lingxi
