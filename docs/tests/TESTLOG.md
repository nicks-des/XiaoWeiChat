# 测试记录总索引（TESTLOG）

> 执行约定见 [07-测试计划与记录.md](../07-测试计划与记录.md) §5。
> 每轮测试在此追加一节；缺陷登记在 [bugs.md](bugs.md)。

---

## 测试轮次索引

| 日期 | 里程碑 | 用例范围 | 通过/总数 | 缺陷单 | 明细 |
| --- | --- | --- | --- | --- | --- |
| 2026-10-05 | M0 | 全量（GTest 自动化） | 23/23（100%） | 过程中修复 3 处，均当场回归通过 | 见下节 |
| 2026-10-05 | M1 | 单测全量 + m1_flow 系统联调 | 单测 30/30；m1_flow 连续 5/5 PASS | 过程中修复 6 处，均当场回归通过 | 见下节 |

---

## 2026-10-05 M1 测试轮（第一轮归档）

- **环境**：DEV 单机三进程（statusserver:9000 / gateserver:8080 / chatserver:8888+9001）
  + MySQL:3316 + Redis:6379；联调程序 `tools/system_test/m1_flow.exe`。
- **单元测试：30/30 通过**（新增 Crypto 4 项：盐/哈希/校验/HMAC RFC4231 已知向量；
  RpcFrame 3 项：回环/截断/标志位）。
- **系统联调 m1_flow：连续 5/5 PASS**，覆盖：
  1. 注册成功（uid 雪花 ID 落库）；2. 重复用户名 409；3. 错误密码 401；
  4. 登录返回 HMAC token + ChatServer 分配（host/port）；
  5. TCP 长连接 token 校验登录成功（LoginResponse）；
  6. **顶号**：同 uid 第二处登录，第一处收到 KickNotice(0x0104)；
  7. 伪造 token（篡改签名）→ 401 拒绝。
- **稳定性**：三服务连续运行 65s+（跨越 2 个心跳周期）无崩溃；多轮联调后仍存活。
- **过程中发现并修复（全部回归通过）**：
  1. **Beast 响应 UAF**：`http::async_write` 只持有消息引用，栈上 response 写完即析构 →
     改 shared_ptr 持有至完成回调（Gate 响应后秒崩）；
  2. **心跳 lambda 悬垂**：递归 lambda 捕获栈上 `std::function` 引用，宿主函数返回后悬垂 →
     改成员函数递归 + shared_ptr 定时器（Chat 启动后整 30s 崩溃）；
  3. **顶号通知丢失（竞态）**：sendFrame 与 close 两次独立投递 strand 存在顺序风险 →
     新增 `sendFrameThenClose`（单任务内入队末帧+置关闭标志）；
  4. **Windows close 丢数据**：写完即 close 会 RST 丢弃未刷出数据 → 优雅关闭
     （flush 后 shutdown_send 发 FIN，延迟 2s close）；
  5. **asio io_context 复用**：跑空后 stopped，复用必须 `restart()`（联调程序第二次 run 立即返回）；
  6. **MySQL salt 字段**：DDL CHAR(16) 放不下 32 位十六进制盐 → CHAR(32)（文档同步修订）。
- **联调方法学**：Python 裸 socket 复现脚本用于区分「服务端行为」与「测试程序自身」问题——
  本轮两次成功定位（服务端正常 → 疑点收窄到客户端；字节层 RST → 确认关闭时序）。

---

## 2026-10-05 客户端自动验收轮（M1 补充 T10-07/08）

- **对象**：`lingxi_client.exe --autotest`（Qt Widgets 无人值守模式：自动生成账号 → 注册 →
  登录 → TCP 长连接校验 → 主面板展示）。
- **结果**：**CLIENT_AUTO_LOGIN_PASS**（exit 0）；对运行中三服务完成真实注册(uid 落库)、
  HTTP 登录(token+分配)、TCP LoginRequest/LoginResponse、主面板切换。
- **回归**：单元测试 30/30 通过。
- **过程中修复**：连接建立后未发送 LoginRequest（流程断链）——连接成功回调内补发登录帧。

---

## 2026-10-06 M3 好友与群组测试轮

- **单元测试**：30/30 通过；**客户端自动验收**：CLIENT_AUTO_LOGIN_PASS（连跑 4 次稳定）；
  **M2 回归**：m2_flow PASS。
