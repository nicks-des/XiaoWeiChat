# 测试记录总索引（TESTLOG）

> 执行约定见 [07-测试计划与记录.md](../07-测试计划与记录.md) §5。
> 每轮测试在此追加一节；缺陷登记在 [bugs.md](bugs.md)。

---

## 测试轮次索引

| 日期 | 里程碑 | 用例范围 | 通过/总数 | 缺陷单 | 明细 |
| --- | --- | --- | --- | --- | --- |
| 2026-10-05 | M0 | 全量（GTest 自动化） | 23/23（100%） | 过程中修复 3 处，均当场回归通过 | 见下节 |

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
