-- =====================================================================
-- @file 005_call.sql
-- @brief M5 通话记录表（完整设计见 docs/03-数据库与存储设计.md §2.5）
-- 执行：mysql -h 127.0.0.1 -P 3316 -u root < sql/005_call.sql
-- =====================================================================
USE lingxi;

CREATE TABLE IF NOT EXISTS t_call_record (
  id           BIGINT UNSIGNED PRIMARY KEY,           -- 雪花 ID
  call_id      CHAR(36)     NOT NULL,                 -- 通话唯一 ID（客户端 UUID）
  conv_id      BIGINT UNSIGNED NOT NULL DEFAULT 0,   -- 关联会话（单聊 member_key 复用）
  caller_uid   BIGINT UNSIGNED NOT NULL,
  callee_uid   BIGINT UNSIGNED NOT NULL,
  media_type   TINYINT      NOT NULL,                 -- 1 音频 2 视频
  state        TINYINT      NOT NULL,                 -- 0 未接 1 已接 2 拒绝 3 取消 4 异常
  duration_sec INT          NOT NULL DEFAULT 0,
  created_at   DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
  KEY idx_caller (caller_uid, created_at),
  KEY idx_callee (callee_uid, created_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='通话记录表';
