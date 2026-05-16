# core/proto — generates C++ stubs from .proto files.
#
# Generated headers land under build/proto/ and are exposed via the
# kvcache_proto target. Go stubs are generated separately by the Go build
# (see control-plane/Makefile, to be added).

find_package(Protobuf REQUIRED)
find_package(gRPC CONFIG QUIET)
if(NOT gRPC_FOUND)
  message(WARNING "gRPC not found via find_package; C++ proto build will be "
                  "stubbed until dependencies are vendored.")
endif()

set(PROTO_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/events.proto
    ${CMAKE_CURRENT_SOURCE_DIR}/node.proto
    ${CMAKE_CURRENT_SOURCE_DIR}/cp.proto
)

set(PROTO_OUT_DIR ${CMAKE_BINARY_DIR}/proto)
file(MAKE_DIRECTORY ${PROTO_OUT_DIR})

# Pre-compute generated source file names so add_custom_command knows the deps.
set(PROTO_SRCS "")
set(PROTO_HDRS "")
foreach(_p ${PROTO_FILES})
    get_filename_component(_name ${_p} NAME_WE)
    list(APPEND PROTO_SRCS ${PROTO_OUT_DIR}/${_name}.pb.cc)
    list(APPEND PROTO_HDRS ${PROTO_OUT_DIR}/${_name}.pb.h)
endforeach()

add_custom_command(
    OUTPUT  ${PROTO_SRCS} ${PROTO_HDRS}
    COMMAND ${Protobuf_PROTOC_EXECUTABLE}
            --proto_path=${CMAKE_CURRENT_SOURCE_DIR}
            --cpp_out=${PROTO_OUT_DIR}
            ${PROTO_FILES}
    DEPENDS ${PROTO_FILES}
    COMMENT "Generating C++ protobuf sources"
)

# gRPC stubs are added once gRPC C++ plugin is on PATH:
#   --grpc_out=${PROTO_OUT_DIR}
#   --plugin=protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>
# TODO(stephen): enable gRPC codegen once dependency is vendored.

add_library(kvcache_proto STATIC ${PROTO_SRCS})
target_include_directories(kvcache_proto PUBLIC ${PROTO_OUT_DIR})
target_link_libraries(kvcache_proto PUBLIC protobuf::libprotobuf)
