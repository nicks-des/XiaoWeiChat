# =====================================================================
# @file server/common/tests/CMakeLists.txt
# @brief 公共库单元测试：单一测试可执行 + CTest 注册。
# =====================================================================
find_package(GTest CONFIG REQUIRED)
include(GoogleTest)  # 提供 gtest_discover_tests（vcpkg CONFIG 包不含该命令）

add_executable(test_lingxi_common
    test_packet.cpp
    test_threadpool.cpp
    test_snowflake.cpp
    test_config.cpp
    test_pools.cpp
    test_proto.cpp
    test_crypto.cpp
    test_rpcframe.cpp
)

target_link_libraries(test_lingxi_common
    PRIVATE lingxi_common
            lingxi_proto
            GTest::gtest
            GTest::gtest_main
)

# dev.json 绝对路径注入（测试加载配置不再依赖工作目录）
target_compile_definitions(test_lingxi_common
    PRIVATE "LINGXI_DEV_CONFIG=\"${CMAKE_SOURCE_DIR}/config/dev.json\"")

# 测试运行目录指向 bin/$<CONFIG>，保证 libmysql.dll 等运行时依赖可达
gtest_discover_tests(test_lingxi_common
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/bin/$<CONFIG>
    DISCOVERY_TIMEOUT 60)
