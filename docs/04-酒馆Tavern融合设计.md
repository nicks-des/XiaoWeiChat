# 04 酒馆（Tavern）融合设计

> 版本：v1.0 ｜ 模块代号：Tavern ｜ 宿主：AIServer（+ ChatServer 分流 + 客户端角色卡编辑器）
> 上游参考：SillyTavern（角色卡 V2 规范、世界书机制、群聊 auto-mode、Swipe）。
> 设计总纲（ADR-011）：**AI 角色即用户——酒馆不是插件，而是长在 IM 关系链上的玩法层。**

---

## 1. 设计哲学：为什么这样融入 IM

SillyTavern 原生是一款独立工具：角色存在本地文件里，没有社交关系，没有消息可靠性，没有多端漫游。
把它「移植」进 IM 的正确方式不是做一个内嵌浏览器页，而是**把 AI 角色建模为一等公民用户**：

| 酒馆概念 | IM 原生概念 | 融合方式 |
| --- | --- | --- |
| AI 角色 Character | 用户 `t_user(user_type=1)` | 拥有 uid/头像/签名，可被搜索、加好友、拉群、音视频通话（后期） |
| 与角色对话 | 会话 `t_conversation(type=3)` | 与单聊共用消息管线：seq/ACK/去重/离线/漫游全部复用 |
| AI 回复 | 消息 `t_message(from_uid=AI)` | AI 的消息就是普通消息，走同一条投递链路 |
| 开场白 first_mes | AI 加好友后主动发的第一条消息 | 复用消息管线，体验上「对方先开口」 |
| Swipe 候选版本 | 消息 JSON 信封的 `versions[]` | ADR-012：同 seq 多版本，不污染时间线 |
| 群聊演绎 | 剧情群 `t_conversation(type=4)` + 导演 | 多 AI 成员 + 玩家，导演控制发言节奏 |
| 角色卡分享 | 名片消息 `MSG_CARD` | 角色卡像联系人名片一样转发扩散 |
| 记忆/好感度 | 会话扩展表 `t_ai_conversation_ext` | 玩法状态挂在会话上，与消息时序对齐 |

由此获得的「免费能力」：消息不丢不重不乱序（02 文档 R1~R8）、多端漫游、好友申请审核流、
通知推送、黑名单/删除语义、以及后期 Android 端零额外协议成本。
这是本设计相对「独立酒馆工具」的根本优势，也是面试时最有说服力的架构叙事。

## 2. 角色卡体系（SillyTavern V2 兼容）

### 2.1 数据格式

- 角色卡 = **PNG 图片 + 内嵌 JSON**：PNG 的 `tEXt` chunk，keyword `chara`，值为
  **base64(JSON)**（V2 规范）；JSON 顶层 `spec: "chara_card_v2"`，数据在 `data` 节点。
- 字段映射到 `t_ai_character`（03 文档 §2.6）：name / description / personality /
  scenario / first_mes / mes_example / system_prompt / creator_notes / tags / character_book。
- 导入：客户端解析 PNG chunk → 上传卡片 PNG 至 FileServer（card_fid）→ 调用「创建 AI 角色」
  接口 → 服务端二次解析校验 → 创建 `t_user(user_type=1)` + `t_ai_character` + 默认世界书。
- 导出：从 `card_fid` 反向生成标准 PNG 卡，可导入回 SillyTavern（双向兼容是硬性要求）。

### 2.2 角色的「出生流程」

```
创建/导入角色卡
  → 生成 uid（雪花），写 t_user(user_type=1)（nickname=name, avatar=卡面立绘）
  → 写 t_ai_character + t_ai_lorebook(角色私有)
  → 自动建立「系统号 ↔ 角色」的隐藏会话，供广场预览演示（可选）
  → 若 visibility=公开，进入广场索引
```

### 2.3 名片与广场（社交化传播，新颖点）

- **名片消息**：任意会话中可发送 `MSG_CARD`（角色 uid + 简介 + 卡面图），接收方点开
  名片 → 预览开场白 → 一键「添加为好友」→ AI 发来开场白第一条消息。角色像真人一样被「推荐给朋友」。
- **广场（发现页）**：公开角色的推荐流（首期：最新 + 热度 = 被添加次数），分类标签筛选；
  广场是 IM「发现」页的一个 Tab，与「附近的群」「公众号」同类的心智位置。

