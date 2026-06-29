// KVZ-1 — KvTensorCodec rate-distortion + round-trip tests.
//
// KV-cache values are smooth across token positions, so the test data is a
// synthetic tensor whose per-element value drifts slowly token-to-token
// (plus a per-element offset) — representative of real K/V correlation,
// which is exactly what the delta+quant pipeline exploits.
#include "codec/kv_tensor_codec.h"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

using kvcache::codec::DecodeKvTensor;
using kvcache::codec::EncodeKvTensor;
using kvcache::codec::KvCodecParams;
using kvcache::codec::KvShape;

namespace {

// Smoothly-varying synthetic KV: value[t][e] = base(e) + slope(e)*t + small
// wiggle. Adjacent tokens differ by ~slope — small, so DPCM residuals are
// tiny relative to absolute magnitude.
std::vector<float> SmoothKv(uint32_t T, uint32_t E) {
    std::vector<float> v(static_cast<std::size_t>(T) * E);
    for (uint32_t e = 0; e < E; ++e) {
        const float base  = 0.5f * static_cast<float>((e % 17)) - 4.0f;
        const float slope = 0.01f * static_cast<float>((e % 7) + 1);
        for (uint32_t t = 0; t < T; ++t) {
            const float wiggle = 0.02f * std::sin(0.1f * (t + e));
            v[static_cast<std::size_t>(t) * E + e] =
                base + slope * static_cast<float>(t) + wiggle;
        }
    }
    return v;
}

double MaxAbsErr(const std::vector<float>& a, const std::vector<float>& b) {
    double m = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i)
        m = std::max(m, std::fabs(static_cast<double>(a[i]) - b[i]));
    return m;
}

double ValueRange(const std::vector<float>& a) {
    float lo = a[0], hi = a[0];
    for (float x : a) { lo = std::min(lo, x); hi = std::max(hi, x); }
    return static_cast<double>(hi) - lo;
}

}  // namespace

TEST(KvTensorCodec, RoundTripWithinErrorBoundInt8) {
    const KvShape shape{128, 256};
    auto orig = SmoothKv(shape.n_tokens, shape.elems_per_token);
    std::string err;
    std::vector<uint8_t> blob;
    ASSERT_TRUE(EncodeKvTensor(orig.data(), shape, {8, true}, &blob, &err)) << err;

    std::vector<float> dec;
    KvShape got{};
    ASSERT_TRUE(DecodeKvTensor(blob.data(), blob.size(), &dec, &got, &err)) << err;
    EXPECT_EQ(got.n_tokens, shape.n_tokens);
    EXPECT_EQ(got.elems_per_token, shape.elems_per_token);
    ASSERT_EQ(dec.size(), orig.size());
    // int8 over a smooth tensor: max abs error well under 1% of the range.
    EXPECT_LT(MaxAbsErr(orig, dec), 0.01 * ValueRange(orig));
}

TEST(KvTensorCodec, HigherBitsLowerErrorRateDistortion) {
    const KvShape shape{64, 512};
    auto orig = SmoothKv(shape.n_tokens, shape.elems_per_token);
    std::string err;

    std::vector<uint8_t> b8, b4;
    ASSERT_TRUE(EncodeKvTensor(orig.data(), shape, {8, true}, &b8, &err)) << err;
    ASSERT_TRUE(EncodeKvTensor(orig.data(), shape, {4, true}, &b4, &err)) << err;

    std::vector<float> d8, d4;
    KvShape s{};
    ASSERT_TRUE(DecodeKvTensor(b8.data(), b8.size(), &d8, &s, &err)) << err;
    ASSERT_TRUE(DecodeKvTensor(b4.data(), b4.size(), &d4, &s, &err)) << err;

    const double e8 = MaxAbsErr(orig, d8);
    const double e4 = MaxAbsErr(orig, d4);
    EXPECT_LT(e8, e4) << "8-bit must reconstruct more accurately than 4-bit";
}

