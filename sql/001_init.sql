-- =====================================================================
-- @file 001_init.sql
-- @brief M1 库表初始化：用户 + 登录流水（完整设计见 docs/03-数据库与存储设计.md）
-- 执行：mysql -h 127.0.0.1 -P 3316 -u root < sql/001_init.sql
-- =====================================================================
CREATE DATABASE IF NOT EXISTS lingxi DEFAULT CHARSET utf8mb4 COLLATE utf8mb4_unicode_ci;
USE lingxi;

CREATE TABLE IF NOT EXISTS t_user (
  id            BIGINT UNSIGNED PRIMARY KEY,          -- 雪花 ID（docs/03 §5.1）
  username      VARCHAR(32)  NOT NULL,
  password_hash CHAR(64)    NOT NULL,                 -- SHA256(salt + password)
  salt          CHAR(32)     NOT NULL,                -- 16 字节随机盐的十六进制（32 字符）
  nickname      VARCHAR(32)  NOT NULL,
  avatar_fid    VARCHAR(32)  NOT NULL DEFAULT '',
  signature     VARCHAR(128) NOT NULL DEFAULT '',
  gender        TINYINT      NOT NULL DEFAULT 0,
  user_type     TINYINT      NOT NULL DEFAULT 0,       -- 0 真人 1 AI 角色
  status        TINYINT      NOT NULL DEFAULT 0,
  created_at    DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
  updated_at    DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  UNIQUE KEY uk_username (username),
  KEY idx_type (user_type)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='用户表（真人+AI角色统一）';

CREATE TABLE IF NOT EXISTS t_login_session (
  id         BIGINT UNSIGNED PRIMARY KEY,
  uid        BIGINT UNSIGNED NOT NULL,
  token      CHAR(64)     NOT NULL,
  device_id  VARCHAR(64)  NOT NULL DEFAULT '',
  login_ip   VARCHAR(45)  NOT NULL DEFAULT '',
  created_at DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
  KEY idx_uid (uid, created_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='登录流水（审计用；token 热数据在 Redis）';