- **M3 全流程系统验证 m3_flow：M3_FLOW_PASS（9 场景一次通过）**
  1. 用户名前缀搜索到目标（未加好友状态正确）；
  2. A 申请 → B 收 0x0203 通知（含 apply_id/昵称/验证消息）→ B 同意 → A 收 0x0205 结果（附对方资料）；
  3. 双方好友列表互见；
  4. A 建群拉 B → B 收 0x0403 入群通知（全量成员 2 人）；
  5. 群消息写扩散：A 发 B 收、B 发 A 收，from_uid 归属正确；
  6. B 退群 → A 收 0x0408(action=1)；**退群后群消息不再投给 B**（轮询 1s 确认零投递）；
  7. A（群主）解散 → 0x0408(action=3) 广播；
  8. 删除好友 → 双向列表均不再包含对方。
- **过程中发现并修复（已回归）**：
  1. **连接池半开连接（隔夜必现）**：机器休眠后池内 MySQL 连接全部死亡，acquire 直出导致
     [2013] Lost connection → 修复：取出时 ping 健康检查，坏连接丢弃重建；
  2. **客户端 SocialService 未初始化**：requestFriendList 空指针段错误（偶发）→
     AccountService::setup 时注入 TcpClient；
  3. protoc 生成访问器命名与手写不符两处（驼峰字段 isFriend→isfriend、保留字 friend→friend_）→
     proto 字段统一改标准 snake_case；代码里 mutable_friend_()。
- **说明**：服务重启后首轮 m2_flow 出现一次瞬时网络失败，重跑即过（无复现，判定为
  服务暖机抖动，不记缺陷单）。

---

## 2026-10-06 M4 文件服务测试轮

- **FileServer 冒烟（Python 裸 HTTP）**：分块上传/不完整 complete 拒绝/同会话断点续传/
  MD5 篡改拒绝合并/Range 206 断点读/秒传命中——全部通过。
- **系统验证 m4_flow：M4_FLOW_PASS（6 场景）**
  1. 5MB 随机文件分块上传（4MB+1MB 两块）；
  2. 下载回读字节级一致（round-trip）；
  3. Range 读尾部 1MB（206 语义）；
  4. 同 md5+size 二次预检 → 秒传命中同一 fid；
  5. 块数不足时 complete 拒绝；
  6. 内容与声明 MD5 不符时合并拒绝且不留脏文件。
- **全量回归**：m2_flow / m3_flow / 客户端自动验收 / 单测 30/30 全部通过。
- **过程中发现并修复（均已回归）**：
  1. **Windows SO_RCVTIMEO/SO_SNDTIMEO 为 DWORD 毫秒**（Linux 是 timeval）——传 timeval
     被解释成 30ms，4MB 大响应必超时。客户端与服务端 alike，教材级跨平台坑；
  2. **HttpServer 路由未剥离查询串**——带 query 的目标匹配不到精确路由 → findHandler
     先剥离 `?` 后段；
  3. **流式 MD5 缺 EVP_DigestInit_ex**——EVP_MD_CTX_new 只建空上下文，Update 直接失败；
  4. **前缀路由第二参数语义**——处理器拿的是 clientIp 不是 target，下载路径改为
     从 request.target() 自取并剥 query；
  5. **t_login_session.token CHAR(64) 过短**——token 实为 uid.expire.hmac 约 100 字符，
     登录流水入库静默失败（1406）→ VARCHAR(128)；
  6. MSBuild 增量构建偶发吞产物：回归前一律全量 build 并以 exe 时间戳为准。
- **遗留**：下载大文件整段入内存（M8 改 file_body/流式）；缩略图生成（stb）与语音录制
  （QAudioSource+Opus）排入 M5 前置小迭代；上传会话位图跨重启持久化。

---

## 2026-10-06 M5 通话信令面测试轮

- **系统验证 m5_flow：M5_FLOW_PASS（8 场景一次通过）**
  1. 邀请 → 被叫振铃（Offer 中继 + 主叫昵称填充）；
  2. 接听 → Answer 中继回主叫；
  3. ICE 候选 / SDP 重协商双向中继（越权防护：非通话成员不可注入）；
  4. A 挂断 → B 收通知（时长取 max(上报, 服务端结算)=12s）；
  5. C 邀 B → B 拒绝 → C 收 reason=0 通知；
  6. 占线双向：B 振铃中（主叫身份）被 A 邀 → 409；B 已有主叫会话再邀人 → 409；
  7. C 邀 B → C 取消 → B 收 0x0609(state=3)；
  8. **t_call_record 落库核验**：state=1(接通,12s)/state=2(拒绝)/state=3(取消) 记录齐全。
- **全量回归**：m2/m3/m4/客户端自动验收/单测 30/30 全部通过。
- **过程中修复（均已回归）**：
  1. **use-after-move**：session.emplace(std::move(session)) 后继续读 session.callId/offerSdp
     （string 被搬空）→ 通知构造移到 move 之前；
  2. **占线判定漏主叫身份**：仅查 calleeUid 遗漏「目标是振铃中主叫」→ 改对称参与者判定；
  3. 通话时长语义：服务端结算与挂断方上报取较大者（容忍时钟误差）。

