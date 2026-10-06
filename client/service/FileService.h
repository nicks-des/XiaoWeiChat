/**
 * @file FileService.h
 * @brief 客户端文件服务（M4）：分块上传（秒传/进度）+ Range 断点续传下载（纯异步编排）。
 *
 * 线程模型：工作线程执行 HTTP，回调经 invokeMethod 切 UI 线程（与 AccountService 同约定）。
 */
#pragma once

#include <QObject>
#include <QString>

#include <functional>

namespace lingxi::client {

/**
 * @brief 文件服务（单例）。
 */
class FileService : public QObject {
    Q_OBJECT
public:
    static FileService& instance();

    /**
     * @brief 初始化（FileServer 地址来自 config/fileserver 段）。
     * @return bool 配置加载成功为 true
     */
    bool setup();

    /**
     * @brief 上传本地文件（预检秒传 → 分块 PUT → complete；进度按块回调）。
     * @param filePath   本地路径
     * @param onProgress 进度回调（UI 线程，0~100）
     * @param onDone     完成回调（UI 线程）：fid 为空表示失败，second 为错误信息
     */
    void uploadFile(const QString& filePath,
                    std::function<void(int)> onProgress,
                    std::function<void(QString fid, QString err)> onDone);

    /**
     * @brief 下载文件（Range 分块拉取 + .part 断点续传；进度按块回调）。
     * @param fid        文件 ID
     * @param savePath   目标路径（临时态为 savePath + ".part"）
     * @param onProgress 进度回调（UI 线程，0~100）
     * @param onDone     完成回调（UI 线程）：ok 为 true 时 savePath 生效
     */
    void downloadFile(const QString& fid, const QString& savePath,
                      std::function<void(int)> onProgress,
                      std::function<void(bool ok, QString savePath, QString err)> onDone);

    /**
     * @brief FileServer HTTP 端口。
     */
    unsigned short port() const { return m_port; }

private:
    std::string m_host;          ///< FileServer 地址
    unsigned short m_port = 8085;
    int m_chunkSize = 4 * 1024 * 1024;
};

} // namespace lingxi::client
