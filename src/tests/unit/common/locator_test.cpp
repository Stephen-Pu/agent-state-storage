// Phase W-1 — Locator wire (de)serialization: round-trip + canonical
// little-endian byte layout (host-endian-independent, LLD §2.1).
#include "locator.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>

#include "kvcache/kv_types.h"

using kvcache::DeserializeLocator;
using kvcache::SerializeLocator;

namespace {

kv_locator_t SampleLocator() {
    kv_locator_t loc{};
    for (int i = 0; i < 16; ++i) loc.tenant_id[i] = static_cast<uint8_t>(0x10 + i);
    loc.model_id_hash = 0x0102030405060708ull;
    for (int i = 0; i < 16; ++i) loc.prefix_hash[i] = static_cast<uint8_t>(0xA0 + i);
    loc.range.layer_start = 0x1122;
    loc.range.layer_count = 0x3344;
    loc.range.head_start  = 0x5566;
    loc.range.head_count  = 0x7788;
    loc.range.token_start = 0x99AABBCCu;
    loc.range.token_count = 0xDDEEFF00u;
    loc.version = 1;
    loc.flags   = 0;
    return loc;
}

}  // namespace

TEST(LocatorWire, RoundTripPreservesEveryField) {
    const kv_locator_t in = SampleLocator();
    std::array<uint8_t, 64> buf{};
    ASSERT_TRUE(SerializeLocator(in, std::span<uint8_t, 64>(buf)));

    kv_locator_t out{};
    ASSERT_TRUE(DeserializeLocator(std::span<const uint8_t, 64>(buf), &out));

    EXPECT_EQ(0, std::memcmp(in.tenant_id, out.tenant_id, 16));
    EXPECT_EQ(in.model_id_hash, out.model_id_hash);
    EXPECT_EQ(0, std::memcmp(in.prefix_hash, out.prefix_hash, 16));
    EXPECT_EQ(in.range.layer_start, out.range.layer_start);
    EXPECT_EQ(in.range.layer_count, out.range.layer_count);
    EXPECT_EQ(in.range.head_start,  out.range.head_start);
    EXPECT_EQ(in.range.head_count,  out.range.head_count);
    EXPECT_EQ(in.range.token_start, out.range.token_start);
    EXPECT_EQ(in.range.token_count, out.range.token_count);
    EXPECT_EQ(in.version, out.version);
    EXPECT_EQ(in.flags,   out.flags);
}

// The headline cross-arch guarantee: the wire bytes are canonical
// little-endian regardless of the host's byte order. We assert the exact
// byte at each scalar field's offset.
TEST(LocatorWire, CanonicalLittleEndianByteLayout) {
    const kv_locator_t in = SampleLocator();
    std::array<uint8_t, 64> b{};
    ASSERT_TRUE(SerializeLocator(in, std::span<uint8_t, 64>(b)));

    // tenant_id verbatim at [0,16).
    for (int i = 0; i < 16; ++i) EXPECT_EQ(b[i], 0x10 + i);
    // model_id_hash 0x0102030405060708 → LE bytes 08 07 06 05 04 03 02 01.
    EXPECT_EQ(b[16], 0x08); EXPECT_EQ(b[17], 0x07);
    EXPECT_EQ(b[18], 0x06); EXPECT_EQ(b[19], 0x05);
    EXPECT_EQ(b[20], 0x04); EXPECT_EQ(b[21], 0x03);
    EXPECT_EQ(b[22], 0x02); EXPECT_EQ(b[23], 0x01);
    // prefix_hash verbatim at [24,40).
    for (int i = 0; i < 16; ++i) EXPECT_EQ(b[24 + i], 0xA0 + i);
    // range.layer_start 0x1122 → LE 22 11 at [40,42).
    EXPECT_EQ(b[40], 0x22); EXPECT_EQ(b[41], 0x11);
    // range.token_start 0x99AABBCC → LE CC BB AA 99 at [48,52).
    EXPECT_EQ(b[48], 0xCC); EXPECT_EQ(b[49], 0xBB);
    EXPECT_EQ(b[50], 0xAA); EXPECT_EQ(b[51], 0x99);
    // version 1 → LE 01 00 00 00 at [56,60).
    EXPECT_EQ(b[56], 0x01); EXPECT_EQ(b[57], 0x00);
    EXPECT_EQ(b[58], 0x00); EXPECT_EQ(b[59], 0x00);
}

TEST(LocatorWire, DeserializeRejectsUnknownVersion) {
    kv_locator_t in = SampleLocator();
    in.version = 7;  // unsupported
    std::array<uint8_t, 64> buf{};
    ASSERT_TRUE(SerializeLocator(in, std::span<uint8_t, 64>(buf)));
    kv_locator_t out{};
    EXPECT_FALSE(DeserializeLocator(std::span<const uint8_t, 64>(buf), &out));
}
