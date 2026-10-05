#!/usr/bin/env bash
# =====================================================================
# @file dev_services.sh
# @brief 启动项目专属开发服务：MySQL(3316, 专属数据目录) + Redis(6379, 专属数据目录)
# 说明：与宿主机既有 MySQL 服务(3308)、C:\Redis 均完全隔离，互不影响
# =====================================================================
set -e
REPO="D:/study/project/NewChat"
MYSQLD="D:/cpp/mysql/mysql-8.0.28-winx64/bin/mysqld.exe"
MYSQL_CLI="D:/cpp/mysql/mysql-8.0.28-winx64/bin/mysql.exe"
REDIS="C:/Redis/redis-server.exe"

mkdir -p "$REPO/build/runtime/mysql-data" "$REPO/build/runtime/redis" "$REPO/build/runtime/logs"

# ---------- MySQL：首次使用先初始化（root 空密码，仅监听 127.0.0.1） ----------
if [ ! -f "$REPO/build/runtime/mysql-data/mysql/system_user.ibd" ] && [ ! -f "$REPO/build/runtime/mysql-data/ibdata1" ]; then
    echo "[dev] 初始化 MySQL 数据目录（--initialize-insecure）..."
    "$MYSQLD" --no-defaults --initialize-insecure \
        --datadir="D:\\study\\project\\NewChat\\build\\runtime\\mysql-data" \
        --log-error="D:\\study\\project\\NewChat\\build\\runtime\\logs\\mysql-init.err"
    echo "[dev] MySQL 初始化完成"
fi

# ---------- Redis（禁用持久化，纯开发缓存） ----------
if ! (echo >/dev/tcp/127.0.0.1/6379) 2>/dev/null; then
    echo "[dev] 启动 Redis(6379)..."
    "$REDIS" --port 6379 --bind 127.0.0.1 --dir "D:\\study\\project\\NewChat\\build\\runtime\\redis" \
        --save "" --appendonly no --daemonize no &
    sleep 1
else
    echo "[dev] Redis(6379) 已在运行"
fi

# ---------- MySQL 启动 ----------
if ! (echo >/dev/tcp/127.0.0.1/3316) 2>/dev/null; then
    echo "[dev] 启动 MySQL(3316)..."
    "$MYSQLD" --no-defaults --port=3316 --bind-address=127.0.0.1 --mysqlx=0 \
        --datadir="D:\\study\\project\\NewChat\\build\\runtime\\mysql-data" \
        --log-error="D:\\study\\project\\NewChat\\build\\runtime\\logs\\mysql.err" \
        --console &
    sleep 3
else
    echo "[dev] MySQL(3316) 已在运行"
fi

# ---------- 建库 ----------
"$MYSQL_CLI" -h 127.0.0.1 -P 3316 -u root -e "CREATE DATABASE IF NOT EXISTS lingxi DEFAULT CHARSET utf8mb4 COLLATE utf8mb4_unicode_ci;"
echo "[dev] MySQL 连通性: $("${MYSQL_CLI}" -h 127.0.0.1 -P 3316 -u root -N -e 'SELECT VERSION();')"

# 前台等待，保持子进程存活（由调用方管理生命周期）
sleep 2147483
