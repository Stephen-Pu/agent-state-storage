// Phase KVZ-2 — kv_kvtensor_encode / kv_kvtensor_decode C ABI tests.
//
// Drives the KV-tensor codec through the public C ABI (the surface engine
// adapters actually call): two-pass sizing, lossy round-trip within an error
// bound, shape peek, and argument guards.
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <vector>

#include "kvcache/kv_abi.h"
#include "kvcache/kv_errors.h"

namespace {

std::vector<float> SmoothKv(uint32_t T, uint32_t E) {
    std::vector<float> v(static_cast<std::size_t>(T) * E);
    for (uint32_t e = 0; e < E; ++e)
        for (uint32_t t = 0; t < T; ++t)
            v[static_cast<std::size_t>(t) * E + e] =
                1.0f + 0.01f * static_cast<float>(t) + 0.1f * static_cast<float>(e % 5);
    return v;
}

double MaxAbsErr(const std::vector<float>& a, const std::vector<float>& b) {
    double m = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i)
        m = std::max(m, std::fabs(static_cast<double>(a[i]) - b[i]));
    return m;
}

}  // namespace

TEST(KvTensorAbi, EncodeSizingThenDecodeRoundTrip) {
    const uint32_t T = 64, E = 128;
    auto orig = SmoothKv(T, E);

    // Pass 1: size the blob (out=NULL).
    size_t need = 0;
    ASSERT_EQ(kv_kvtensor_encode(orig.data(), T, E, 8, 1, nullptr, 0, &need), KV_OK);
    ASSERT_GT(need, 0u);
    EXPECT_LT(need, orig.size() * sizeof(float));  // smaller than raw fp32

    // Too-small buffer → NOMEM, blob not written.
    std::vector<uint8_t> tiny(need - 1);
    EXPECT_EQ(kv_kvtensor_encode(orig.data(), T, E, 8, 1, tiny.data(),
                                 tiny.size(), &need), KV_E_NOMEM);

    // Pass 2: real encode.
    std::vector<uint8_t> blob(need);
    size_t wrote = 0;
    ASSERT_EQ(kv_kvtensor_encode(orig.data(), T, E, 8, 1, blob.data(),
                                 blob.size(), &wrote), KV_OK);
    EXPECT_EQ(wrote, need);

    // Shape peek (out=NULL).
    uint32_t pt = 0, pe = 0;
    ASSERT_EQ(kv_kvtensor_decode(blob.data(), blob.size(), nullptr, 0, &pt, &pe), KV_OK);
    EXPECT_EQ(pt, T);
    EXPECT_EQ(pe, E);

    // Too-small output → NOMEM, shape still reported.
    std::vector<float> small(T * E - 1);
    uint32_t dt = 0, de = 0;
    EXPECT_EQ(kv_kvtensor_decode(blob.data(), blob.size(), small.data(),
                                 small.size(), &dt, &de), KV_E_NOMEM);
    EXPECT_EQ(dt, T);

    // Full decode → lossy round-trip within <1% of the value range (~0.63).
    std::vector<float> dec(T * E);
    ASSERT_EQ(kv_kvtensor_decode(blob.data(), blob.size(), dec.data(),
                                 dec.size(), &dt, &de), KV_OK);
    EXPECT_LT(MaxAbsErr(orig, dec), 0.01);
}

TEST(KvTensorAbi, RejectsBadArgs) {
    std::vector<float> d(16, 1.0f);
    size_t n = 0;
    EXPECT_EQ(kv_kvtensor_encode(d.data(), 4, 4, 8, 1, nullptr, 0, nullptr), KV_E_INVAL);
    EXPECT_EQ(kv_kvtensor_encode(d.data(), 4, 4, 3, 1, nullptr, 0, &n), KV_E_INVAL);  // bad bits
    EXPECT_EQ(kv_kvtensor_encode(d.data(), 0, 4, 8, 1, nullptr, 0, &n), KV_E_INVAL);  // 0 tokens
    uint32_t t = 0, e = 0;
    uint8_t junk[4] = {'X', 'X', 'X', 'X'};
    EXPECT_EQ(kv_kvtensor_decode(junk, 4, nullptr, 0, &t, &e), KV_E_INVAL);  // bad magic
    EXPECT_EQ(kv_kvtensor_decode(nullptr, 0, nullptr, 0, &t, &e), KV_E_INVAL);
}
