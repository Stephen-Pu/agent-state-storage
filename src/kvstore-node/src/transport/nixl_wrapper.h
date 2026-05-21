// LLD §3.5 — NIXL transport facade.
//
// NIXL (NVIDIA's transfer library) is the data plane. It selects an actual
// backend at runtime — UCX over InfiniBand / RoCE, GPUDirect RDMA, NVMe-oF,
// NVLink, GDS, or plain TCP — based on link capabilities.
//
// Our policy (LLD invariants §4.4):
//   * Server-Pull only — the server initiates the DMA, never the client.
//     This is what makes priority scheduling possible: the scheduler sits
//     server-side and decides admission.
//   * Zero-copy on the local hot path — caller memory is registered once as
//     a memory region (MR) and reused across operations.
//
// Distributed model:
//   * Each backend exposes its local memory regions via `ExportMr`, which
//     returns an opaque `RemoteMrDescriptor`. The descriptor is serialised
//     to bytes for transmission (typically inside a gRPC Fetch request).
//   * The peer backend ingests the descriptor via `ImportRemoteMr`, getting
//     back a local MrKey that refers to the *remote* memory.
//   * `Pull` accepts a `src_mr` that may be either a local-registered MR or
//     an imported remote MR; the backend dispatches accordingly (memcpy
//     for local, network transfer for remote).
//
// We don't depend on the upstream NIXL headers in this codebase directly;
// instead we define an INixlBackend abstract and ship concrete backends
// (LoopbackBackend for intra-process tests, TcpBackend for real
// cross-process / cross-node transfers, future UcxBackend for RDMA).
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace kvcache::node::transport {

using MrKey         = uint32_t;
using CompletionId  = uint64_t;

inline constexpr MrKey         kInvalidMrKey        = 0;
inline constexpr CompletionId  kInvalidCompletionId = 0;

// Opaque, backend-specific encoding of a memory region that lives on a
// peer process / node. Typical contents: host:port + backend's local MR id
// + bytes. Serialised by the producing backend, deserialised by the
// consuming backend; meaning is private to the backend kind.
struct RemoteMrDescriptor {
    // ASCII or binary; backends agree on the format. Empty = invalid.
    std::vector<uint8_t> opaque;
};

struct PullRequest {
    MrKey    dst_mr;
    uint64_t dst_off;
    MrKey    src_mr;
    uint64_t src_off;
    uint64_t bytes;
};

class INixlBackend {
   public:
    virtual ~INixlBackend() = default;

    virtual std::string Name() const = 0;

    // ---- Local MR management ----

    // Register a host- or device-memory region with the transport. Returns a
    // non-zero key on success; sets `err` on failure.
    virtual MrKey RegisterRegion(void* addr, std::size_t bytes, std::string* err) = 0;

    virtual void  UnregisterRegion(MrKey key) = 0;

    // Resolve a MR key to (addr, bytes). Only meaningful for LOCAL MR keys
    // (those returned by RegisterRegion on this backend). For imported
    // remote MRs the address is not meaningful; callers must inspect
    // `bytes` only or use a backend-specific path. Returns false if unknown.
    virtual bool  ResolveRegion(MrKey key, void** addr, std::size_t* bytes) const = 0;

    // ---- Remote MR exchange (cross-process / cross-node) ----

    // Export a local MR as a descriptor that peer backends can use. Returns
    // false on unknown / non-exportable key. The descriptor is meant to be
    // shipped over the wire (e.g. inside a gRPC FetchRequest) to the peer
    // that will perform the Pull.
    virtual bool ExportMr(MrKey local_key,
                          RemoteMrDescriptor* out_desc,
                          std::string* err) = 0;

    // Import a peer's descriptor into this backend, returning a fresh local
    // MrKey usable as `src_mr` in a subsequent Pull. Returns kInvalidMrKey
    // on failure. The imported key has the same lifetime semantics as a
    // locally-registered MR — call `UnregisterRegion` to release it.
    virtual MrKey ImportRemoteMr(const RemoteMrDescriptor& desc,
                                 std::string* err) = 0;

    // ---- Transfer ----

    // Issue a server-initiated Pull. `src_mr` may be either a local-
    // registered MR or an imported remote MR; the backend dispatches.
    // Returns a completion id; caller calls Wait() to block. Some backends
    // (loopback, TCP synchronous mode) complete inline.
    virtual CompletionId Pull(const PullRequest& req, std::string* err) = 0;

    // Wait for a completion. Returns true if completed within `timeout_ms`,
    // false on timeout or unknown id.
    virtual bool Wait(CompletionId cid, uint32_t timeout_ms, std::string* err) = 0;
};

// ---------------------------------------------------------------------------
// Backend factory
// ---------------------------------------------------------------------------

struct BackendOptions {
    std::string name = "loopback";   // "loopback" | "tcp" | "ucx" | "gdr" | ...
    // Backend-specific options below. The factory ignores fields irrelevant
    // to the selected backend.
    std::string device;              // e.g. "mlx5_0" for UCX
    std::string bind_host = "127.0.0.1";  // TcpBackend listener host
    uint32_t    bind_port = 0;            // TcpBackend listener port (0 = OS-picked)
    uint32_t    listen_backlog = 32;      // TcpBackend SOMAXCONN cap
};

std::unique_ptr<INixlBackend> CreateBackend(const BackendOptions& opts, std::string* err);

// ---------------------------------------------------------------------------
// NixlWrapper — convenience around a backend pointer. Owns the registered MR
// table on behalf of higher layers so callers don't have to remember which
// keys belong to which backend.
// ---------------------------------------------------------------------------

class NixlWrapper {
   public:
    explicit NixlWrapper(std::unique_ptr<INixlBackend> backend);

    INixlBackend* backend() noexcept { return backend_.get(); }
    const std::string& BackendName() const { return name_; }

    MrKey Register(void* addr, std::size_t bytes, std::string* err);
    void  Unregister(MrKey key);

    // Convenience: submit and synchronously wait. Real fetch paths should use
    // the async pair (Pull → Wait) so the Priority Scheduler stays in charge.
    bool PullSync(const PullRequest& req, uint32_t timeout_ms, std::string* err);

   private:
    std::unique_ptr<INixlBackend> backend_;
    std::string name_;
};

}  // namespace kvcache::node::transport
