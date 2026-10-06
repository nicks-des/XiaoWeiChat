/**
 * @file FileService.h
 * @brief M4 文件服务：秒传预检（md5+size）、分块上传、合并校验入库、下载（Range 断点续传）。
 *
 * 流程（docs/03 §4 / ADR-007）：
 *   POST /api/file/preflight {"md5","size","name","mime"}
 *     → 秒传命中 {"uploaded":true,"fid":...}
 *     → 否则 {"uploaded":false,"uploadId":...,"chunkSize":...,"received":[...]}
 *   PUT  /api/file/chunk?uploadId=&index=      （body=原始分块字节）
 *   POST /api/file/complete {"uploadId"}       → 合并 + 流式 MD5 校验 → 入库 → {"fid":...}
 *   GET  /api/file/{fid}                       （Range: bytes=N- 断点续传，206/200）
 */
#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>

#include "lingxi/base/NonCopyable.h"
#include "lingxi/base/SnowflakeIdGenerator.h"
#include "lingxi/db/MySqlPool.h"
#include "lingxi/http/HttpServer.h"

namespace lingxi {

/**
 * @brief 文件存储服务（FileServer 进程核心）。
 */
class FileService : private NonCopyable {
public:
    /**
     * @param db         MySQL 池（t_file 元数据）
     * @param storageDir 存储根目录（blobs/chunks 子目录自动创建）
     * @param chunkSize  分块大小（字节，建议 1~4MB）
     */
    FileService(db::MySqlConnectionPool* db, std::string storageDir, int chunkSize,
                SnowflakeIdGenerator* idGen);

    /**
     * @brief 装配全部 HTTP 路由（须在 HttpServer::start 前调用）。
     */
    void setup(http::HttpServer& server);

private:
    /**
     * @brief 上传会话（内存态：服务重启未完成的会话失效，客户端重新预检续传）。
     */
    struct UploadSession {
        std::string md5;         ///< 声明摘要（complete 时校验）
        std::string name;        ///< 原始文件名
        std::string mime;
        int64_t size = 0;        ///< 总字节数
        int64_t createdAtMs = 0; ///< 过期清理依据
        std::set<int> received;  ///< 已落盘块号
    };

    http::HttpResponse handlePreflight(const http::HttpRequest& request, const std::string&);
    http::HttpResponse handleChunk(const http::HttpRequest& request, const std::string&);
    http::HttpResponse handleComplete(const http::HttpRequest& request, const std::string&);
    http::HttpResponse handleDownload(const http::HttpRequest& request, const std::string& target);

    /**
     * @brief 从查询串取参数。
     */
    static std::string queryParam(const std::string& target, const std::string& key);

    /**
     * @brief 分块暂存路径 chunks/{uploadId}/{index}。
     */
    std::string chunkPath(const std::string& uploadId, int index) const;

    /**
     * @brief 最终对象路径 blobs/{fid[0:2]}/{fid[2:4]}/{fid}。
     */
    std::string blobPath(const std::string& fid) const;

    /**
     * @brief 写小文件（分块落盘）。
     */
    static bool writeFile(const std::string& path, const std::string& data);

    db::MySqlConnectionPool* m_db;
    std::string m_storageDir;
    int m_chunkSize;
    SnowflakeIdGenerator* m_idGen;

    std::mutex m_uploadsMutex;                            ///< 上传会话表锁
    std::map<std::string, UploadSession> m_uploads;       ///< uploadId → 会话
};

} // namespace lingxi
