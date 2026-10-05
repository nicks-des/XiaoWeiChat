# =====================================================================
# @file tools/demos/CMakeLists.txt
# @brief 工具演示程序：qt_hello 仅在找到 Qt5 时构建。
# =====================================================================
if(Qt5_FOUND)
    add_executable(qt_hello qt_hello/main.cpp)
    target_link_libraries(qt_hello PRIVATE Qt5::Core Qt5::Gui Qt5::Widgets)
else()
    message(STATUS "skip qt_hello (Qt5 not found)")
endif()
