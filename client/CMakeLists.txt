# =====================================================================
# @file client/CMakeLists.txt
# @brief 灵犀 IM Qt5 桌面客户端
# =====================================================================
add_executable(lingxi_client
    main.cpp
    net/TcpClient.cpp
    net/HttpManager.cpp
    service/AccountService.cpp
    service/ConversationService.cpp
    data/LocalStore.cpp
    ui/LoginWindow.cpp
    ui/MainPanel.cpp
)
target_include_directories(lingxi_client PRIVATE ${CMAKE_SOURCE_DIR})  # 以 "client/..." 前缀引用
target_link_libraries(lingxi_client
    PRIVATE lingxi_common
            lingxi_proto
            unofficial::sqlite3::sqlite3
            Qt5::Core
            Qt5::Gui
            Qt5::Widgets
)