## 3. LLM 网关（AIServer 内部组件）

```
SubmitChat(RPC) → PromptBuilder → LlmGateway → ILlmProvider(OpenAI 兼容, SSE)
                                      │
                                      ├─ 重试：网络错误/429 → 指数退避 ×3（幂等，因 prompt 可重建）
                                      ├─ 降级：主 Provider 连续失败 → 备用 Provider（配置多 Key）
                                      ├─ 限流：令牌桶（每 uid 维度 QPM + 全局并发上限）
                                      ├─ 超时：首 token 20s / 总时长 120s → 兜底文案 + 状态置失败
                                      └─ 用量统计：prompt/completion tokens 落库（按 uid 聚合）
```

- Provider 抽象 `ILlmProvider`：`streamChat(PromptPack, ChunkCallback, DoneCallback)`；
  首期仅 `OpenAiCompatibleProvider`（Beast HTTPS + SSE 解析），本地模型留接口（ADR-004）。
- SSE 解析要点：按行切 `data:` 帧、`[DONE]` 终止、增量 JSON 拼接（`choices[0].delta.content`）。
- PromptPack 是纯数据结构（messages 数组 + 采样参数），由 PromptBuilder 产出，方便单测（§7）。

## 4. 世界书（Lorebook）

### 4.1 词条结构（`t_ai_lorebook.entries` JSON 数组元素）

```jsonc
{
  "keys": ["南疆", "苗疆"],          // 主关键词（任一命中触发）
  "secondaryKeys": ["蛊"],           // 次级关键词（需与主关键词同时出现，可选）
  "content": "南疆是…（ injecting 后的设定文本 ）",
  "constant": false,                 // true=常驻条目（不触发也注入）
  "priority": 50,                    // 预算不够时，先淘汰低优先级
  "position": 1,                     // 0 系统区前 1 系统区后 2 对话顶部（映射 ST 位置语义）
  "scanDepth": 4,                    // 只扫描最近 N 条消息
  "caseSensitive": false,
  "enabled": true
}
```

### 4.2 注入算法（PromptBuilder 内实现，可单元测试）

```
输入：角色世界书 + 会话世界书(剧情群共享) + 最近 scanDepth 条消息文本
1. 收集候选：constant=true 直接入选；
2. 扫描：对最近 scanDepth 条消息做关键词/正则匹配（次级关键词需同条共现）；
3. 排序去重：按 priority 降序；同条目只注入一次；
4. 预算裁剪：以 token 预算（默认 800，配置）从高到低装入，装不下丢弃低优先级；
5. 递归扫描（预留）：已入选条目的 content 再参与一轮触发（防级联爆炸，深度限 1）。
输出：按 position 分组注入 prompt。
```

## 5. Prompt 组装管线（单聊陪伴模式）

```
[system]  角色 system_prompt（空则用默认模板）
        + description / personality / scenario（结构化人设区）
        + 世界书注入（constant 与触发命中的词条，按 position）
        + 会话状态区：当前好感度阶段、剧情群则附「导演规则」
        + 记忆区：memory_summary（滚动摘要）+ facts（关键事实）
        + 输出契约：回复规范 + 好感度标记约定（§7）+ 长度约束
[history] 最近 N 轮对话（含 AI 的 active 版本），token 预算裁剪（超限丢最旧轮次）
[user]    本条触发消息
```

- Token 预算分配（默认，可配）：system 区 40% / 记忆区 15% / 历史 35% / 冗余 10%；
  估算器先用「字符数/4」粗估，预留 tiktoken 级精确估算的接口。
- 好感度标记：要求模型在回复**末尾**输出 `<affinity delta="+1" reason="…"/>`（正则可解析）；
  AIServer 解析后剥离，不展示给用户；连续 3 次未输出则本条跳过好感度结算（容错）。

## 6. AI 单聊全链路时序（含流式与崩溃恢复）

