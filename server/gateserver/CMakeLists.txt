# =====================================================================
# @file server/gateserver/CMakeLists.txt
# @brief GateServer 可执行目标
# =====================================================================
add_executable(gateserver
    main.cpp
    GateServer.cpp
)
target_link_libraries(gateserver PRIVATE lingxi_common lingxi_proto OpenSSL::SSL OpenSSL::Crypto)
