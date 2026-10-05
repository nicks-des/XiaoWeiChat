# 灵犀 IM (LingXi)

> 一个类似 QQ 的桌面端即时通讯系统，深度融合「酒馆」AI 角色扮演玩法。
> 客户端：Qt5 + C++ ｜ 服务端：C++ 分布式架构 ｜ 网络层：Boost.Asio ｜ 后期扩展 Android 端。

> 项目代号「灵犀」为暂定名，可随时改名；酒馆玩法模块代号 **Tavern**。

---

## 1. 项目定位

做一个功能对标 QQ/微信的桌面 IM（好友、单聊群聊、文字/语音/图片/视频/文件消息、音视频通话），
并把 **酒馆（SillyTavern）式的 AI 角色扮演** 作为一等公民玩法融入 IM 体系：

- **AI 角色即用户**：AI 角色拥有 uid、头像、个性签名，和真人走完全相同的好友 / 会话 / 消息管线；
- **酒馆玩法层**：角色卡（兼容 SillyTavern PNG 卡）、世界书、剧情群聊 + 导演调度、
  长期记忆、好感度、Swipe 重掷、TTS 语音消息，全部挂接在 IM 的会话模型之上。

## 2. 技术栈总览

| 层次 | 选型 | 说明 |
| --- | --- | --- |
| 客户端 UI | Qt 5.12.11（Widgets） | 本机已安装（D:\Qt\Qt5.12.11） |
| 客户端架构 | Qt Widgets + 分层架构（net / service / ui / media） | 协议层平台无关，为 Android 端预留 |
| 网络库 | Boost 1.90（Asio 长连接/RPC、Beast HTTP） | 本机已就绪：D:\cpp\boost_1_90_0（vc143 x64 预编译库齐全） |
| 序列化 | Protobuf（proto3） | 通信协议与 RPC 统一使用 |
| 服务端 | C++ 多进程分布式：GateServer / StatusServer / ChatServer / FileServer / AIServer | 自研 RPC（Protobuf + Asio） |
| 数据库 | MySQL 8.0（本机 D:\cpp\mysql）+ Connector C++ 8.3（本机已有） | 用户 / 关系 / 消息 / AI 数据 |
| 缓存 | Redis（本机 C:\Redis） | 路由表 / token / seq / 分布式锁 |
| 音视频 | libdatachannel（WebRTC）+ Opus + OpenH264 + Qt 采集；FFmpeg 4.2.2（本机已有）用于解码渲染与缩略图 | P2P 优先，TURN 中转兜底 |
| LLM 接入 | 统一 LLM 网关（OpenAI 兼容 API 优先，预留本地模型 Provider） | 流式 SSE 输出 |
| 构建 | CMake（VS 18 自带，M0 配置 PATH）+ vcpkg（补齐 protobuf / libdatachannel 等缺失依赖） | MSVC（VS2022/VS18）工具链 |
| 测试 | GoogleTest 单元测试 + 自研协议机器人（simbot）压测 + 手工用例记录 | 记录见 docs/tests |

## 3. 文档导航（开发文档全集）

