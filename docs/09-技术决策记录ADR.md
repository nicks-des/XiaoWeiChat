# 09 技术决策记录（ADR）

> 记录项目所有关键技术决策：背景 → 备选方案 → 结论 → 理由 → 后续影响。
> 状态标记：✅ 已确认（含项目负责人确认项 ⭐）｜ 🔄 待重审条件。

---

## ADR-001 客户端框架：Qt 5.12 Widgets ⭐

- **背景**：需求指定 Qt5 + C++；本机已有 Qt 5.12.11（D:\Qt）与 Qt 6.10.x。
- **备选**：Qt 6.10（新 API、 better 高 DPI）；QML/Quick（动效好）。
- **结论**：Qt 5.12.11 + Widgets。
- **理由**：需求明确 Qt5；5.12 与 MSVC v14x ABI 兼容稳定；IM 主界面以列表/气泡/表单为主，
  Widgets 生态（Model/View）成熟；避免 QML 与 C++ 混合栈的学习成本。
- **影响**：Qt 5.12 无内置 H264 解码 → 媒体解码走 FFmpeg（本机已有 4.2.2）自行渲染到 `QOpenGLWidget`。
- **重审条件**：若后期 Android 端选择 Qt for Android 路线且需要新特性，再评估升 6.x。

## ADR-002 服务间通信：自研 RPC（Protobuf + Asio）⭐

- **背景**：Gate/Status/Chat/File/AI 五类服务需要互联。
- **备选**：gRPC（**本机 D:\cpp\grpc 有源码**）；HTTP/REST。
- **结论**：自研 RPC：Protobuf 序列化 + Asio 长连接 + 连接池 + 服务注册发现（挂靠 StatusServer）。
- **理由**：与全栈 boost 技术统一；自研服务发现/连接池/负载均衡/超时重试/熔断是本项目面试价值
  最高的模块之一；gRPC 引入新框架与代码生成链，且掩盖分布式细节。
- **影响**：需自研：注册心跳、requestId 匹配、双向 Push（AIServer 流式推送依赖）；
  超时/重试/熔断为最小可用集，不做跨语言与流控（见 01 文档 §4）。

## ADR-003 音视频媒体层：libdatachannel ⭐

- **背景**：1v1 音视频通话需要传输层与编解码。
- **备选**：自研 RTP over Asio（面试价值最高，工作量/风险最大）；自建 SFU（多人场景）。
- **结论**：libdatachannel（MIT，C++17 WebRTC 实现）+ Opus + OpenH264 + TURN(coturn)。
- **理由**：P2P 优先、NAT 穿透与加密开箱即用；能系统讲透 ICE/DTLS/SRTP；稳定性可控。
  自研 RTP 的回声消除、JitterBuffer、弱网对抗远超首期预算；SFU 属多人扩展。
- **影响**：M0 需经 vcpkg 引入；采集编解码用 Qt + Opus/OpenH264；解码渲染用 FFmpeg（本机已有）。
  自研 RTP 的核心思想（RTP 包结构、JitterBuffer 原理）仍写入 05 文档与面试题库作为知识储备。

## ADR-004 LLM 接入：OpenAI 兼容 API 优先 ⭐

- **备选**：本地模型优先（Ollama/llama.cpp）；两条通道都做。
- **结论**：AIServer 内置统一 LLM 网关，Provider 抽象 `ILlmProvider`；
  首个实现为 OpenAI 兼容 Provider（GLM/DeepSeek/Kimi 等均可配 Key 使用，SSE 流式）；
  本地模型 Provider 预留接口不实现。
- **理由**：零部署成本、效果最好、流式协议成熟；Provider 抽象保证后期可插拔。
- **影响**：API Key 经 `*.local.json` 配置，不入 git；网关层负责重试/降级/限流/用量统计。

## ADR-005 消息可靠性模型：会话 seq + 服务端权威 + 拉推结合 ⭐

- **备选**：纯推送（服务端保证送达，弱网下复杂）；时间戳排序（时钟不可靠）。
- **结论**：每个会话由服务端分配**单调递增 seq**（Redis INCR + MySQL 权威落库）；
  客户端按 seq 去重、排序、检测空洞；上线/重连后以「本地最大连续 seq」为游标补拉；
  客户端消息带 clientMsgId（UUID）做服务端幂等去重。
- **理由**：seq 是不丢、不重、不乱序的最小完备机制，也是多端漫游的基础；推拉结合弱网友好。
- **影响**：02 文档给出协议；SQLite 本地缓存以 (conv_id, seq) 为主键的一部分。

## ADR-006 群消息扩散：写扩散为主（≤500人群），大群读扩散留演进

- **备选**：全员读扩散（时间线模型）。
- **结论**：普通群采用写扩散（每成员会话游标推进）；>500 人群的读扩散方案写入演进文档不实现。
- **理由**：本项目群规模小，写扩散实现简单、离线补拉逻辑与单聊统一；面试时能讲清两者取舍即可。

## ADR-007 文件传输：HTTP 直传 FileServer，IM 通道只传元数据 ⭐

- **备选**：走 IM TCP 长连接分块传输。
- **结论**：文件（含图片/语音/视频/文档）统一 HTTP 直传 FileServer（分块 4MB、MD5 秒传、
  断点续传、Range 下载、缩略图）；IM 消息只携带 fid 与元数据。
