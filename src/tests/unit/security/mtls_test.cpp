#include "security/mtls.h"

#include <gtest/gtest.h>

using namespace kvcache::node::security;

TEST(MtlsRegistryTest, UpsertResolveRemove) {
    MtlsRegistry r;
    Identity id;
    id.kind = IdentityKind::kTenant;
    id.cn = "tenant-A";
    id.display_name = "Tenant Alpha";
    EXPECT_TRUE(r.UpsertMapping("tenant-A", id));
    auto got = r.Resolve("tenant-A");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->kind, IdentityKind::kTenant);
    EXPECT_EQ(got->display_name, "Tenant Alpha");
    EXPECT_TRUE(r.RemoveMapping("tenant-A"));
    EXPECT_FALSE(r.Resolve("tenant-A").has_value());
}

TEST(MtlsRegistryTest, ExtractCnFromPem) {
    auto cn = MtlsRegistry::ExtractCnFromPem(
        "Subject: O=Acme, CN=kvagent-node-1, OU=infra\n");
    ASSERT_TRUE(cn.has_value());
    EXPECT_EQ(*cn, "kvagent-node-1");
}

TEST(MtlsRegistryTest, ExtractCnReturnsNulloptOnMissing) {
    auto cn = MtlsRegistry::ExtractCnFromPem("Subject: O=Acme\n");
    EXPECT_FALSE(cn.has_value());
}
