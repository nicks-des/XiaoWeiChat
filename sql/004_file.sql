-- =====================================================================
-- @file 004_file.sql
-- @brief M4 文件元数据表（完整设计见 docs/03-数据库与存储设计.md §2.4）
-- 执行：mysql -h 127.0.0.1 -P 3316 -u root < sql/004_file.sql
-- =====================================================================
USE lingxi;

CREATE TABLE IF NOT EXISTS t_file (
  fid        VARCHAR(32)  PRIMARY KEY,               -- 雪花 ID 十六进制
  md5        CHAR(32)     NOT NULL,                  -- 秒传索引
  file_name  VARCHAR(255) NOT NULL,
  size       BIGINT       NOT NULL,
  mime       VARCHAR(64)  NOT NULL DEFAULT '',
  storage    TINYINT      NOT NULL DEFAULT 0,        -- 0 本地盘 1 云对象存储（预留）
  ref_count  INT          NOT NULL DEFAULT 1,        -- 引用计数（为 0 时可物理清理）
  uploader   BIGINT UNSIGNED NOT NULL DEFAULT 0,
  created_at DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
  KEY idx_md5 (md5, size)                            -- 秒传：同 md5 且同 size 才可信
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='文件元数据表';
