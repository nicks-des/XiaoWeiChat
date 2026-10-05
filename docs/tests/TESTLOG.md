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
