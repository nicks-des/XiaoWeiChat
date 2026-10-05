# =====================================================================
# @file server/common/tests/CMakeLists.txt
# @brief 公共库单元测试：单一测试可执行 + CTest 注册。
# =====================================================================
find_package(GTest CONFIG REQUIRED)

add_executable(test_lingxi_common
    test_packet.cpp
    test_threadpool.cpp
    test_snowflake.cpp
    test_config.cpp
    test_pools.cpp
    test_proto.cpp
)

target_link_libraries(test_lingxi_common
    PRIVATE lingxi_common
            lingxi_proto
            GTest::gtest
            GTest::gtest_main
)

# 测试运行目录指向 build/bin，保证 libmysql.dll 等运行时依赖可达
gtest_discover_tests(test_lingxi_common
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    DISCOVERY_TIMEOUT 60)
