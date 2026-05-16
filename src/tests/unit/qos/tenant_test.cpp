#include "qos/tenant.h"

#include <gtest/gtest.h>

using namespace kvcache::node::qos;

namespace {
TenantUuid U(uint8_t b) {
    TenantUuid u{};
    for (int i = 0; i < 16; ++i) u[i] = static_cast<uint8_t>(b + i);
    return u;
}
}

TEST(TenantRegistryTest, UpsertGetRemove) {
    TenantRegistry r;
    TenantConfig c{};
    c.tenant_id = U(1);
    c.display_name = "alpha";
    c.quota = {1024, 100, 1ull << 20, false};
    r.Upsert(c);
    EXPECT_EQ(r.Size(), 1u);
    auto got = r.Get(U(1));
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->display_name, "alpha");
    EXPECT_TRUE(r.Remove(U(1)));
    EXPECT_FALSE(r.Get(U(1)).has_value());
}

TEST(TenantRegistryTest, HashLookupMatchesUuidLookup) {
    TenantRegistry r;
    TenantConfig c{};
    c.tenant_id = U(7);
    r.Upsert(c);
    auto by_uuid = r.Get(U(7));
    auto by_hash = r.GetByHash(TenantHash(U(7)));
    ASSERT_TRUE(by_uuid.has_value());
    ASSERT_TRUE(by_hash.has_value());
    EXPECT_EQ(by_uuid->tenant_id, by_hash->tenant_id);
}

TEST(TenantRegistryTest, UpsertOverwrites) {
    TenantRegistry r;
    TenantConfig c{};
    c.tenant_id = U(1);
    c.display_name = "v1";
    r.Upsert(c);
    c.display_name = "v2";
    r.Upsert(c);
    EXPECT_EQ(r.Get(U(1))->display_name, "v2");
    EXPECT_EQ(r.Size(), 1u);
}
