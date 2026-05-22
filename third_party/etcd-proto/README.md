# etcd v3 protos — vendored subset

This directory holds a minimal, hand-curated subset of the etcd v3
protobuf API, used by `GrpcEtcdClient` (Phase F-2).

## Source

Upstream: [etcd-io/etcd][etcd] release-3.5 branch. Files mapped:

| Local                      | Upstream                                   |
|----------------------------|--------------------------------------------|
| `mvccpb/kv.proto`          | `api/mvccpb/kv.proto`                       |
| `etcdserverpb/rpc.proto`   | `api/etcdserverpb/rpc.proto` (subset)       |

[etcd]: https://github.com/etcd-io/etcd

## What we strip

* **gogoproto annotations** — etcd's upstream proto files lean on
  `gogo/protobuf` extensions for Go-side ergonomics (`stdtime`,
  `customname`, `nullable=false`). They have no semantic effect on
  the C++ wire and need a `gogo.proto` import that we'd rather not
  vendor. Stripped.
* **google.api annotations** — the upstream files map RPCs to a
  REST gateway via `google.api.http`. We have a separate
  `HttpEtcdClient` for the JSON gateway, so the C++ gRPC client
  doesn't need this mapping. Stripped.
* **Auth / Cluster / Maintenance services** — not used by the
  operator or kvstore-node paths.
* **Watch streaming** — Phase F-2 polls (matching `HttpEtcdClient`).
  Phase F-3 will reintroduce the bidirectional Watch stream.

The result is two self-contained `.proto` files with only the
standard `import "mvccpb/kv.proto";` cross-link — no `google/api/*`,
no `gogoproto/gogo.proto`, no `google/protobuf/*` (we deliberately
avoid `Timestamp` to keep the dependency graph trivial).

## Regenerating

The C++ stubs are generated at build time by `protoc` + the
`grpc_cpp_plugin` (CMake handles this in `src/CMakeLists.txt` when
`find_package(gRPC CONFIG)` succeeds). Nothing to commit beyond the
`.proto` source.
