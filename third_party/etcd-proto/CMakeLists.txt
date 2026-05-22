# Vendored etcd v3 protos → kvcache_etcd_proto.
#
# Runs `protoc` twice per .proto file (once for the message C++, once
# for the gRPC service C++) and bundles the generated .cc files into a
# single static lib that GrpcEtcdClient links against.
#
# Brought in only by src/CMakeLists.txt's gRPC-found branch — the
# main tree builds fine without us.

set(_proto_in_dir ${CMAKE_CURRENT_SOURCE_DIR})
set(_proto_out_dir ${CMAKE_CURRENT_BINARY_DIR}/gen)
file(MAKE_DIRECTORY ${_proto_out_dir})

set(_proto_files
    mvccpb/kv.proto
    etcdserverpb/rpc.proto
)

set(_grpc_proto_files
    etcdserverpb/rpc.proto       # only rpc.proto has service{} blocks
)

# Resolve protoc + the grpc plugin. We prefer the imported targets that
# gRPC's CONFIG package exports (cleanest under multi-config + cross-
# compilation); fall back to the legacy Module-mode variable when the
# package only set that.
if(TARGET protobuf::protoc)
    set(_protoc "$<TARGET_FILE:protobuf::protoc>")
elseif(Protobuf_PROTOC_EXECUTABLE)
    set(_protoc "${Protobuf_PROTOC_EXECUTABLE}")
else()
    message(FATAL_ERROR "protoc not found via Protobuf package")
endif()
set(_grpc_plugin_exe "$<TARGET_FILE:gRPC::grpc_cpp_plugin>")

set(_gen_srcs)
foreach(_pf ${_proto_files})
    get_filename_component(_dir  ${_pf} DIRECTORY)
    get_filename_component(_stem ${_pf} NAME_WE)
    set(_out_dir ${_proto_out_dir}/${_dir})
    set(_pb_cc  ${_out_dir}/${_stem}.pb.cc)
    set(_pb_h   ${_out_dir}/${_stem}.pb.h)
    file(MAKE_DIRECTORY ${_out_dir})
    add_custom_command(
        OUTPUT  ${_pb_cc} ${_pb_h}
        COMMAND ${_protoc}
                ARGS --proto_path=${_proto_in_dir}
                     --cpp_out=${_proto_out_dir}
                     ${_proto_in_dir}/${_pf}
        DEPENDS ${_proto_in_dir}/${_pf}
        COMMENT "protoc --cpp_out  ${_pf}"
    )
    list(APPEND _gen_srcs ${_pb_cc})
endforeach()

foreach(_pf ${_grpc_proto_files})
    get_filename_component(_dir  ${_pf} DIRECTORY)
    get_filename_component(_stem ${_pf} NAME_WE)
    set(_out_dir ${_proto_out_dir}/${_dir})
    set(_grpc_cc ${_out_dir}/${_stem}.grpc.pb.cc)
    set(_grpc_h  ${_out_dir}/${_stem}.grpc.pb.h)
    add_custom_command(
        OUTPUT  ${_grpc_cc} ${_grpc_h}
        COMMAND ${_protoc}
                ARGS --proto_path=${_proto_in_dir}
                     --grpc_out=${_proto_out_dir}
                     --plugin=protoc-gen-grpc=${_grpc_plugin_exe}
                     ${_proto_in_dir}/${_pf}
        DEPENDS ${_proto_in_dir}/${_pf}
        COMMENT "protoc --grpc_out ${_pf}"
    )
    list(APPEND _gen_srcs ${_grpc_cc})
endforeach()

add_library(kvcache_etcd_proto STATIC ${_gen_srcs})
target_include_directories(kvcache_etcd_proto PUBLIC ${_proto_out_dir})
target_link_libraries(kvcache_etcd_proto
    PUBLIC
        protobuf::libprotobuf
        gRPC::grpc++
)
# Generated code triggers some pedantic warnings on AppleClang; the
# vendored .proto files themselves are clean and any churn from
# upstream-style codegen isn't something we should police.
target_compile_options(kvcache_etcd_proto PRIVATE
    -Wno-unused-parameter
    -Wno-missing-field-initializers
)
