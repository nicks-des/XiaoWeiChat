# =====================================================================
# @file server/fileserver/CMakeLists.txt
# @brief FileServer 可执行目标
# =====================================================================
add_executable(fileserver
    main.cpp
    FileService.cpp
)
target_link_libraries(fileserver PRIVATE lingxi_common lingxi_proto OpenSSL::SSL OpenSSL::Crypto)
