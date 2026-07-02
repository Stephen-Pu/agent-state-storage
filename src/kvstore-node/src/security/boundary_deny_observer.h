// src/kvstore-node/src/security/boundary_deny_observer.h
//
// A10 Regulated Mode — BoundaryDenyObserver factory.
//
// Builds the deny-observer callback that the GuardedHttpTransport calls each
// time it blocks an outbound request. The observer:
//
//   1. Calls `incr_metric()` (if set) — the caller binds this to a
//      `kv_boundary_denied_total` Counter increment so the security/ TU stays
//      free of the metrics:: dependency.
//
//   2. Calls `audit->Append(rec)` (if audit != nullptr) — records an
//      AuditRecord with Decision::kDeny and the deny reason in `message`.
//
// Both sinks are optional: a null audit and/or an empty metric lambda
// degrade gracefully, keeping the observer unit-testable without a real
// metrics registry or AuditLog.
//
// Include-direction note:
//   boundary_guard.h originally defined `struct Decision` while rbac.h defines
//   `enum class Decision`, both in kvcache::node::security — a name collision
//   that prevented both headers from appearing in the same translation unit.
//   The struct was renamed `BoundaryDecision` in boundary_guard.h to resolve
//   the conflict; this header now safely includes both.
//
//   The returned type is std::function<void(const Endpoint&, std::string_view)>,
//   which is structurally identical to tier::GuardedHttpTransport::DenyObserver
//   (an alias of the same std::function signature) and therefore directly
//   assignable to it without a cast. We avoid including tier/guarded_transport.h
//   here to keep the dependency direction one-way (security/ must not import
//   tier/).
#pragma once

#include <chrono>
#include <functional>
#include <string>
#include <string_view>

#include "security/audit.h"          // AuditLog, AuditRecord
#include "security/boundary_guard.h" // Endpoint (BoundaryDecision no longer conflicts)
#include "security/mtls.h"           // IdentityKind
#include "security/rbac.h"           // Action, Decision (enum class)

namespace kvcache::node::security {

// Build a deny observer that increments the metric counter and appends an
// audit record on each call.
//
//   incr_metric — nullary lambda; called once per deny. Pass {} to skip.
//   audit       — AuditLog to append to. Pass nullptr to skip.
//
// Returns: std::function<void(const Endpoint&, std::string_view)>
//   Assignable directly to tier::GuardedHttpTransport::DenyObserver (identical
//   underlying std::function type).
inline std::function<void(const Endpoint&, std::string_view)>
MakeBoundaryDenyObserver(std::function<void()> incr_metric, AuditLog* audit) {
    return [incr_metric = std::move(incr_metric),
            audit](const Endpoint& /*ep*/, std::string_view reason) {
        // 1. Metric counter.
        if (incr_metric) {
            incr_metric();
        }

        // 2. Audit record.
        if (audit) {
            AuditRecord rec{};
            rec.at          = std::chrono::system_clock::now();
            // kInternal: the boundary-guard subsystem is an internal component.
            rec.kind        = IdentityKind::kInternal;
            // Synthetic system CN identifying the boundary enforcement point.
            rec.cn          = "boundary-guard";
            // kFetch: the blocked operation was an outbound fetch attempt.
            rec.action      = Action::kFetch;
            rec.decision    = Decision::kDeny;
            rec.tenant_hash = 0;
            rec.message     = std::string(reason);
            audit->Append(rec);
        }
    };
}

}  // namespace kvcache::node::security
