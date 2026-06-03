# TRT-LLM adapter — static lib + gtest.
#
# TRT-LLM's plugin surface is native C++, so unlike the Python adapters
# (vllm / sglang / aibrix) the integration point is a C++ library.
# Production builds embed this static archive directly into a TRT-LLM
# plugin .so; the test binary below links the same archive against the
# kvcache shared lib (the public C ABI) to verify end-to-end behavior.

add_library(kvcache_trtllm STATIC
    src/backend.cpp
)

target_include_directories(kvcache_trtllm
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        # Phase W-2 — canonical kvcache::Fnv1a64 lives in core/common/hashing.h.
        # Header-only + inline, so this adds NO link dependency (the adapter
        # still links only the public C ABI, per the note below).
        ${CMAKE_SOURCE_DIR}/core/common
)

# Compile in PIC so a downstream TRT-LLM plugin .so can absorb us.
set_target_properties(kvcache_trtllm PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)

# Link the public C ABI shared lib only. We deliberately avoid pulling
# in kvstore_node_core here — production users get their `libkvcache.so`
# from the wheel / package and we don't want this adapter to need the
# whole node build to link cleanly.
target_link_libraries(kvcache_trtllm
    PUBLIC
        kvcache
)

if(KVCACHE_BUILD_TESTS)
    add_executable(test_trtllm_backend tests/backend_test.cpp)
    target_link_libraries(test_trtllm_backend PRIVATE
        kvcache_trtllm
        kvcache
        GTest::gtest_main
    )
    # GoogleTest is fetched at top-level; include the helper to register
    # the suite with ctest.
    include(GoogleTest)
    gtest_discover_tests(test_trtllm_backend)
endif()