---

## 2026-10-07 M5 媒体面测试轮

- **系统验证 m5_media：M5_MEDIA_PASS（连跑 3 次稳定）**
  双端 WebRtcPeer 经真实信令交换 SDP/ICE → **ICE/DTLS/SRTP 全链路建立**（双方 PeerConnection
  进入 Connected 态）→ 完整挂断信令收尾。音频编解码关闭（无采集设备场景），连接态即验证
  WebRTC 传输栈；Opus 编解码在真机客户端启用。
- **客户端媒体栈落地**：WebRtcPeer（libdatachannel PeerConnection + Opus 编解码 +
  AudioRtpPacketizer/SR/NACK 链）、CallManager（信令编排 + QAudioInput 20ms 采集 +
  QAudioOutput 推放）、CallWindow（来电/呼叫中/通话计时 UI）、MainPanel 语音通话按钮。
- **过程中发现并修复（均已回归）**：
  1. **无媒体轨 PC 无法生成 SDP** → 音轨始终创建，withAudio 仅控制编解码器；
  2. **早到候选竞态**：A 的 ICE 候选可能先于 B 的 setRemoteDescription 到达而被 libdatachannel
     丢弃 → WebRtcPeer 实现候选缓冲（markRemoteDescriptionSet 后统一冲刷）——WebRTC 标准模式；
  3. **libdatachannel API 三连**：vcpkg port 的 srtp feature 才控制 NO_MEDIA（media feature 不存在）、
     AudioRtpPacketizer 是模板（OpusRtpPacketizer=48kHz 别名）、rtc::binary 是 std::byte 容器；
  4. **后台线程异常防护**：WebRtcPeer 回调与 ICE 泵全部 try/catch（asio io 线程的未捕获异常
     会 terminate 整个进程，退出码 3）。
- **全量回归**：m2/m3/m4/m5_flow/客户端/单测 30/30 全部通过。

---

## 2026-10-07 M6 酒馆一期测试轮

- **系统验证 m6_flow：M6_FLOW_PASS（全链路一次贯通）**
  1. 用户消息落库（含世界书触发词「蛊」）→ ACK；
  2. AI 占位消息即时通知（from=白露, status=2, seq=user+1 预占）；
  3. mock LLM 流式分片 277 字符（0x0701，UTF-8 字符边界感知切割）；
  4. 结束帧（0x0702）：权威全文 162 字符、**好感度标记已剥离**、affinity=+1；
  5. 正式消息（0x0303 status=0）payload 携带 versions[]（Swipe 结构就位）；
  6. **DB 核验**：t_message.status=0 + versions JSON ✓；t_ai_conversation_ext.affinity=1 ✓。
- **全量回归**：m2/m3/m4/m5_flow/m5_media/客户端验收/单测 30/30 全部通过（连跑 2 轮稳定）。

---

## 2026-10-07 M6 收尾测试轮（PNG 角色卡）

- **单测**：**35/35 通过**（新增 PngCard 5 项：base64 回环含中文、嵌入→提取回环、旧卡替换、
  无卡返回 false、非 PNG 容错）。
- **全量回归**：m2/m3/m4/m5_flow/m5_media/m6_flow/客户端验收全部通过。
- **说明**：makeTestPng 初版 IDAT/IEND chunk 长度前缀写成了 ASCII 字符串而非二进制大端 →
  embedCardJson 遍历越界返回 false → 修复为统一 appendChunk 辅助（4 字节大端长度）。
- **过程中发现并修复（均已回归）**：
  1. **RpcClient connect 悬垂 promise（教材级）**：async_connect 回调按引用捕获栈上 promise，
     超时路径下栈帧销毁 → 回调写悬垂内存 → abort（退出码 3）→ promise 移入 shared_ptr 由回调持有；
  2. **mock 分片劈开 UTF-8 多字节字符**：protobuf string 字段报 invalid UTF-8 → 分片切割
     改为 UTF-8 字符边界感知；
  3. **Gate 限流 20/10s 拦截回归套件** → rateLimit 可配置（dev=200）；
  4. **同 call_id 重复邀请覆盖失败**：m_sessions.emplace 不覆盖残留会话 → insert_or_assign；
  5. MSBuild 增量构建吞产物（本里程碑第 3 次）→ 回归前全量构建已立为铁律。
