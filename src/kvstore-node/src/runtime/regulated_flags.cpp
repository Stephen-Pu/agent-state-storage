// src/kvstore-node/src/runtime/regulated_flags.cpp
//
// A10 — Task 3: Implementation of the regulated-mode CLI flag helper.
#include "runtime/regulated_flags.h"

namespace kvcache::node::runtime {

security::RegulatedModeConfig
BuildRegulatedModeConfig(const RegulatedFlags&                   flags,
                         const std::vector<security::Endpoint>&  egress_sinks) {
    security::RegulatedModeConfig cfg;
    cfg.enabled          = flags.enabled;
    cfg.allow_rules      = flags.allow_rules;
    cfg.configured_sinks = egress_sinks;
    // cfg.requirements is intentionally left empty here.  The deferred
    // controls (FIPS provider, IKeyProvider/KMS, hash-chained audit) plug in
    // by pushing their RequirementCheck predicates onto cfg.requirements when
    // those features land.  An empty requirements vector means ValidateRegulatedMode
    // skips the requirement loop — correct for the current (no-deferred-checks) case.
    return cfg;
}

}  // namespace kvcache::node::runtime
