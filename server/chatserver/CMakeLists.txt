# =====================================================================
# @file server/chatserver/CMakeLists.txt
# @brief ChatServer 可执行目标
# =====================================================================
add_executable(chatserver
    main.cpp
    ChatServer.cpp
    ChatSession.cpp
    MessageService.cpp
)
target_link_libraries(chatserver PRIVATE lingxi_common lingxi_proto OpenSSL::SSL OpenSSL::Crypto)
