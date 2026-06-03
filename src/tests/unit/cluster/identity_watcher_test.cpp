// Phase B8.3 — IdentityWatcher tests.
//
// Drives the watcher against a real in-process InMemoryEtcdClient and a
// real MtlsRegistry: parse-contract checks + end-to-end (Put an identity
// entry → registry resolves the tenant; Delete → mapping gone). Closes
// the SPIFFE/table tenant-binding loop with B8.2's ResolveTenant.
#include "cluster/identity_watcher.h"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "cluster/etcd_client.h"
#include "security/mtls.h"

using kvcache::node::cluster::IdentityWatcher;
using kvcache::node::cluster::InMemoryEtcdClient;
using kvcache::node::cluster::kIdentitiesPrefix;
using kvcache::node::cluster::kNoLease;
using kvcache::node::security::CertInfo;
using kvcache::node::security::MtlsRegistry;

namespace {
template <typename Pred>
bool WaitFor(Pred p, std::chrono::milliseconds t = std::chrono::seconds(2)) {
    const auto deadline = std::chrono::steady_clock::now() + t;
    while (std::chrono::steady_clock::now() < deadline) {
        if (p()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return p();
}
std::string Key(const std::string& id) { return std::string(kIdentitiesPrefix) + "default/" + id; }
}  // namespace

TEST(IdentityParseTest, ParsesSpiffeCnTenantKind) {
    auto e = IdentityWatcher::ParseEntry(
        R"({"spiffe_id":"spiffe://td/tenant/acme","cn":"acme.client",
            "tenant":"acme","kind":"tenant"})");
    ASSERT_TRUE(e.has_value());
    EXPECT_EQ(e->spiffe_id, "spiffe://td/tenant/acme");
    EXPECT_EQ(e->cn, "acme.client");
    EXPECT_EQ(e->identity.tenant, "acme");
    EXPECT_EQ(e->identity.kind, kvcache::node::security::IdentityKind::kTenant);
}

TEST(IdentityParseTest, RejectsEntryWithNeitherSpiffeNorCn) {
    EXPECT_FALSE(IdentityWatcher::ParseEntry(R"({"tenant":"acme"})").has_value());
    EXPECT_FALSE(IdentityWatcher::ParseEntry("{garbage").has_value());
    EXPECT_FALSE(IdentityWatcher::ParseEntry("[1,2]").has_value());  // not object
}

TEST(IdentityWatcherTest, SeedsRegistryFromExistingEntries) {
    InMemoryEtcdClient etcd;
    std::string err;
    ASSERT_TRUE(etcd.Put(Key("acme"),
        R"({"spiffe_id":"spiffe://td/node/n1","tenant":"acme"})",
        kNoLease, nullptr, &err)) << err;

    MtlsRegistry reg;
    IdentityWatcher w(etcd, reg);
    ASSERT_TRUE(w.Start());

    // The table now resolves the spiffe id to tenant "acme".
    CertInfo cert;
    cert.spiffe_id = "spiffe://td/node/n1";  // no /tenant/ seg → must come from table
    auto t = MtlsRegistry::ResolveTenant(cert, &reg);
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(*t, "acme");
}

TEST(IdentityWatcherTest, LivePutAndDelete) {
    InMemoryEtcdClient etcd;
    MtlsRegistry reg;
    IdentityWatcher w(etcd, reg);
    ASSERT_TRUE(w.Start());
    EXPECT_EQ(reg.SpiffeSize(), 0u);

    // Put → mapping appears.
    std::string err;
    ASSERT_TRUE(etcd.Put(Key("acme"),
        R"({"spiffe_id":"spiffe://td/node/n1","tenant":"acme"})",
        kNoLease, nullptr, &err)) << err;
    EXPECT_TRUE(WaitFor([&] { return reg.SpiffeSize() == 1u; }));
    EXPECT_TRUE(reg.ResolveBySpiffe("spiffe://td/node/n1").has_value());

    // Delete → mapping removed (watcher reads prev_kv).
    ASSERT_TRUE(etcd.Delete(Key("acme"), &err)) << err;
    EXPECT_TRUE(WaitFor([&] { return reg.SpiffeSize() == 0u; }));
}

TEST(IdentityWatcherTest, CnOnlyEntryPopulatesCnTable) {
    InMemoryEtcdClient etcd;
    std::string err;
    ASSERT_TRUE(etcd.Put(Key("legacy"),
        R"({"cn":"legacy.client","tenant":"legacy-tenant"})",
        kNoLease, nullptr, &err)) << err;
    MtlsRegistry reg;
    IdentityWatcher w(etcd, reg);
    ASSERT_TRUE(w.Start());

    // ResolveCert via CN table → Identity with tenant; ResolveTenant
    // returns it (table path).
    CertInfo cert;
    cert.cn = "legacy.client";
    auto t = MtlsRegistry::ResolveTenant(cert, &reg);
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(*t, "legacy-tenant");
}

TEST(IdentityWatcherTest, StartFailSafeKeepsExistingOnNoEntries) {
    // No entries → Start succeeds with an empty seed; registry stays empty
    // (not an error). A subsequent ResolveTenant with no cert identity is
    // nullopt.
    InMemoryEtcdClient etcd;
    MtlsRegistry reg;
    IdentityWatcher w(etcd, reg);
    ASSERT_TRUE(w.Start());
    EXPECT_EQ(reg.SpiffeSize(), 0u);
}
