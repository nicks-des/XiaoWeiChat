-- =====================================================================
-- @file 002_message_kernel.sql
-- @brief M2 消息内核建表：会话/成员/消息（完整设计见 docs/03-数据库与存储设计.md §2.2）
-- 执行：mysql -h 127.0.0.1 -P 3316 -u root < sql/002_message_kernel.sql
-- =====================================================================
USE lingxi;

CREATE TABLE IF NOT EXISTS t_conversation (
  id          BIGINT UNSIGNED PRIMARY KEY,            -- 雪花 ID
  type        TINYINT      NOT NULL,                  -- 1 单聊 2 群聊 3 AI单聊 4 剧情群
  member_key  VARCHAR(64)  NOT NULL DEFAULT '',       -- 单聊: "minUid_maxUid"；群聊为空
  name        VARCHAR(64)  NOT NULL DEFAULT '',
  owner_uid   BIGINT UNSIGNED NOT NULL DEFAULT 0,
  last_seq    BIGINT UNSIGNED NOT NULL DEFAULT 0,     -- 冗余最后 seq，会话列表加速
  last_msg_at DATETIME(3)  NULL,
  created_at  DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
  UNIQUE KEY uk_member_key (member_key),
  KEY idx_owner (owner_uid)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='会话表（单聊/群聊/AI统一）';

CREATE TABLE IF NOT EXISTS t_conversation_member (
  conv_id       BIGINT UNSIGNED NOT NULL,
  uid           BIGINT UNSIGNED NOT NULL,
  role          TINYINT      NOT NULL DEFAULT 2,      -- 0 群主 1 管理员 2 成员
  last_read_seq BIGINT UNSIGNED NOT NULL DEFAULT 0,   -- 已读游标（未读数 = last_seq - last_read_seq）
  muted_until   DATETIME     NULL,
  joined_at     DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (conv_id, uid),
  KEY idx_uid (uid)                                   -- 查「我的会话列表」
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='会话成员表（含已读游标）';

CREATE TABLE IF NOT EXISTS t_message (
  conv_id       BIGINT UNSIGNED NOT NULL,
  seq           BIGINT UNSIGNED NOT NULL,               -- 会话内序号，与 conv_id 构成逻辑主键
  msg_id        BIGINT UNSIGNED NOT NULL,               -- 雪花 ID（撤回/引用寻址用）
  client_msg_id CHAR(36)     NOT NULL,                  -- 幂等 ID
  from_uid      BIGINT UNSIGNED NOT NULL,
  msg_type      TINYINT      NOT NULL,
  status        TINYINT      NOT NULL DEFAULT 0,        -- 0 正常 1 撤回 2 生成中 3 截断
  payload       JSON         NOT NULL,                  -- docs/02 §3.2 JSON 信封
  created_at    DATETIME(3)  NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
  PRIMARY KEY (conv_id, seq),
  UNIQUE KEY uk_client (conv_id, client_msg_id),
  KEY idx_msg_id (msg_id),
  KEY idx_from_time (from_uid, created_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='消息表（分表演进见 docs/03 §5.3）';