TEST(KvTensorCodec, CompressesBelowRawFp32) {
    const KvShape shape{256, 256};  // 64K elems × 4B = 256 KiB raw
    auto orig = SmoothKv(shape.n_tokens, shape.elems_per_token);
    const std::size_t raw = orig.size() * sizeof(float);
    std::string err;

    std::vector<uint8_t> b8;
    ASSERT_TRUE(EncodeKvTensor(orig.data(), shape, {8, true}, &b8, &err)) << err;
    // int8 quant alone is ~4x; even with per-token scale overhead the blob is
    // comfortably under half the raw fp32 size (more with the zstd stage).
    EXPECT_LT(b8.size(), raw / 2)
        << "int8 blob " << b8.size() << " vs raw " << raw;

    std::vector<uint8_t> b4;
    ASSERT_TRUE(EncodeKvTensor(orig.data(), shape, {4, true}, &b4, &err)) << err;
    EXPECT_LT(b4.size(), b8.size()) << "int4 must be smaller than int8";
}

TEST(KvTensorCodec, DeltaOffStillRoundTrips) {
    const KvShape shape{32, 64};
    auto orig = SmoothKv(shape.n_tokens, shape.elems_per_token);
    std::string err;
    std::vector<uint8_t> blob;
    ASSERT_TRUE(EncodeKvTensor(orig.data(), shape, {8, false}, &blob, &err)) << err;
    std::vector<float> dec;
    KvShape s{};
    ASSERT_TRUE(DecodeKvTensor(blob.data(), blob.size(), &dec, &s, &err)) << err;
    EXPECT_LT(MaxAbsErr(orig, dec), 0.01 * ValueRange(orig));
}

TEST(KvTensorCodec, SingleTokenAndAllZero) {
    std::string err;
    // 1 token.
    std::vector<float> one(64, 3.5f);
    std::vector<uint8_t> blob;
    ASSERT_TRUE(EncodeKvTensor(one.data(), {1, 64}, {8, true}, &blob, &err)) << err;
    std::vector<float> dec; KvShape s{};
    ASSERT_TRUE(DecodeKvTensor(blob.data(), blob.size(), &dec, &s, &err)) << err;
    EXPECT_LT(MaxAbsErr(one, dec), 0.05);
    // All-zero tensor → scale defaults to 1, exact round-trip to zero.
    std::vector<float> zeros(128, 0.0f);
    ASSERT_TRUE(EncodeKvTensor(zeros.data(), {4, 32}, {8, true}, &blob, &err)) << err;
    ASSERT_TRUE(DecodeKvTensor(blob.data(), blob.size(), &dec, &s, &err)) << err;
    EXPECT_EQ(MaxAbsErr(zeros, dec), 0.0);
}

TEST(KvTensorCodec, RejectsBadParamsAndShape) {
    std::string err;
    std::vector<float> d(16, 1.0f);
    std::vector<uint8_t> blob;
    EXPECT_FALSE(EncodeKvTensor(d.data(), {4, 4}, {3, true}, &blob, &err));  // bad bits
    EXPECT_FALSE(EncodeKvTensor(d.data(), {0, 4}, {8, true}, &blob, &err));  // 0 tokens
    EXPECT_FALSE(EncodeKvTensor(d.data(), {4, 0}, {8, true}, &blob, &err));  // 0 elems
    EXPECT_FALSE(EncodeKvTensor(nullptr, {4, 4}, {8, true}, &blob, &err));   // null
}

TEST(KvTensorCodec, RejectsCorruptBlob) {
    std::string err;
    std::vector<float> orig = SmoothKv(8, 16);
    std::vector<uint8_t> blob;
    ASSERT_TRUE(EncodeKvTensor(orig.data(), {8, 16}, {8, true}, &blob, &err)) << err;
    std::vector<float> dec; KvShape s{};
    // Short blob.
    EXPECT_FALSE(DecodeKvTensor(blob.data(), 4, &dec, &s, &err));
    // Bad magic.
    auto bad = blob; bad[0] = 'X';
    EXPECT_FALSE(DecodeKvTensor(bad.data(), bad.size(), &dec, &s, &err));
}
