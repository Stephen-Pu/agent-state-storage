# Phase S-1 — kvcache micro-benchmarks.
#
# Each bench is a tiny standalone binary that links against the
# public C ABI (libkvcache.dylib). No external bench framework — we
# use <chrono> + sort. Build with `cmake --build . --target bench`.

add_executable(bench_lookup bench_lookup.cpp)
target_include_directories(bench_lookup PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(bench_lookup PRIVATE kvcache)

add_executable(bench_fetch bench_fetch.cpp)
target_include_directories(bench_fetch PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(bench_fetch PRIVATE kvcache)

# Phase S-2 — multi-tenant fairness bench.
add_executable(bench_fairness bench_fairness.cpp)
target_include_directories(bench_fairness PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(bench_fairness PRIVATE kvcache)

# Phase S-3 — priority-class preemption bench.
add_executable(bench_priority bench_priority.cpp)
target_include_directories(bench_priority PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(bench_priority PRIVATE kvcache)

# Phase D-3 — ART tombstone-GC throughput bench.
add_executable(bench_art_gc bench_art_gc.cpp)
target_include_directories(bench_art_gc PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(bench_art_gc PRIVATE kvcache)

# Phase S-7.1 — priority bench, gRPC wire edition. Conditional on
# the gRPC stack being available (matches the existing
# test_node_data_service guard). Drives the same priority-classed
# workload as bench_priority but routes every Lookup+Fetch through
# a real gRPC NodeData stub instead of the in-process C ABI, so the
# PriorityScheduler queue actually fills enough for priority
# differentiation to be measurable.
if(KVCACHE_HAVE_GRPC)
    add_executable(bench_priority_grpc bench_priority_grpc.cpp)
    target_include_directories(bench_priority_grpc PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_link_libraries(bench_priority_grpc PRIVATE
        kvstore_node_grpc
        kvcache
        OpenSSL::Crypto
    )
endif()

# Convenience aggregate target.
add_custom_target(bench DEPENDS bench_lookup bench_fetch bench_fairness bench_priority bench_art_gc)
