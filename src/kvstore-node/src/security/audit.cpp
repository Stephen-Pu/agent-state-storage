// LLD §5.2 — AuditLog.
#include "security/audit.h"

#include <sstream>

namespace kvcache::node::security {

std::string SerializeAudit(const AuditRecord& rec) {
    using namespace std::chrono;
    const auto unix_ns = duration_cast<nanoseconds>(
        rec.at.time_since_epoch()).count();
    std::ostringstream os;
    os << "{\"ts\":" << unix_ns
       << ",\"kind\":\"" << IdentityKindName(rec.kind)
       << "\",\"cn\":\"" << rec.cn
       << "\",\"action\":\"" << ActionName(rec.action)
       << "\",\"decision\":\"" << (rec.decision == Decision::kAllow ? "allow" : "deny")
       << "\",\"tenant\":" << rec.tenant_hash;
    if (!rec.message.empty()) os << ",\"msg\":\"" << rec.message << "\"";
    os << "}";
    return os.str();
}

AuditLog::AuditLog(const Options& opts, ConsumerFn consumer)
    : opts_(opts), consumer_(std::move(consumer)),
      buffer_(opts.ring_capacity) {
    drainer_ = std::thread([this] { DrainLoop(); });
}

AuditLog::~AuditLog() { Stop(); }

void AuditLog::Stop() {
    if (drainer_.joinable()) {
        stop_.store(true, std::memory_order_release);
        drainer_.join();
    }
}

bool AuditLog::Append(const AuditRecord& rec) {
    std::lock_guard lk(mu_);
    if (size_ == buffer_.size()) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    buffer_[tail_] = rec;
    tail_ = (tail_ + 1) % buffer_.size();
    ++size_;
    return true;
}

std::size_t AuditLog::QueueSize() const {
    std::lock_guard lk(mu_);
    return size_;
}

void AuditLog::DrainLoop() {
    while (!stop_.load(std::memory_order_acquire)) {
        // Drain whatever's available. Holding the mutex for one record at a
        // time bounds latency for hot-path Append callers.
        for (;;) {
            AuditRecord rec;
            {
                std::lock_guard lk(mu_);
                if (size_ == 0) break;
                rec = buffer_[head_];
                head_ = (head_ + 1) % buffer_.size();
                --size_;
            }
            if (consumer_) consumer_(rec);
            delivered_.fetch_add(1, std::memory_order_relaxed);
        }
        std::this_thread::sleep_for(opts_.drain_interval);
    }
    // Final flush.
    for (;;) {
        AuditRecord rec;
        {
            std::lock_guard lk(mu_);
            if (size_ == 0) break;
            rec = buffer_[head_];
            head_ = (head_ + 1) % buffer_.size();
            --size_;
        }
        if (consumer_) consumer_(rec);
        delivered_.fetch_add(1, std::memory_order_relaxed);
    }
}

}  // namespace kvcache::node::security
