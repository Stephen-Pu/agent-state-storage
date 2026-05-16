// kvagent entrypoint.
//
// LLD §6.1.5:
//   - Open /dev/shm SQ/CQ rings for each registered engine.
//   - Connect to local kvstore-node (UNIX socket).
//   - Subscribe to KV event stream from primary node.
//   - Periodically pull bloom-sketch view from peers (30 s tick).
#include <cstdio>

int main(int /*argc*/, char** /*argv*/) {
    // TODO(stephen): implement engine ring setup + node connection + event loop.
    std::puts("kvagent: boot sequence not yet implemented");
    return 0;
}