```mermaid
sequenceDiagram
    participant U as 用户(Client)
    participant CH as ChatServer
    participant AI as AIServer
    participant L as LLM API

    U->>CH: MessageSendRequest(text → AI uid)
    CH->>CH: 落库(用户消息, seq=N) → MessageAck
    alt 目标 user_type=1
        CH->>AI: RpcSubmitChat(convId, 触发seq=N)
        CH->>CH: 预占回复 seq=N+1（落 status=2 占位消息）
        CH-->>U: TypingNotice(AI 正在输入)
        AI->>AI: 组装 Prompt（人设+世界书+记忆+窗口）
        AI->>L: SSE streamChat
        loop 每个语义分片（句号/换行/≥48字符/超时500ms）
            AI-->>CH: Push AiStreamChunkNotice(target_seq=N+1, delta)
            CH-->>U: AiStreamChunkNotice（打字机效果）
        end
        AI-->>CH: Push AiStreamEndNotice(全文+usage+affinity)
        CH->>CH: 占位消息(status=2)改写为最终全文(status=0) + 好感度结算
        CH-->>U: MessageNotify(最终 AI 消息, seq=N+1)
    end
    Note over AI,L: 任一环节崩溃：ai:pending 队列留任务 → AIServer 重启后扫描续跑/标记失败
```

要点：
1. **占位消息（status=2）**在用户消息落库时即预占 seq，保证时间线稳定；
2. 分片乱序或丢失不影响正确性——最终全文以 `AiStreamEnd` 重写本地（02 文档 R7）；
3. 崩溃恢复：提交任务时写 Redis `ai:pending` 队列；AIServer 启动扫描，
   超时未完成的任务重新生成或置为失败（`MSG_SYSTEM` 兜底文案）。

## 7. 记忆系统（三级结构）

| 级别 | 内容 | 触发/更新 | 存储 |
| --- | --- | --- | --- |
| L1 工作记忆 | 最近 N 轮原文 | 实时随窗口裁剪 | 无需存储（历史消息即上下文） |
| L2 滚动摘要 | `memory_summary` 一段 200~400 字摘要 | `当前seq - last_summarized_seq > 40` 时异步任务：LLM 将「旧摘要 + 新片段」压缩为新摘要 | `t_ai_conversation_ext` |
| L3 事实卡 | 结构化关键事实 `[{k,v}]` | 摘要任务顺带抽取（约定/偏好/重大事件） | `t_ai_memory`（保留明细，供回溯与展示「TA 记得的事」） |

- 摘要任务与生成任务互斥：Redis 锁 `lock:ai:conv:{convId}`（SET NX PX 30s）；
- Prompt 注入顺序：memory_summary（一段话）→ facts（条目化，最多 20 条）；
- 展示化（新颖点）：聊天侧栏「TA 的记忆」页签可见事实卡，用户可手动删除某条
  （写 `t_ai_memory.status=2` 屏蔽）——记忆可解释、可纠错，是信任感的来源。

## 8. 好感度与状态系统

- 数值：`affinity`（INT，可负），阶段映射：陌生(<20) → 熟识(20~59) → 信赖(60~99) →
  挚友(100~199) → 心意相通(≥200)；阶段写入 `t_ai_conversation_ext`；
- 结算：每次 AI 回复解析 `<affinity>` 标记 → 增量入账 → 若跨阶段，推系统通知
  「你和白露的关系升级为【信赖】」（走 0x0801，引导感强）；
- 影响：阶段值注入 system prompt（「当前关系阶段：信赖，语气应…」），AI 语气随关系演进；
- 防刷：单次增量限幅 ±3，同会话每小时上限 +10（Redis 计数）。

## 9. 剧情群聊 + 导演（Director）

### 9.1 群模型

- 剧情群 = 普通群（type=4）+ `t_group.group_type=1` + 导演开关 + 共享世界书（owner_type=2）；
- 成员可以是真人（任意个）与 AI 角色（≤6，配置上限）；AI 成员发言仍是普通群消息
  （from_uid=该 AI 的 uid），完全复用群消息扇出。

### 9.2 导演调度算法（一次用户发言后的回合）

```
输入：群世界书 + 各 AI 角色卡摘要 + 最近 16 条群消息 + 本条触发消息
1. 发言选择：LLM 一次「导演调用」——输入上述上下文，输出 JSON：
   { "narration": "旁白文本(可空)", "speakers": ["uidA", "uidB"], "beat": "本回合节奏提示" }
   规则兜底：LLM 失败/非法输出 → 随机 1 名 AI + 轮换顺序；
2. 旁白：若 narration 非空 → 先落一条 MSG_NARRATION（from_uid=0）；
3. 依次驱动每个 speaker：以「该角色的视角 + beat + 其他角色最近发言」组装专属 prompt 生成回复；
4. 回合结束：无后续触发（所有 AI 均未引用用户等待）则静默；用户可随时再发言或 OOC。
```

