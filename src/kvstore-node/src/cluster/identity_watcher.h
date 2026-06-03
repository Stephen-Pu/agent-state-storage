// LLD §5.2 — node-side mTLS identity table sync.
//
// Phase B8.3 — the consumer half of SPIFFE/table tenant binding. B8.2's
// MtlsRegistry::ResolveTenant resolves a peer cert to its authoritative
// tenant via a table the CP publishes; B8.3 is the watcher that keeps
// that table fresh from etcd.
//
// The CP writes one JSON entry per identity under
// /kvcache/identities/<cluster>/<id>:
//
//   { "spiffe_id": "spiffe://td/tenant/acme",   // optional
//     "cn":        "acme.client",               // optional
//     "tenant":    "acme",                       // authoritative tenant
//     "kind":      "tenant" }                    // optional
//
// IdentityWatcher seeds the MtlsRegistry from a GetPrefix at Start, then
// keeps it in sync via a WatchPrefix: a Put upserts the spiffe_id and/or
// CN mapping, a Delete removes them (from the event's prev_kv). Same
// shape as DrainWatcher (A2.2) and ClusterViewWatcher (A1.8).
#pragma once

#include <optional>
#include <string>

#include "cluster/etcd_client.h"
#include "security/mtls.h"

namespace kvcache::node::cluster {

// Default etcd prefix the CP publishes identity entries under.
inline constexpr const char* kIdentitiesPrefix = "/kvcache/identities/";

class IdentityWatcher {
   public:
    IdentityWatcher(IEtcdClient& etcd, security::MtlsRegistry& registry,
                    std::string prefix = kIdentitiesPrefix);
    ~IdentityWatcher();

    IdentityWatcher(const IdentityWatcher&)            = delete;
    IdentityWatcher& operator=(const IdentityWatcher&) = delete;

    // Seed the registry from the current prefix contents (GetPrefix),
    // then subscribe to changes. Returns false on the initial GetPrefix
    // error (registry left as-is — fail safe: a node that can't read the
    // identity table keeps whatever mappings it had rather than blanking
    // them and locking every tenant out).
    bool Start();
    void Stop();

    // Parsed identity-entry fields. Either spiffe_id or cn (or both) is
    // present; tenant is the authoritative tenant string. Static +
    // pure so the JSON contract is unit-testable without etcd.
    struct Entry {
        std::string       spiffe_id;  // may be empty
        std::string       cn;         // may be empty
        security::Identity identity;  // .tenant + .cn + .kind populated
    };
    static std::optional<Entry> ParseEntry(const std::string& json);

   private:
    // Apply / remove one parsed entry to/from the registry.
    void Apply(const Entry& e);
    void Remove(const Entry& e);

    IEtcdClient&             etcd_;
    security::MtlsRegistry&  registry_;
    std::string              prefix_;
    WatchHandle              handle_ = 0;
    bool                     watching_ = false;
};

}  // namespace kvcache::node::cluster