| 文档 | 内容 |
| --- | --- |
| [docs/00-项目总览与术语.md](docs/00-项目总览与术语.md) | 目标、范围、术语表、里程碑总览、风险清单 |
| [docs/01-系统架构设计.md](docs/01-系统架构设计.md) | 分布式拓扑、各服务职责、自研 RPC 设计、目录结构、编码规范 |
| [docs/02-通信协议设计.md](docs/02-通信协议设计.md) | 帧格式、消息 ID 分配表、proto 定义、关键时序图、可靠性设计 |
| [docs/03-数据库与存储设计.md](docs/03-数据库与存储设计.md) | MySQL 表结构 DDL、Redis Key 设计、文件存储布局、分表演进 |
| [docs/04-酒馆Tavern融合设计.md](docs/04-酒馆Tavern融合设计.md) | AI 角色体系、LLM 网关、世界书、记忆、剧情群聊、Swipe、好感度、TTS |
| [docs/05-音视频通话设计.md](docs/05-音视频通话设计.md) | 信令流程、libdatachannel 集成、采集编解码、弱网与 AI 通话扩展 |
| [docs/06-任务流与里程碑.md](docs/06-任务流与里程碑.md) | M0~M8 详细任务流（任务编号、依赖、产出物、验收标准） |
| [docs/07-测试计划与记录.md](docs/07-测试计划与记录.md) | 测试策略、用例模板、测试记录归档约定 |
| [docs/08-IM面试题库.md](docs/08-IM面试题库.md) | 常见 IM 面试题与答题要点（持续更新） |
| [docs/09-技术决策记录ADR.md](docs/09-技术决策记录ADR.md) | 所有关键技术决策（备选项、结论、理由） |
| [docs/devlog/](docs/devlog) | 开发日志（按日期追加，记录坑与细节） |

## 4. 里程碑速览

| 里程碑 | 内容 | 预估 |
| --- | --- | --- |
| M0 | 工程地基：CMake/vcpkg/目录骨架/公共库/协议 v0 | 1 周 |
| M1 | 账号与接入：注册登录、token、StatusServer、客户端登录 UI | 1~2 周 |
| M2 | 单聊消息内核：seq/ACK/去重/离线/漫游 + 聊天窗口 v1 | 2 周 |
| M3 | 好友与群组 | 2 周 |
| M4 | 文件与富媒体：FileServer 秒传/分块 + 图片/语音/文件消息 | 2 周 |
| M5 | 音视频通话（libdatachannel） | 2~3 周 |
| M6 | 酒馆一期：AIServer + LLM 网关 + 角色卡/世界书 + 流式回复 + Swipe | 2~3 周 |
| M7 | 酒馆二期：剧情群聊 + 导演 + 记忆 + 好感度 + TTS + 广场 | 2 周 |
| M8 | 打磨交付：压测报告、安全加固、安装包、文档终稿 | 1~2 周 |

详细任务拆解见 [docs/06-任务流与里程碑.md](docs/06-任务流与里程碑.md)。

## 5. 编码规范（强约束）

- **命名**：类名/枚举类型/文件名用大驼峰（`ChatServer`、`MessageManager`）；
  函数、变量、参数用小驼峰（`sendMessage`、`connPool`）；常量 `k` + 大驼峰（`kMaxRetry`）；
  宏与编译期常量全大写下划线（`LINGXI_VERSION`）。
- **注释**：所有类、函数（含参数与返回值）、重要成员变量、协议字段必须写 Doxygen 标准注释（`/** ... */`），
  关键算法与决策处补充行内注释说明「为什么」。
- 每个里程碑结束执行 `clang-format` 全量格式化并跑通全部测试后才允许归档。

## 6. 快速开始（M0 完成后补充）

本机环境盘点（2026-10-05 核实）：

| 依赖 | 状态 |
| --- | --- |
| Boost 1.90.0 | ✅ D:\cpp\boost_1_90_0（含 vc143 x64 预编译静态库，debug/release） |
| Qt 5.12.11 | ✅ D:\Qt\Qt5.12.11 |
| MySQL 8.0.28 / Connector C++ 8.3 | ✅ D:\cpp\mysql、D:\cpp\mysql-connector-c++-8.3.0-winx64 |
| Redis | ✅ C:\Redis |
| FFmpeg 4.2.2（含开发库） | ✅ D:\cpp\ffmpeg-4.2.2 |
| CMake | ✅ VS 18 自带（Community），M0 配置 PATH |
| vcpkg | ❌ M0 安装（用于 protobuf / libdatachannel / spdlog / hiredis / gtest 等） |
| OpenSSL / Protobuf / libdatachannel | ❌ M0 经 vcpkg 安装 |

规划中：一键构建脚本、服务端启动顺序、客户端运行方式。
