// A10 — Regulated Mode startup gate in NodeRuntime.
// Verifies that NodeRuntime refuses to bind (Ok()==false) when Regulated Mode
// is enabled and a configured sink is out of boundary, passes when all sinks
// are in boundary, and is a no-op when disabled.
#include "runtime/node_runtime.h"
#include <gtest/gtest.h>
using kvcache::node::runtime::NodeRuntime;
using namespace kvcache::node::security;

// Use port 0 so the OS picks a free port and the test never conflicts.
static NodeRuntime::Options BaseOpts() {
    NodeRuntime::Options o;
    o.grpc_port = 0; o.metrics_port = 0; o.skip_grpc_listener = true;
    return o;
}

TEST(NodeRuntimeRegulated, OutOfBoundarySinkRefusesToStart) {
    auto o = BaseOpts();
    o.regulated.enabled = true;
    o.regulated.allow_rules = {"*.svc.local"};
    o.regulated.configured_sinks = {
        {.host = "s3.public.io", .purpose = Purpose::kColdTier},  // out of boundary
    };
    NodeRuntime rt(o);
    EXPECT_FALSE(rt.Ok()) << "must refuse to bind when a sink is out of boundary";
    EXPECT_NE(rt.error().find("s3.public.io"), std::string::npos)
        << "error must name the offending sink";
}

TEST(NodeRuntimeRegulated, InBoundaryStartsOk) {
    auto o = BaseOpts();
    o.regulated.enabled = true;
    o.regulated.allow_rules = {"*.svc.local"};
    o.regulated.configured_sinks = {{.host = "s3.svc.local", .purpose = Purpose::kColdTier}};
    NodeRuntime rt(o);
    EXPECT_TRUE(rt.Ok()) << rt.error();
}

TEST(NodeRuntimeRegulated, DisabledIsUnchanged) {
    auto o = BaseOpts();                 // regulated.enabled = false
    o.regulated.configured_sinks = {{.host = "anywhere.evil.com", .purpose = Purpose::kColdTier}};
    NodeRuntime rt(o);
    EXPECT_TRUE(rt.Ok()) << "disabled Regulated Mode never blocks startup";
}
