# =====================================================================
# @file tools/system_test/CMakeLists.txt
# @brief 系统级联调程序（需三个服务进程已启动）
# =====================================================================
add_executable(m1_flow m1_flow.cpp)
target_link_libraries(m1_flow PRIVATE lingxi_common lingxi_proto)
