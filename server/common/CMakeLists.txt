# =====================================================================
# @file server/common/CMakeLists.txt
# @brief 公共库 lingxi_common：日志/配置/协议帧/线程池/ID 工具/连接池。
# =====================================================================
add_library(lingxi_common STATIC
    src/logging/Logger.cpp
    src/config/Config.cpp
    src/net/Packet.cpp
    src/net/BufReader.cpp
    src/db/MySqlPool.cpp
    src/db/RedisPool.cpp
    src/rpc/RpcFrame.cpp
    src/rpc/RpcServer.cpp
    src/rpc/RpcClient.cpp
    src/http/HttpServer.cpp
    src/crypto/Crypto.cpp
    src/utils/PngCard.cpp
)

target_include_directories(lingxi_common
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
           ${LINGXI_BOOST_ROOT}          # Boost 头文件直连（asio/beast 头文件为主）
)

target_link_libraries(lingxi_common
    PUBLIC spdlog::spdlog
           nlohmann_json::nlohmann_json
           OpenSSL::SSL
           OpenSSL::Crypto
           ws2_32                        # Boost.Asio 在 Windows 的网络依赖
    PRIVATE hiredis::hiredis
)

# libmysqlclient：头文件私有（仅 MySqlPool.cpp 触碰 C API），导入库私有链接
target_include_directories(lingxi_common PRIVATE ${LINGXI_MYSQL_ROOT}/include)
target_link_libraries(lingxi_common PRIVATE ${LINGXI_MYSQL_ROOT}/lib/libmysql.lib)

# libmysql.dll 复制到运行时目录，保证测试与演示开箱即用
add_custom_command(TARGET lingxi_common POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${LINGXI_MYSQL_ROOT}/lib/libmysql.dll ${CMAKE_BINARY_DIR}/bin/$<CONFIG>
    COMMENT "复制 libmysql.dll 到运行时目录")
