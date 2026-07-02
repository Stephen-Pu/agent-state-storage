// src/kvstore-node/src/security/regulated_mode.h
#pragma once
#include <functional>
#include <optional>
#include <string>
#include <vector>
#include "security/boundary_guard.h"

namespace kvcache::node::security {

// Injectable requirement check for a deferred control (FIPS / KMS / audit).
// Returns std::nullopt if satisfied, or an error string naming what's missing.
// This is the seam where the deferred §6 controls plug in later; for now the
// caller supplies concrete checks (or none, in a non-strict test).
using RequirementCheck = std::function<std::optional<std::string>()>;

struct RegulatedModeConfig {
    bool enabled = false;                       // R4: off by default
    std::vector<std::string> allow_rules;       // operator allowlist (host globs / CIDRs)
    std::vector<Endpoint> configured_sinks;     // every statically-configured egress endpoint
    std::vector<RequirementCheck> requirements; // FIPS/KMS/audit presence (deferred controls)
};

struct ValidationResult { bool ok = false; std::string error; };

// Fail-closed startup gate. If cfg.enabled is false → {ok=true} (no-op, unchanged behavior).
// If enabled: build the guard, Check EVERY configured sink (refuse on first deny,
// error names the offending sink + reason), then run EVERY requirement check
// (refuse on first unmet). Returns {ok=true} only if all pass.
ValidationResult ValidateRegulatedMode(const RegulatedModeConfig& cfg);

}  // namespace kvcache::node::security
