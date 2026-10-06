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
