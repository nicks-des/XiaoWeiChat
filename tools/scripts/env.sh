#!/usr/bin/env bash
# =====================================================================
# @file env.sh
# @brief Git Bash 环境变量（供自动化脚本与命令行使用，与 env.bat 等价）
# =====================================================================
export VS_CMAKE="/c/Program Files/Microsoft Visual Studio/18/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
export VCPKG_ROOT="D:/cpp/vcpkg"
export LINGXI_BOOST_ROOT="D:/cpp/boost_1_90_0"
export LINGXI_MYSQL_ROOT="D:/cpp/mysql/mysql-8.0.28-winx64"
export LINGXI_QT_DIR="D:/Qt/aqt/5.12.11/msvc2017_64"
# 本机 schannel 限制：curl 过代理必须跳过吊销检查（见 devlog 2026-10-05）
export LINGXI_CURL_OPTS="--ssl-no-revoke -x http://127.0.0.1:7890"
