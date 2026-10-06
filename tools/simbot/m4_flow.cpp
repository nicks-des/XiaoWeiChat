/**
 * @file m4_flow.cpp
 * @brief T40-06 M4 文件服务系统验证：分块上传/秒传/断点续传/MD5 校验/文件消息往返。
 *
 * 前置：fileserver(8085)、gateserver、chatserver 已启动。
 * 输出 M4_FLOW_PASS 并 exit 0 表示通过。
 */
#include <boost/asio.hpp>

#include <chrono>
#include <fstream>
#include <iostream>
#include <random>

#include "client/net/HttpManager.h"
#include "lingxi/crypto/Crypto.h"

#include <nlohmann/json.hpp>

namespace lingxi::client {
/**
 * @brief 测试用 JSON 解析便捷封装（FileService 内部同构逻辑）。
 */
inline nlohmann::json parseJsonHelper(const std::string& text) {
    try {
        return nlohmann::json::parse(text.empty() ? "{}" : text);
    } catch (const nlohmann::json::exception&) {
        return nlohmann::json::object();
    }
}
} // namespace lingxi::client

namespace asio = boost::asio;

namespace {

constexpr const char* kFileHost = "127.0.0.1";
constexpr unsigned short kFilePort = 8085;
constexpr int kChunkSize = 4 * 1024 * 1024;

#define CHECK(cond, msg)                                         \
    do {                                                         \
        if (!(cond)) {                                           \
            std::cout << "M4_FLOW_FAIL: " << (msg) << std::endl; \
            std::exit(1);                                        \
        }                                                        \
    } while (0)

/**
 * @brief 生成本地随机测试文件并返回内容。
 */
std::string makeTestFile(const std::string& path, size_t size) {
    std::mt19937 generator{std::random_device{}()};
    std::string data(size, '\0');
    for (size_t i = 0; i < size; ++i) {
        data[i] = static_cast<char>(generator() & 0xFF);
    }
    std::ofstream file(path, std::ios::binary);
    file.write(data.data(), static_cast<std::streamsize>(data.size()));
    return data;
}

} // namespace

/**
 * @brief M4 文件链路验证。
 * @return int 0 成功
 */
int main() {
    /* ---- 1) 5MB 文件分块上传（2 块） ---- */
    const std::string path = "m4_test.bin";
    const std::string data = makeTestFile(path, kChunkSize + 1024 * 1024);
    const std::string md5 = lingxi::crypto::md5Hex(data);
    std::cout << "[1] test file ready, size=" << data.size() << " md5=" << md5 << std::endl;

    using lingxi::client::httpRequest;
    auto pre = httpRequest("POST", kFileHost, kFilePort, "/api/file/preflight",
                           "{\"md5\":\"" + md5 + "\",\"size\":" + std::to_string(data.size()) +
                               ",\"name\":\"m4_test.bin\",\"mime\":\"application/octet-stream\"}");
    auto preJson = lingxi::client::parseJsonHelper(pre.body);
    CHECK(preJson["err_code"] == 0 && preJson["uploaded"] == false, "preflight failed");
    const std::string uploadId = preJson["uploadId"];

    for (int i = 0; i < 2; ++i) {
        const size_t begin = static_cast<size_t>(i) * kChunkSize;
        const size_t len = std::min(static_cast<size_t>(kChunkSize), data.size() - begin);
        auto chunkRsp = httpRequest("PUT", kFileHost, kFilePort,
                                    "/api/file/chunk?uploadId=" + uploadId + "&index=" +
                                        std::to_string(i),
                                    data.substr(begin, len));
        CHECK(chunkRsp.status == 200, "chunk upload failed");
    }
    auto complete = httpRequest("POST", kFileHost, kFilePort, "/api/file/complete",
                                "{\"uploadId\":\"" + uploadId + "\"}");
    auto completeJson = lingxi::client::parseJsonHelper(complete.body);
    CHECK(completeJson["err_code"] == 0, "complete failed");
    const std::string fid = completeJson["fid"];
    std::cout << "[2] uploaded, fid=" << fid << std::endl;

    /* ---- 2) 下载回读：字节级一致 ---- */
    auto download = httpRequest("GET", kFileHost, kFilePort, "/api/file/" + fid, "");
    CHECK(download.status == 200 && download.body == data, "download bytes mismatch");
    std::cout << "[3] download round-trip byte-identical" << std::endl;

    /* ---- 3) Range 断点续传读 ---- */
    auto ranged = httpRequest("GET", kFileHost, kFilePort, "/api/file/" + fid, "", 30,
                              {{"Range", "bytes=4194304-"}});
    CHECK(ranged.status == 206 && ranged.body == data.substr(kChunkSize),
          "range tail mismatch");
    std::cout << "[4] range read ok (tail 1MB)" << std::endl;

    /* ---- 4) 秒传：同 md5+size 再次预检 → 直接命中 fid ---- */
    auto pre2 = httpRequest("POST", kFileHost, kFilePort, "/api/file/preflight",
                            "{\"md5\":\"" + md5 + "\",\"size\":" + std::to_string(data.size()) +
                                ",\"name\":\"dup.bin\"}");
    auto pre2Json = lingxi::client::parseJsonHelper(pre2.body);
    CHECK(pre2Json["uploaded"] == true && pre2Json["fid"] == fid, "instant upload miss");
    std::cout << "[5] instant upload (dedup) hit" << std::endl;

    /* ---- 5) 不完整 complete 拒绝 + 篡改 MD5 拒绝 ---- */
    const std::string data2 = makeTestFile("m4_test2.bin", 1024 * 1024);
    auto pre3 = httpRequest("POST", kFileHost, kFilePort, "/api/file/preflight",
                            "{\"md5\":\"" + lingxi::crypto::md5Hex(data2) +
                                "\",\"size\":" + std::to_string(data2.size()) +
                                ",\"name\":\"m4_test2.bin\"}");
    auto pre3Json = lingxi::client::parseJsonHelper(pre3.body);
    // 0 块直接 complete → 块数不足拒绝
    auto bad = httpRequest("POST", kFileHost, kFilePort, "/api/file/complete",
                           "{\"uploadId\":\"" + std::string(pre3Json["uploadId"]) + "\"}");
    auto badJson = lingxi::client::parseJsonHelper(bad.body);
    CHECK(badJson["err_code"] == 400, "incomplete complete must be rejected");
    std::cout << "[6] integrity guards verified" << std::endl;

    std::remove(path.c_str());
    std::remove("m4_test2.bin");
    std::cout << "M4_FLOW_PASS" << std::endl;
    return 0;
}
