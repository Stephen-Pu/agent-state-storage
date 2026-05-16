// LLD §3.5 — NIXL transport facade, plus a loopback backend.
//
// The loopback backend is sufficient for:
//   * Unit tests (no RDMA / GPU dependency).
//   * Single-process integration tests.
//   * Bring-up on dev laptops.
//
// Real backends (UCX, GDR, NVMe-oF, NVLink, TCP) plug in behind INixlBackend.
// They are TODO and selected by name via CreateBackend.
#include "transport/nixl_wrapper.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace kvcache::node::transport {

// ---------------------------------------------------------------------------
// LoopbackBackend
// ---------------------------------------------------------------------------

class LoopbackBackend final : public INixlBackend {
   public:
    std::string Name() const override { return "loopback"; }

    MrKey RegisterRegion(void* addr, std::size_t bytes, std::string* err) override {
        if (!addr || bytes == 0) {
            if (err) *err = "loopback: invalid region";
            return kInvalidMrKey;
        }
        std::lock_guard lk(mu_);
        const MrKey k = next_key_.fetch_add(1, std::memory_order_relaxed);
        regions_.emplace(k, Region{addr, bytes});
        return k;
    }

    void UnregisterRegion(MrKey key) override {
        std::lock_guard lk(mu_);
        regions_.erase(key);
    }

    bool ResolveRegion(MrKey key, void** addr, std::size_t* bytes) const override {
        std::lock_guard lk(mu_);
        auto it = regions_.find(key);
        if (it == regions_.end()) return false;
        if (addr)  *addr  = it->second.addr;
        if (bytes) *bytes = it->second.bytes;
        return true;
    }

    CompletionId Pull(const PullRequest& req, std::string* err) override {
        void *src_addr = nullptr, *dst_addr = nullptr;
        std::size_t src_n = 0, dst_n = 0;
        {
            std::lock_guard lk(mu_);
            auto it_s = regions_.find(req.src_mr);
            auto it_d = regions_.find(req.dst_mr);
            if (it_s == regions_.end() || it_d == regions_.end()) {
                if (err) *err = "loopback: unknown MR key";
                return kInvalidCompletionId;
            }
            src_addr = it_s->second.addr; src_n = it_s->second.bytes;
            dst_addr = it_d->second.addr; dst_n = it_d->second.bytes;
        }
        if (req.src_off + req.bytes > src_n || req.dst_off + req.bytes > dst_n) {
            if (err) *err = "loopback: out-of-bounds Pull";
            return kInvalidCompletionId;
        }
        std::memcpy(static_cast<uint8_t*>(dst_addr) + req.dst_off,
                    static_cast<uint8_t*>(src_addr) + req.src_off,
                    req.bytes);
        // Synchronously completed; the completion id is purely a token so
        // callers can mirror the async path on real backends.
        return next_completion_.fetch_add(1, std::memory_order_relaxed);
    }

    bool Wait(CompletionId cid, uint32_t /*timeout_ms*/, std::string* err) override {
        if (cid == kInvalidCompletionId) {
            if (err) *err = "loopback: invalid completion id";
            return false;
        }
        return true;  // loopback Pulls are synchronous-on-issue.
    }

   private:
    struct Region { void* addr; std::size_t bytes; };
    mutable std::mutex mu_;
    std::unordered_map<MrKey, Region> regions_;
    std::atomic<MrKey>        next_key_{1};
    std::atomic<CompletionId> next_completion_{1};
};

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

std::unique_ptr<INixlBackend> CreateBackend(const BackendOptions& opts, std::string* err) {
    if (opts.name == "loopback") {
        return std::make_unique<LoopbackBackend>();
    }
    // TODO(stephen): plug in real backends:
    //   "ucx"   — UCX over IB / RoCE (LLD §3.5, D-NET-1)
    //   "gdr"   — GPUDirect RDMA
    //   "gds"   — GPUDirect Storage (NVMe → GPU direct, LLD §3.3 T3)
    //   "tcp"   — plain TCP fallback
    //   "nvlink"— intra-host NVLink fabric
    if (err) *err = "nixl: unknown backend '" + opts.name + "'";
    return nullptr;
}

// ---------------------------------------------------------------------------
// NixlWrapper
// ---------------------------------------------------------------------------

NixlWrapper::NixlWrapper(std::unique_ptr<INixlBackend> backend)
    : backend_(std::move(backend)),
      name_(backend_ ? backend_->Name() : std::string()) {}

MrKey NixlWrapper::Register(void* addr, std::size_t bytes, std::string* err) {
    return backend_ ? backend_->RegisterRegion(addr, bytes, err) : kInvalidMrKey;
}
void NixlWrapper::Unregister(MrKey key) {
    if (backend_) backend_->UnregisterRegion(key);
}
bool NixlWrapper::PullSync(const PullRequest& req, uint32_t timeout_ms, std::string* err) {
    if (!backend_) { if (err) *err = "nixl: no backend"; return false; }
    auto cid = backend_->Pull(req, err);
    if (cid == kInvalidCompletionId) return false;
    return backend_->Wait(cid, timeout_ms, err);
}

}  // namespace kvcache::node::transport
