// src/kvstore-node/src/security/regulated_mode.cpp
#include "security/regulated_mode.h"
#include "security/boundary_policy_builder.h"

namespace kvcache::node::security {

namespace {

const char* PurposeName(Purpose p) {
    switch (p) {
        case Purpose::kColdTier:    return "cold-tier";
        case Purpose::kKms:         return "kms";
        case Purpose::kTelemetry:   return "telemetry";
        case Purpose::kReplication: return "replication";
        case Purpose::kEtcd:        return "etcd";
        default:                    return "other";
    }
}

}  // namespace

ValidationResult ValidateRegulatedMode(const RegulatedModeConfig& cfg) {
    if (!cfg.enabled) return {true, ""};   // R4: off by default → unchanged behavior

    BoundaryGuard guard(BuildPolicy(cfg.allow_rules, /*default_deny=*/true));

    for (const auto& sink : cfg.configured_sinks) {
        Decision d = guard.Check(sink);
        if (!d.allow) {
            return {false, std::string("Regulated Mode: configured ") +
                           PurposeName(sink.purpose) + " sink '" + sink.host +
                           "' is out of boundary: " + d.reason};
        }
    }

    for (const auto& req : cfg.requirements) {
        if (auto err = req()) {
            return {false, "Regulated Mode: unmet requirement: " + *err};
        }
    }

    return {true, ""};
}

}  // namespace kvcache::node::security
