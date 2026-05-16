// kvstore-node entrypoint.
//
// Boot sequence (LLD §4.1 membership FSM, §7.3 crash recovery):
//   1. Parse config (cluster, NUMA pinning, tier capacities).
//   2. Open RocksDB; replay sealed-chunk index into ART.
//   3. Initialize tiers (Pinned/DRAM/NVMe; HBM is engine-owned, Cold via Alluxio).
//   4. Register NIXL memory sections.
//   5. Start gRPC server (peer + CP).
//   6. Etcd register → state=joining.
//   7. Warm-up window (5 min, 0.1× traffic weight).
//   8. Transition state=active.
//
// Shutdown (graceful drain, LLD §6.3):
//   - K8s preStop hook → state=draining.
//   - Refuse new reserves; let in-flight handles complete (≤5 min).
//   - Flush audit ring buffer.
//   - Deregister from Etcd; exit.
#include <csignal>
#include <cstdio>

namespace kvcache::node {

class NodeApp {
   public:
    int Run(int /*argc*/, char** /*argv*/) {
        // TODO(stephen): implement boot sequence above.
        std::puts("kvstore-node: boot sequence not yet implemented");
        return 0;
    }
};

}  // namespace kvcache::node

int main(int argc, char** argv) {
    kvcache::node::NodeApp app;
    return app.Run(argc, argv);
}