- **理由**：HTTP 生态成熟（断点续传、Range、未来可上 CDN）；IM 通道保持轻量低延迟；
  大文件走长连接会挤占信令带宽。
- **影响**：FileServer 用 Beast 实现；存储接口抽象 `IFileStorage`（本地磁盘 / 可选腾讯 COS）。

## ADR-008 Gate 定位：仅 HTTP 接入，客户端直连 ChatServer ⭐

- 见 01 文档 §2。结论：Gate 只做注册/登录 HTTP；消息面由客户端与 ChatServer 直连。
- 理由：接入层与逻辑层分离，ChatServer 可独立扩容，消息面少一跳。

## ADR-009 存储选型：MySQL 8.0 + Redis（本机已有）+ 客户端 SQLite

- **结论**：MySQL 做权威存储；Redis 承担 token、路由表、seq 计数、群成员缓存、分布式锁、去重；
  客户端 SQLite 做消息/会话本地缓存（离线可看、增量同步）。
- **理由**：本机三者齐备；Redis 的 INCR/EXPIRE/SETNX 与 IM 场景高度契合；
  SQLite 是桌面端 IM 标配（微信/QQ 同思路）。
- **影响**：seq 的 Redis 与 MySQL 一致性策略见 03 文档 §3.4。

## ADR-010 MySQL 访问层：Connector C++ 8.3 + 自研连接池

- **备选**：原生 libmysqlclient C API。
- **结论**：使用本机已有的 mysql-connector-c++ 8.3.0（带 CMake config，接入顺畅），
  之上自研连接池（延迟初始化、健康检查、获取超时）。
- **理由**：C++ API 更安全（RAII、预编译语句防注入），且本机已就绪。

## ADR-011 AI 角色建模：复用用户体系，user_type 区分 ⭐

- **备选**：独立 AI 实体体系（独立表、独立管线）。
- **结论**：AI 角色就是「用户」：`t_user.user_type = 1`，拥有 uid/头像/签名，走同一条
  好友/会话/消息管线；AI 专属数据（角色卡、世界书、记忆、好感度）挂在扩展表。
- **理由**：「AI 角色即联系人」是本项目「完美融入 IM」的核心设计——消息管线、离线、漫游、
  群聊、通话信令全部零成本复用；扩展表隔离 AI 专属复杂度。
- **影响**：ChatServer 投递时按目标 user_type 分流；协议复用同一套消息结构。

## ADR-012 Swipe 存储模型：同 seq 多版本（versions 数组）⭐

- **备选**：每次重掷生成新消息（seq+1）。
- **结论**：AI 消息的多个候选回复存在**同一条消息的 `versions[]` 内**（JSON 数组 + activeIndex），
  重掷不消耗新 seq。
- **理由**：seq 承载的是「会话时序」，候选版本属于「同一轮对话」的内部状态；
  版本化不污染时间线，与酒馆原生体验一致；多端漫游时同步 activeIndex 即可。

## ADR-013 协议序列化：Protobuf（proto3）+ 自研帧头 ⭐

- **备选**：JSON（可读性好）；纯自定义二进制（极致紧凑）。
- **结论**：body 一律 Protobuf；帧头自研（12 字节，见 02 文档）。
- **理由**：强 schema 防字段漂移；体积小；proto 文件即协议文档；Android 端（protobuf-java /
  kotlinx）与 Web 均可复用；帧头自研保留心跳/压缩/加密 flag 的演进空间。

## ADR-014 HTTP 实现：Boost.Beast（自研轻路由）

- **备选**：cpp-httplib（引入第三方）；Poco（**本机有 1.15 源码**，框架重）；libcurl（仅客户端）。
- **结论**：Gate/FileServer/LLM 网关的 HTTP 全部用 Beast（属于 boost，统一栈），
  之上自研极简路由/JSON 工具。
- **理由**：零新增依赖；手写路由是理解 HTTP 服务端的练习；LLM SSE 需要底层控制权（Beast 有）。

## ADR-015 日志与配置：spdlog + nlohmann/json

- **备选**：glog；boost::log；yaml-cpp。
- **结论**：spdlog（异步滚动文件）+ nlohmann/json（配置与消息 content 信封）。
- **理由**：spdlog 性能与易用性最佳；nlohmann/json 单头文件、proto 消息的动态字段（消息
  content 信封、世界书词条）需要 schema-free 结构，与 Protobuf 分工明确：
  **信令与 RPC 用 Protobuf，content 信封与配置用 JSON**（见 03 文档 §2.3）。

## ADR-016 测试框架与压测工具

- **结论**：GTest 单元测试；自研 simbot（复用客户端 net/ 层的 C++ 协议机器人）做联调与压测；
  Python 脚本做协议黄金用例与造数；手工用例记录归档 docs/tests/。
- **理由**：simbot 复用客户端网络层 = 每次压测同时验证客户端协议栈；GTest 与 MSVC/CMake 集成无缝。

## ADR-017 构建系统：CMake（VS 自带）+ vcpkg

- **背景**：CMake 未独立安装，但 VS 18 自带；vcpkg 缺失；Boost/FFmpeg/MySQL Connector 本机已有。
- **结论**：根 CMakeLists + vcpkg 工具链；vcpkg 只补齐缺失项（protobuf、spdlog、hiredis、
  gtest、libdatachannel、openssl、sqlite3）；Boost/FFmpeg/Connector 走 find_package 指向本机路径。
- **理由**：最小安装成本；依赖清单进 git 保证可复现。
