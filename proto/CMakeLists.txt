# =====================================================================
# @file proto/CMakeLists.txt
# @brief Protobuf 协议库：调用 protoc 生成 C++ 代码并编译为 lingxi_proto 静态库。
# =====================================================================
set(PROTO_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/gen)
file(MAKE_DIRECTORY ${GEN_DIR})

# protoc 定位：优先用 vcpkg 配置提供的可执行目标，否则在 vcpkg tools 目录查找
if(TARGET protobuf::protoc)
    set(LINGXI_PROTOC_CMD "$<TARGET_FILE:protobuf::protoc>")
else()
    find_program(LINGXI_PROTOC_EXE protoc
        PATHS "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/tools/protobuf"
        REQUIRED)
    set(LINGXI_PROTOC_CMD "${LINGXI_PROTOC_EXE}")
endif()

set(PROTO_FILES lingxi.proto)
set(PROTO_GENERATED_SRCS)
set(PROTO_GENERATED_HDRS)

foreach(PROTO_FILE ${PROTO_FILES})
    get_filename_component(PROTO_NAME ${PROTO_FILE} NAME_WE)
    set(GENERATED_SRC ${GEN_DIR}/${PROTO_NAME}.pb.cc)
    set(GENERATED_HDR ${GEN_DIR}/${PROTO_NAME}.pb.h)

    add_custom_command(
        OUTPUT ${GENERATED_SRC} ${GENERATED_HDR}
        COMMAND ${LINGXI_PROTOC_CMD}
        ARGS --cpp_out=${GEN_DIR} -I${PROTO_DIR} ${PROTO_DIR}/${PROTO_FILE}
        DEPENDS ${PROTO_DIR}/${PROTO_FILE}
        COMMENT "protoc 生成 ${PROTO_NAME}.pb.{h,cc}"
        VERBATIM)

    list(APPEND PROTO_GENERATED_SRCS ${GENERATED_SRC})
    list(APPEND PROTO_GENERATED_HDRS ${GENERATED_HDR})
endforeach()

add_library(lingxi_proto STATIC ${PROTO_GENERATED_SRCS} ${PROTO_GENERATED_HDRS})
target_include_directories(lingxi_proto PUBLIC ${GEN_DIR})
target_link_libraries(lingxi_proto PUBLIC protobuf::libprotobuf)
