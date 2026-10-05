# =====================================================================
# @file server/statusserver/CMakeLists.txt
# @brief StatusServer 可执行目标
# =====================================================================
add_executable(statusserver
    main.cpp
    StatusService.cpp
)
target_link_libraries(statusserver PRIVATE lingxi_common lingxi_proto OpenSSL::SSL OpenSSL::Crypto)
