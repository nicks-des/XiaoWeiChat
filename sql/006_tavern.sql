-- =====================================================================
-- @file 006_tavern.sql
-- @brief M6 酒馆一期建表（完整设计见 docs/03-数据库与存储设计.md §2.6）
-- 执行：mysql -h 127.0.0.1 -P 3316 -u root < sql/006_tavern.sql
-- =====================================================================
USE lingxi;

CREATE TABLE IF NOT EXISTS t_ai_character (
  uid            BIGINT UNSIGNED PRIMARY KEY,          -- 即 t_user.id（user_type=1）
  card_version   VARCHAR(16)  NOT NULL DEFAULT 'v2',
  name           VARCHAR(64)  NOT NULL,
  description    TEXT         NULL,                    -- 人物设定
  personality    TEXT         NULL,                    -- 性格摘要
  scenario       TEXT         NULL,                    -- 场景设定
  first_mes      TEXT         NULL,                    -- 开场白
  mes_example    TEXT         NULL,                    -- 示例对话
  system_prompt  TEXT         NULL,                    -- 覆盖默认 system（可空）
  creator_notes  VARCHAR(512) NULL,
  tags           JSON         NULL,
  card_fid       VARCHAR(32)  NOT NULL DEFAULT '',     -- 原始 PNG 卡（供导出/分享）
  greeting_seq   BIGINT UNSIGNED NOT NULL DEFAULT 0, -- 开场白消息 seq（0=未发）
  visibility     TINYINT      NOT NULL DEFAULT 0,      -- 0 私有 1 广场公开
  owner_uid      BIGINT UNSIGNED NOT NULL DEFAULT 0,
  created_at     DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
  updated_at     DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='AI 角色卡（SillyTavern V2 兼容）';

CREATE TABLE IF NOT EXISTS t_ai_conversation_ext (
  conv_id             BIGINT UNSIGNED PRIMARY KEY,
  mode                TINYINT      NOT NULL DEFAULT 1, -- 1 陪伴闲聊 2 剧情演绎（M7）
  director_enabled    TINYINT      NOT NULL DEFAULT 0,
  affinity            INT          NOT NULL DEFAULT 0, -- 好感度数值（docs/04 §8）
  affinity_stage      VARCHAR(16)  NOT NULL DEFAULT '陌生',
  memory_summary      MEDIUMTEXT   NULL,               -- 滚动摘要（M7）
  last_summarized_seq BIGINT UNSIGNED NOT NULL DEFAULT 0,
  updated_at          DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='AI 会话扩展（好感度/记忆游标）';

CREATE TABLE IF NOT EXISTS t_ai_lorebook (
  id         BIGINT UNSIGNED PRIMARY KEY,
  owner_type TINYINT      NOT NULL,                   -- 1 角色私有 2 会话共享 3 全局
  owner_id   BIGINT UNSIGNED NOT NULL,                -- 角色 uid / conv_id
  name       VARCHAR(64)  NOT NULL,
  entries    JSON         NOT NULL,                   -- 词条数组（docs/04 §4.1）
  created_at DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
  KEY idx_owner (owner_type, owner_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='世界书（词条触发注入）';
