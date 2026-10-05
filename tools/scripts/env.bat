@echo off
rem =====================================================================
rem @file env.bat
rem @brief 开发环境变量：CMake/vcpkg/本机依赖路径集中定义（供 build.bat/test.bat 引用）
rem =====================================================================
set "VS_CMAKE=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "VCPKG_ROOT=D:\cpp\vcpkg"
set "LINGXI_BOOST_ROOT=D:\cpp\boost_1_90_0"
set "LINGXI_MYSQL_ROOT=D:\cpp\mysql\mysql-8.0.28-winx64"
set "LINGXI_QT_DIR=D:\Qt\aqt\5.12.11\msvc2017_64"
rem curl 过代理需跳过证书吊销检查（本机 schannel 限制，见 devlog）
set "LINGXI_CURL_OPTS=--ssl-no-revoke -x http://127.0.0.1:7890"
