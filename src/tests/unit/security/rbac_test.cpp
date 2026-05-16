#include "security/rbac.h"

#include <gtest/gtest.h>
#include <thread>

#include "qos/tenant.h"

using namespace kvcache::node::security;

namespace {
Identity TenantPeer(uint8_t b) {
    Identity i{};
    i.kind = IdentityKind::kTenant;
    i.cn = "tenant-" + std::to_string(b);
    for (int j = 0; j < 16; ++j) i.tenant_id[j] = static_cast<uint8_t>(b + j);
    return i;
}
}

TEST(RbacTest, UnknownIdentityDenied) {
    Rbac rbac;
    Identity i{};  // kUnknown
    EXPECT_EQ(rbac.Check(i, Action::kLookup, 1, false), Decision::kDeny);
}

TEST(RbacTest, TenantCanActOnOwnDataOnly) {
    Rbac rbac;
    auto t = TenantPeer(1);
    const auto own_hash = kvcache::node::qos::TenantHash(t.tenant_id);
    EXPECT_EQ(rbac.Check(t, Action::kLookup, own_hash, false), Decision::kAllow);
    EXPECT_EQ(rbac.Check(t, Action::kLookup, own_hash + 1, false), Decision::kDeny);
}

TEST(RbacTest, AdminRoleRestrictions) {
    Rbac rbac;
    Identity admin{}; admin.kind = IdentityKind::kAdmin; admin.cn = "admin-1";
    Identity tenant = TenantPeer(2);
    EXPECT_EQ(rbac.Check(admin, Action::kAdminWrite, 0, false), Decision::kAllow);
    EXPECT_EQ(rbac.Check(tenant, Action::kAdminWrite, 0, false), Decision::kDeny);
}

TEST(RbacTest, InternalPrincipalAllowedForDataOps) {
    Rbac rbac;
    Identity sv{}; sv.kind = IdentityKind::kInternal; sv.cn = "kvagent-1";
    EXPECT_EQ(rbac.Check(sv, Action::kFetch, 0, false), Decision::kAllow);
    EXPECT_EQ(rbac.Check(sv, Action::kAdminWrite, 0, false), Decision::kDeny);
}

TEST(RbacTest, DeletionPendingBlocksWrites) {
    Rbac rbac;
    auto t = TenantPeer(3);
    const auto h = kvcache::node::qos::TenantHash(t.tenant_id);
    EXPECT_EQ(rbac.Check(t, Action::kSeal,    h, true), Decision::kDeny);
    EXPECT_EQ(rbac.Check(t, Action::kPublish, h, true), Decision::kDeny);
    EXPECT_EQ(rbac.Check(t, Action::kLookup,  h, true), Decision::kAllow);
}

TEST(RbacTest, DecisionCacheHits) {
    Rbac::Options o; o.cache_ttl = std::chrono::seconds(10);
    Rbac rbac(o);
    auto t = TenantPeer(9);
    const auto h = kvcache::node::qos::TenantHash(t.tenant_id);
    for (int i = 0; i < 100; ++i) rbac.Check(t, Action::kLookup, h, false);
    EXPECT_GE(rbac.CacheHits(), 99u);
}
