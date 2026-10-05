# =====================================================================
# @file server/demos/CMakeLists.txt
# @brief 服务端演示程序（编译链验证）。
# =====================================================================
add_executable(boost_echo boost_echo.cpp)
target_link_libraries(boost_echo PRIVATE lingxi_common)
