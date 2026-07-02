// src/kvstore-node/src/runtime/regulated_flags.h
//
// A10 — Task 3: Regulated-mode CLI flag helper.
//
// Converts the tokenised --regulated-mode / --boundary-allow CLI surface into a
// security::RegulatedModeConfig.  The helper is pure (no globals, no I/O) so
// it is unit-testable without spinning up a NodeRuntime or parsing real argv.
//
// main.cpp stays thin: it parses flags into RegulatedFlags, assembles the
// vector<Endpoint> egress sinks it already knows about, then calls
// BuildRegulatedModeConfig to produce the config it hands to
//   - NodeRuntime::Options::regulated  (Task 2 startup gate), and
//   - ColdTierOptions::guard / deny_observer  (Task 1 transport guard).
#pragma once
#include <string>
#include <vector>
#include "security/regulated_mode.h"  // RegulatedModeConfig, ValidateRegulatedMode

namespace kvcache::node::runtime {

// Parsed representation of the regulated-mode CLI flags:
//   --regulated-mode            → enabled = true
//   --boundary-allow <rule>     → appended to allow_rules (repeatable)
struct RegulatedFlags {
    bool                     enabled     = false;
    std::vector<std::string> allow_rules;
};

// Build a RegulatedModeConfig from the parsed flags and the caller-supplied
// egress-sink list.  Pure + testable: copies enabled, allow_rules, and sinks
// directly; leaves requirements empty (FIPS/KMS/audit are deferred controls).
security::RegulatedModeConfig
BuildRegulatedModeConfig(const RegulatedFlags&                    flags,
                         const std::vector<security::Endpoint>&   egress_sinks);

}  // namespace kvcache::node::runtime
