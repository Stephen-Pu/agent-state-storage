// LLD §5.2 — Audit log: in-memory ring buffer + async spill.
//
// Audit records the security-relevant ops:
//   * Identity (cn, kind)
//   * Action + result (Allow/Deny)
//   * Tenant target
//   * Wall-clock time
//   * Optional message
//
// Hot path: append to a bounded SPSC-ish ring (single mutex protects the
// queue; producer never blocks beyond a mutex acquisition). A background
// consumer thread drains the ring to:
//   * The local audit log stream → Alluxio (LLD §6.2 — independent path).
//   * The RocksDB audit_buffer_overflow CF when the audit stream is down
//     (LLD §2.3).
//
// MVP simplification: the spill consumer is a callback the integrator supplies.
// The default callback writes to RocksDB via meta::RocksdbStore::AppendAuditOverflow.
// Replace with a real Alluxio writer at Step-9 time.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "security/mtls.h"
#include "security/rbac.h"

namespace kvcache::node::security {

struct AuditRecord {
    std::chrono::system_clock::time_point at;
    IdentityKind  kind;
    std::string   cn;
    Action        action;
    Decision      decision;
    uint64_t      tenant_hash;
    std::string   message;       // optional context (e.g. error string)
};

// Serializer for one record. Layout is line-oriented JSON ("ndjson") so the
// downstream stream can be tail-followed without a parser.
std::string SerializeAudit(const AuditRecord& rec);

class AuditLog {
   public:
    struct Options {
        std::size_t                 ring_capacity = 16 * 1024;
        std::chrono::milliseconds   drain_interval{200};
    };
    using ConsumerFn = std::function<void(const AuditRecord&)>;

    AuditLog(const Options& opts, ConsumerFn consumer);
    ~AuditLog();

    AuditLog(const AuditLog&)            = delete;
    AuditLog& operator=(const AuditLog&) = delete;

    // Hot-path append. Returns false (and increments dropped counter) when
    // the ring is full — caller is expected to log a metric.
    bool Append(const AuditRecord& rec);

    void Stop();

    uint64_t DroppedCount()   const noexcept { return dropped_.load(std::memory_order_relaxed); }
    uint64_t DeliveredCount() const noexcept { return delivered_.load(std::memory_order_relaxed); }
    std::size_t QueueSize()   const;

   private:
    void DrainLoop();

    Options                    opts_;
    ConsumerFn                 consumer_;
    mutable std::mutex         mu_;
    std::vector<AuditRecord>   buffer_;       // simple bounded queue (vector + index)
    std::size_t                head_ = 0;
    std::size_t                tail_ = 0;
    std::size_t                size_ = 0;
    std::atomic<uint64_t>      dropped_{0};
    std::atomic<uint64_t>      delivered_{0};
    std::atomic<bool>          stop_{false};
    std::thread                drainer_;
};

}  // namespace kvcache::node::security
