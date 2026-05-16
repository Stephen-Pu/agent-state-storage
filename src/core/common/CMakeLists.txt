# core/common — shared C++ utilities used by node, agent, and ABI layers.

add_library(kvcache_common STATIC
    locator.cpp
    blake3.cpp
    logging.cpp
    metrics.cpp
    hashing.cpp
)

target_include_directories(kvcache_common
    PUBLIC
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}
)

# TODO(stephen): link spdlog, abseil, prometheus-cpp, and a vendored BLAKE3
# implementation once dependencies are wired in.
