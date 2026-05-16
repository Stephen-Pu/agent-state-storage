# kvagent — sidecar daemon.

set(KVAGENT_SRCS
    src/main.cpp
    src/shmem_ring/sq_cq.cpp
    src/shmem_ring/doorbell.cpp
    src/routing_cache/routing_cache.cpp
    src/bloom_view/bloom_view.cpp
)

add_executable(kvagent ${KVAGENT_SRCS})

target_include_directories(kvagent
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(kvagent
    PRIVATE
        kvcache_common
        # TODO: grpc++, nixl, spdlog, abseil.
)
