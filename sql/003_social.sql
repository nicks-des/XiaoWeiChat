-- =====================================================================
-- @file 003_social.sql
-- @brief M3 好友与群组建表（完整设计见 docs/03-数据库与存储设计.md §2.1/§2.3）
-- 执行：mysql -h 127.0.0.1 -P 3316 -u root < sql/003_social.sql
-- =====================================================================
USE lingxi;

CREATE TABLE IF NOT EXISTS t_friend (
  id         BIGINT UNSIGNED PRIMARY KEY,             -- 雪花 ID
  uid        BIGINT UNSIGNED NOT NULL,
  friend_uid BIGINT UNSIGNED NOT NULL,
  remark     VARCHAR(32)  NOT NULL DEFAULT '',        -- 好友备注
  group_name VARCHAR(32)  NOT NULL DEFAULT '我的好友',-- 分组名
  source     TINYINT      NOT NULL DEFAULT 0,         -- 0 搜索 1 名片/广场 2 群
  created_at DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
  UNIQUE KEY uk_pair (uid, friend_uid),               -- 双向各一行：A→B 与 B→A
  KEY idx_friend (friend_uid)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='好友关系表';

CREATE TABLE IF NOT EXISTS t_friend_apply (
  id         BIGINT UNSIGNED PRIMARY KEY,
  from_uid   BIGINT UNSIGNED NOT NULL,
  to_uid     BIGINT UNSIGNED NOT NULL,
  verify_msg VARCHAR(128) NOT NULL DEFAULT '',
  status     TINYINT      NOT NULL DEFAULT 0,         -- 0 待处理 1 同意 2 拒绝 3 过期
  created_at DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
  handled_at DATETIME     NULL,
  KEY idx_to_status (to_uid, status)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='好友申请表';

CREATE TABLE IF NOT EXISTS t_group (
  conv_id      BIGINT UNSIGNED PRIMARY KEY,            -- 与会话同 ID
  name         VARCHAR(64)  NOT NULL,
  avatar_fid   VARCHAR(32)  NOT NULL DEFAULT '',
  announcement VARCHAR(512) NOT NULL DEFAULT '',
  group_type   TINYINT      NOT NULL DEFAULT 0,        -- 0 普通群 1 剧情群（酒馆，M7）
  max_member   INT          NOT NULL DEFAULT 200,
  created_at   DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='群资料表';
