// Phase W-2 — canonical FNV-1a-64 golden vectors.
//
// These exact values are ALSO asserted by the Python side
// (src/adapters/core/tests/test_connector_hash.py). The two test files are
// the cross-language lock on model_id_hash: if either implementation drifts,
// its golden test fails. Do not change a value here without changing it there.
#include "hashing.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string_view>

using kvcache::Fnv1a64;

TEST(Fnv1a64, GoldenVectors) {
    EXPECT_EQ(Fnv1a64(std::string_view("")),  0xcbf29ce484222325ULL);  // offset basis
    EXPECT_EQ(Fnv1a64(std::string_view("a")), 0xaf63dc4c8601ec8cULL);  // canonical FNV ref
    EXPECT_EQ(Fnv1a64(std::string_view("llama-3-8b")),         0x2154ed5dc3aa6e7bULL);
    EXPECT_EQ(Fnv1a64(std::string_view("mistral-7b-instruct")), 0x4cdeeea8ec431068ULL);
}

TEST(Fnv1a64, BytesOverloadMatchesStringView) {
    const char* s = "llama-3-8b";
    EXPECT_EQ(Fnv1a64(s, 10), Fnv1a64(std::string_view(s)));
}

TEST(Fnv1a64, SixteenZeroBytes) {
    // The node-service tenant path FNV-1a's a 16-byte SHA-1 digest prefix;
    // pin the all-zero-digest case.
    const uint8_t zeros[16] = {0};
    EXPECT_EQ(Fnv1a64(zeros, 16), 0x88201fb960ff6465ULL);
}
