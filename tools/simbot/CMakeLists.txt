# =====================================================================
# @file tools/simbot/CMakeLists.txt
# @brief 协议机器人（T20-08）：复用客户端网络层做可靠性与压测
# =====================================================================
add_executable(m3_flow
    m3_flow.cpp
    ${CMAKE_SOURCE_DIR}/client/net/TcpClient.cpp
    ${CMAKE_SOURCE_DIR}/client/net/HttpManager.cpp
)
target_include_directories(m3_flow PRIVATE ${LINGXI_BOOST_ROOT} ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/server/common/include)
add_executable(m2_flow
    m2_flow.cpp
    ${CMAKE_SOURCE_DIR}/client/net/TcpClient.cpp
    ${CMAKE_SOURCE_DIR}/client/net/HttpManager.cpp
)
target_include_directories(m2_flow PRIVATE ${CMAKE_SOURCE_DIR})
target_include_directories(m3_flow PRIVATE ${CMAKE_BINARY_DIR}/proto/gen)
target_link_libraries(m3_flow PRIVATE lingxi_common lingxi_proto)
target_link_libraries(m2_flow PRIVATE lingxi_common lingxi_proto)

# =====================================================================
# @brief m4_flow：M4 文件服务系统验证
# =====================================================================
add_executable(m4_flow
    m4_flow.cpp
    ${CMAKE_SOURCE_DIR}/client/net/TcpClient.cpp
    ${CMAKE_SOURCE_DIR}/client/net/HttpManager.cpp
)
target_include_directories(m4_flow PRIVATE ${LINGXI_BOOST_ROOT} ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/server/common/include ${CMAKE_BINARY_DIR}/proto/gen)
target_link_libraries(m4_flow PRIVATE lingxi_common lingxi_proto)

# =====================================================================
# @brief m5_flow：M5 通话信令面系统验证
# =====================================================================
add_executable(m5_flow
    m5_flow.cpp
    ${CMAKE_SOURCE_DIR}/client/net/TcpClient.cpp
    ${CMAKE_SOURCE_DIR}/client/net/HttpManager.cpp
)
target_include_directories(m5_flow PRIVATE ${LINGXI_BOOST_ROOT} ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/server/common/include ${CMAKE_BINARY_DIR}/proto/gen)
target_link_libraries(m5_flow PRIVATE lingxi_common lingxi_proto)
