# =====================================================================
# @file CMakeLists.txt
# @brief 灵犀 IM 根构建脚本：统一 C++17 标准、第三方依赖接入与子目录组织。
#
# 依赖接入方式（详见 docs/09-技术决策记录ADR.md ADR-017）：
# - vcpkg 清单模式（vcpkg.json）：spdlog / nlohmann-json / protobuf / hiredis / gtest；
# - 本机路径直连：Boost（头文件为主）、libmysqlclient、Qt5（aqt 安装的 MSVC 套件）。
# =====================================================================
cmake_minimum_required(VERSION 3.24)

project(LingXi VERSION 0.1.0 LANGUAGES C CXX)

# ---- 语言与全局编译选项 ----
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)                       # Qt 元对象编译（Q_OBJECT 类）

# Windows 常规宏：避免 min/max 宏污染与 WinSock 版本告警
add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)

# MSVC：源码统一按 UTF-8 读取/输出（避免中文注释 C4819 与字符串字面量乱码）
if(MSVC)
    add_compile_options(/utf-8)
endif()

# ---- 统一输出目录：显式带 $<CONFIG>，避免 VS 多配置生成器自动追加配置子目录带来的路径分歧 ----
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/$<CONFIG>)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib/$<CONFIG>)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib/$<CONFIG>)

# ---- 可配置的本机依赖路径（均有默认值，build.bat 会显式传入） ----
set(LINGXI_BOOST_ROOT "D:/cpp/boost_1_90_0" CACHE PATH "Boost 根目录（头文件 + stage 预编译库）")
set(LINGXI_MYSQL_ROOT "D:/cpp/mysql/mysql-8.0.28-winx64" CACHE PATH "MySQL Server 目录（提供 libmysqlclient C API）")
set(LINGXI_QT_DIR "D:/Qt/aqt/5.12.11/msvc2017_64" CACHE PATH "Qt 5.12.11 MSVC 套件目录")

option(LINGXI_BUILD_TESTS "是否构建单元测试" ON)

# ---- 第三方依赖（vcpkg 清单模式，经工具链文件解析） ----
find_package(spdlog CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(Protobuf CONFIG REQUIRED)
find_package(hiredis CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)  # SHA256/HMAC（docs/02 §6）
find_package(unofficial-sqlite3 CONFIG REQUIRED)  # 客户端本地缓存（ADR-009）

# Qt 为可选依赖：未找到时跳过所有 Qt 目标（M0 仅影响 qt_hello 演示）
find_package(Qt5 QUIET COMPONENTS Core Gui Widgets PATHS ${LINGXI_QT_DIR} NO_DEFAULT_PATH)
if(Qt5_FOUND)
    message(STATUS "Qt5 found: ${Qt5_DIR}")
else()
    message(STATUS "Qt5 NOT found, qt_hello demo will be skipped")
endif()

include(CTest)

# ---- 子目录 ----
add_subdirectory(server/common)   # 公共库（日志/配置/协议帧/线程池/ID/连接池/RPC/HTTP/加密）
add_subdirectory(proto)           # Protobuf 协议库
add_subdirectory(server/gateserver)
add_subdirectory(server/statusserver)
add_subdirectory(server/chatserver)
add_subdirectory(client)          # Qt5 客户端（Qt5 未找到时自动跳过）
add_subdirectory(tools/demos)     # 工具演示（qt_hello 编译链验证）
add_subdirectory(tools/simbot)    # 协议机器人（压测/可靠性验证）
add_subdirectory(tools/system_test)  # 系统级联调程序（m1_flow）

if(LINGXI_BUILD_TESTS)
    add_subdirectory(server/common/tests)
endif()