- **客户端体验说明**：真实 Key 配置 llm.provider=openai + apiKey 后即接 GLM 等兼容 API；
  mock provider 为自动化测试确定性后端。

---

## 2026-10-05 M2 消息内核测试轮

- **环境**：DEV 单机三进程 + MySQL:3316 + Redis:6379；联调 `tools/simbot/m2_flow.exe`（复用客户端网络层）。
- **单元测试**：30/30 通过。
- **客户端回归**：`lingxi_client.exe --autotest` → CLIENT_AUTO_LOGIN_PASS（含会话列表拉取与增量同步路径）。
- **可靠性系统验证 m2_flow：M2_FLOW_PASS（全部场景一次通过）**
  1. A 连发 10 条 → 10 个 ACK，seq 1..10 严格连续（R3/R6 seq 权威与排序）；
  2. B 在线按序收到 10 条，seq 与 payload 与 ACK 完全一致（R1 先落库后投递）；
  3. B 断线 → A 再发 5 条 → 全部落库（seq 11..15）；
  4. B 重连重登 → SyncRequest(lastSeq=10) 恰好补到 5 条且 has_more=false（R4 补拉）；
  5. 同 clientMsgId 重发 → ACK 复用原 seq（R2 幂等，Redis 快路径 + MySQL 唯一键兜底）。
- **过程中修复**：ChatServer 构造顺序错误（MessageService 拿到未初始化的 Redis 池 → 空指针崩溃）。
- **UI 说明**：聊天窗口 v1（会话列表/消息流/发送/未读/发起会话）需要人工体验，
  自动化覆盖了其依赖的 ConversationService 全部协议路径。

---

## 2026-10-05 M0 全量测试轮（第一轮归档）

- **环境**：DEV 单机；Windows 11 x64；VS 18（v143 工具集）+ CMake 4.2.3 + Qt 5.12.11(msvc2017_64)
  + Boost 1.90 + vcpkg（protobuf 33.4.0 / spdlog 1.17.0 / hiredis / gtest 1.18 / nlohmann-json 3.12）；
  开发服务：MySQL 8.0.28 @127.0.0.1:3316（专属实例）、Redis 5.0.14 @6379（专属目录）。
- **命令**：`cmake --build build --config Debug` → `ctest -C Debug`（测试可执行 `build/bin/Debug/test_lingxi_common.exe`）。
- **结果：23/23 全部通过（100%），总耗时 1.4s**
  - Packet/BufReader（TC 对应 R 类）：编码回环、整包、逐字节半包、双包粘包、半包+整包混合、
    脏长度丢弃、短头等待 —— 7/7 ✅
  - ThreadPool：结果返回、1000 并发任务、异常传递 —— 3/3 ✅
  - Snowflake：10 万 ID 唯一且单调、4 线程 4 万无重复、非法机器号拒绝 —— 3/3 ✅
  - Config：加载/默认值回退、local 深合并、非法 JSON 拒绝、缺文件容错 —— 4/4 ✅
  - **MySqlPoolTest.PingAndBasicQuery：真实连接 3316 实例 SELECT 40+2=42 ✅（集成）**
  - **RedisPoolTest.SetGetRoundtrip / PoolBalance：SET/GET/TTL 回环 + 池借还平衡 ✅（集成）**
  - Proto（protoc 33.4.0 生成）：MsgBody 回环（含中文 payload）、SyncRequest 嵌套、未知字段容忍 —— 3/3 ✅
- **编译链验证（T00-03）**：`boost_echo.exe` → `BOOST_ECHO_PASS`（exit 0）；
  `qt_hello.exe` → 窗口正常创建、2 秒自动退出（exit 0；运行需 Qt bin 在 PATH）。
- **过程中发现并修复（全部回归通过）**：
  1. 连接池空闲队列误用 shared_ptr::release() → 改 unique_ptr 所有权模型，并修复并发建连
     超发竞态（预占名额再解锁建连）；
  2. dev.json 头部写了 C 风格注释导致 JSON 解析失败 → 移除（JSON 无注释语法）；
  3. RedisReply::str() 未处理 STATUS 类型回复（PING→PONG 被判失败）→ 兼容 STRING/STATUS/ERROR；
  4. 测试自身两处 bug：脏数据用例的 std::string 字面量在嵌入 NUL 处被 strlen 截断（改显式长度构造）；
     Config 深合并用例两个临时文件同名互相覆盖（改带序号文件名）。

<!-- 明细节模板：
## 2026-XX-XX M2 消息内核联调轮
- 环境：DEV（单机全服务）+ simbot v1
- 结果：TC-M2-001 ✅ / TC-M2-002 ✅ / TC-M2-007 ❌（BUG-001）
- 备注：...
-->
