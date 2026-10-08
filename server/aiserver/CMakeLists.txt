# =====================================================================
# @file server/aiserver/CMakeLists.txt
# @brief AIServer 可执行目标（酒馆节点）
# =====================================================================
add_executable(aiserver
    main.cpp
    TavernService.cpp
    LlmGateway.cpp
    StoryService.cpp
)
target_link_libraries(aiserver PRIVATE lingxi_common lingxi_proto OpenSSL::SSL OpenSSL::Crypto)
