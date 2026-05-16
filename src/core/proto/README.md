# core/proto

gRPC / Protobuf schemas shared across components.

- `node.proto` — Node ↔ Node RPCs (lookup, fetch handle, publish, seal).
- `cp.proto` — Node ↔ Control-Plane RPCs (register, heartbeat, bloom sync,
  quota updates, config push). LLD §4.1.
- `events.proto` — KV event stream (LLD §2.2).

Generated code lands in `build/proto/` (C++) and `control-plane/internal/pb/` (Go).

TODO(stephen): add `node.proto`, `cp.proto`, `events.proto`; wire `protoc` into
the top-level CMake and Go build.
