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
//   * Zero-copy — caller memory is registered once as a memory region (MR)
//     and reused across operations. The MR key is returned at registration
//     and carried in every Pull.
//
// We don't depend on the NIXL headers in this codebase directly; instead we
// define an INixlBackend abstract and ship one in-memory loopback backend.
// The real backends (UCX, GDR, GDS, …) plug in behind the same interface and
// are selected by name via the factory.
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

    // Register a host- or device-memory region with the transport. Returns a
    // non-zero key on success; sets `err` on failure.
    virtual MrKey RegisterRegion(void* addr, std::size_t bytes, std::string* err) = 0;

    virtual void  UnregisterRegion(MrKey key) = 0;

    // Resolve a MR key to (addr, bytes). Used by transports that need the
    // physical address (e.g. the loopback backend). Returns false if unknown.
    virtual bool  ResolveRegion(MrKey key, void** addr, std::size_t* bytes) const = 0;

    // Issue a server-initiated Pull. Returns a completion id; caller calls
    // Wait() to block on completion. The bytes are NOT necessarily transferred
    // by the time Pull returns — some backends (loopback) complete inline.
    virtual CompletionId Pull(const PullRequest& req, std::string* err) = 0;

    // Wait for a completion. Returns true if completed within `timeout_ms`,
    // false on timeout or unknown id.
    virtual bool Wait(CompletionId cid, uint32_t timeout_ms, std::string* err) = 0;
};

// ---------------------------------------------------------------------------
// Backend factory
// ---------------------------------------------------------------------------

struct BackendOptions {
    std::string name = "loopback";   // "loopback" | "ucx" | "gdr" | "tcp" | "nvlink"
    // Backend-specific options below. The factory ignores fields irrelevant
    // to the selected backend.
    std::string device;              // e.g. "mlx5_0" for UCX
    uint32_t    tcp_port    = 0;
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
