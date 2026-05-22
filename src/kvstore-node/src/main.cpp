// kvstore-node entrypoint.
//
// Phase L-1 scope: bring the binary up to the smallest "Ready pod"
// surface so the operator's StatefulSet + TCPSocket probe actually
// pass when run under kind / production. Real boot sequence (etcd
// register, tier init, NIXL register, gRPC handlers) is layered on top
// of this in later phases.
//
// What this binary does today:
//
//   1. Parses --config (kvcache-node.yaml path). We don't open the
//      file yet — the operator-emitted ConfigMap is correct shape,
//      the consumer just hasn't been wired through. Stored for
//      future use; printed at startup for log breadcrumbs.
//   2. Starts a NodeRuntime: TCP accept-loop on the grpc port (passes
//      the operator readiness probe) + tiny HTTP server on the
//      metrics port (/metrics + /healthz).
//   3. Blocks until SIGTERM (K8s pod evict) or SIGINT (Ctrl-C).
//
// Anything else from the historical TODO docblock (RocksDB replay,
// tier init, NIXL register, etcd join, warm-up window, draining) is
// the explicit next layer.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "runtime/node_runtime.h"

namespace {

void Usage(const char* argv0) {
    std::fprintf(stderr,
        "usage: %s [--config PATH] [--grpc-port N] [--metrics-port N]\n",
        argv0);
}

bool TryFlag(int& i, int argc, char** argv,
              const char* flag, std::string* out) {
    if (std::strcmp(argv[i], flag) != 0) return false;
    if (i + 1 >= argc) return false;
    *out = argv[i + 1];
    ++i;
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    kvcache::node::runtime::NodeRuntime::Options o;
    std::string grpc_port_s    = "7000";
    std::string metrics_port_s = "9090";
    for (int i = 1; i < argc; ++i) {
        if (TryFlag(i, argc, argv, "--config",       &o.config_path)) continue;
        if (TryFlag(i, argc, argv, "--grpc-port",    &grpc_port_s))   continue;
        if (TryFlag(i, argc, argv, "--metrics-port", &metrics_port_s)) continue;
        if (std::strcmp(argv[i], "-h") == 0 ||
            std::strcmp(argv[i], "--help") == 0) {
            Usage(argv[0]);
            return 0;
        }
        std::fprintf(stderr, "unknown arg: %s\n", argv[i]);
        Usage(argv[0]);
        return 2;
    }
    o.grpc_port    = static_cast<uint16_t>(std::atoi(grpc_port_s.c_str()));
    o.metrics_port = static_cast<uint16_t>(std::atoi(metrics_port_s.c_str()));

    std::fprintf(stderr,
        "kvstore-node: starting (config=%s grpc=%u metrics=%u)\n",
        o.config_path.c_str(), o.grpc_port, o.metrics_port);

    kvcache::node::runtime::NodeRuntime rt(o);
    if (!rt.Ok()) {
        std::fprintf(stderr, "kvstore-node: %s\n", rt.error().c_str());
        return 1;
    }
    rt.Start();
    std::fprintf(stderr,
        "kvstore-node: listening (grpc=%u metrics=%u); SIGTERM to stop\n",
        rt.GrpcPort(), rt.MetricsPort());
    const int sig = rt.Wait();
    std::fprintf(stderr, "kvstore-node: stopped (signal=%d)\n", sig);
    return 0;
}