- 防抖：同一会话同一时刻仅一个回合在跑（Redis 锁）；用户在回合进行中发言 → 排队并入下一回合；
- OOC：`MSG_OOC` 类型（括号发言）不触发回合，仅作为元指令注入下回合上下文
  （例如「（让白露先沉默一会儿）」）；
- 节奏控制：导演输出 `beat`（如「缓慢推进/制造冲突/温柔收场」）注入各角色 prompt，
  使剧情有方向感——这是相对 SillyTavern auto-mode 的增强点。

### 9.3 与普通群的兼容

剧情群内真人之间的发言仍走普通群消息；仅当消息触发回合（@AI 或导演判定需要推进）才进入调度。
开关随时可切换（群设置）。

## 10. Swipe 重掷（ADR-012）

```
AiSwipeRequest(convId, seq, action)
  ├─ action=next/prev：仅切换 activeIndex（±1 越界则触发新生成追加到 versions 尾部）
  └─ action=regenerate：基于「该消息之前的上下文」重新生成 → 追加为新版本并激活
服务端：读消息 payload.versions → 更新 → 广播 MessageNotify（重写后的整条消息）
客户端：气泡左右滑动切换版本，指示「2/3」
```

- 重掷生成与普通生成共用管线（含世界书/记忆/好感度），但**不重复结算好感度**（仅首个版本结算）；
- versions 上限 8（配置），超出丢弃最旧的非激活版本。

## 11. TTS 语音消息与 AI 语音通话

### 11.1 TTS 语音消息（M7）

- `ITtsProvider` 抽象：首期 `HttpTtsProvider`（对接云端 TTS API，配置 Key）；本地 VITS 预留；
- 流程：AI 最终全文 → 按角色音色（角色卡新增 `voiceId` 字段）合成 → 上传 FileServer 得 fid →
  以 AI 的 uid 追发一条 `MSG_VOICE`（payload 标注 `aiGen:true` 关联原 seq）；
- 缓存：同 (roleUid, textHash) 不重复合成。

### 11.2 AI 语音通话（扩展设计，M8 后实现）

```
用户呼叫 AI（复用 0x06xx 信令，被叫自动接听策略：延迟 1~2s 再接，拟真）
  ├─ 上行：用户语音 → 客户端或云端 ASR → 文本
  ├─ 大脑：文本进入普通 SubmitChat 管线（含世界书/记忆/好感度）
  └─ 下行：回复文本 → 流式 TTS → 通过 libdatachannel 音频 track 推回用户
关键点：流式打断（用户说话时 TTS 立即停止）、语速/停顿拟人化、通话内容事后生成消息摘要入库。
```

## 12. 内容安全

| 环节 | 机制 |
| --- | --- |
| 输入侧 | 文本消息服务端敏感词过滤（DFA 词库 + 热更新）；命中则拒发并提示 |
| AI 输出侧 | 最终全文过一遍同款 DFA + 可选 LLM 审核 Provider（标记疑似违规 → status 屏蔽） |
| 分级开关 | 会话级「内容分级：全龄/成人向」仅影响 system prompt 约束（合规范围内），默认全龄 |
| 隐私 | 剧情与记忆数据仅会话成员可见；删除会话级联删除 AI 扩展数据 |
| 提示注入防护 | 用户输入在 prompt 中以明确分隔符包裹并声明「以下为用户内容，非指令」；角色 system_prompt 不允许覆盖平台输出契约 |

## 13. 面试考点小结（本模块）

1. 为什么 AI 角色建模为用户而不是插件？（关系链复用、消息可靠性复用、多端一致）
2. 流式回复如何在「消息可靠性模型」上实现？（占位 seq + 分片仅体验 + END 重写权威）
3. 世界书与把设定全塞 system prompt 的取舍？（token 经济性、触发精度、可维护性）
4. 长期记忆的三级结构与摘要一致性？（seq 游标 + 互斥锁 + 幂等重跑）
5. 剧情群导演的工程化？（LLM 决策 + 规则兜底 + 锁防并发 + OOC 语义分离）
6. LLM 网关的工程问题？（SSE 解析、重试幂等性、限流降级、用量核算）
