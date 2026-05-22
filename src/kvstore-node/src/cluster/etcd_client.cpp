// LLD §4.1 — Etcd client implementations.
#include "cluster/etcd_client.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#if defined(KVCACHE_HAVE_GRPC)
#  include <grpcpp/grpcpp.h>
#  include "etcdserverpb/rpc.pb.h"
#  include "etcdserverpb/rpc.grpc.pb.h"
#  include "mvccpb/kv.pb.h"
#endif

namespace kvcache::node::cluster {

// ===========================================================================
// InMemoryEtcdClient — semantic-faithful etcd v3 surface for tests / demos.
// ===========================================================================

InMemoryEtcdClient::InMemoryEtcdClient() : InMemoryEtcdClient(Options{}) {}

InMemoryEtcdClient::InMemoryEtcdClient(const Options& opts)
    : sweep_interval_(opts.lease_sweep_interval) {
    sweeper_ = std::thread([this] { SweeperLoop(); });
}

InMemoryEtcdClient::~InMemoryEtcdClient() {
    stop_.store(true, std::memory_order_release);
    if (sweeper_.joinable()) sweeper_.join();
}

void InMemoryEtcdClient::NotifyLocked(const WatchEvent& ev) {
    // Dispatch synchronously while holding mu_. Watchers must be cheap;
    // production use would post to a dispatcher thread (TODO(stephen)).
    for (const auto& [_, w] : watchers_) {
        if (ev.kv.key.rfind(w.prefix, 0) == 0) {
            w.cb(ev);
        }
    }
}

void InMemoryEtcdClient::ExpireLeaseLocked(LeaseId lease) {
    // Drop every KV bound to the lease and emit Delete events.
    for (auto it = kvs_.begin(); it != kvs_.end(); ) {
        if (it->second.lease == lease) {
            WatchEvent ev{WatchEventType::kDelete, it->second, it->second};
            NotifyLocked(ev);
            it = kvs_.erase(it);
        } else {
            ++it;
        }
    }
    leases_.erase(lease);
}

void InMemoryEtcdClient::SweeperLoop() {
    while (!stop_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(sweep_interval_);
        if (stop_.load(std::memory_order_acquire)) break;
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard lk(mu_);
        std::vector<LeaseId> expired;
        for (const auto& [id, l] : leases_) {
            if (l.deadline <= now) expired.push_back(id);
        }
        for (auto id : expired) ExpireLeaseLocked(id);
    }
}

bool InMemoryEtcdClient::Put(const std::string& key, const std::string& value,
                              LeaseId lease, Revision* out_rev,
                              std::string* err) {
    std::lock_guard lk(mu_);
    if (lease != kNoLease && leases_.find(lease) == leases_.end()) {
        if (err) *err = "etcd: lease not found"; return false;
    }
    ++revision_;
    auto it = kvs_.find(key);
    KeyValue prev{};
    bool had_prev = (it != kvs_.end());
    if (had_prev) prev = it->second;
    KeyValue cur{key, value, revision_,
                  had_prev ? prev.create_revision : revision_,
                  lease};
    kvs_[key] = cur;
    WatchEvent ev{WatchEventType::kPut, cur, had_prev ? prev : KeyValue{}};
    NotifyLocked(ev);
    if (out_rev) *out_rev = revision_;
    return true;
}

std::optional<KeyValue> InMemoryEtcdClient::Get(const std::string& key, std::string*) {
    std::lock_guard lk(mu_);
    auto it = kvs_.find(key);
    if (it == kvs_.end()) return std::nullopt;
    return it->second;
}

std::vector<KeyValue> InMemoryEtcdClient::GetPrefix(const std::string& prefix,
                                                    std::string*) {
    std::lock_guard lk(mu_);
    std::vector<KeyValue> out;
    for (const auto& [k, v] : kvs_) {
        if (k.rfind(prefix, 0) == 0) out.push_back(v);
    }
    std::sort(out.begin(), out.end(),
              [](const auto& a, const auto& b) { return a.key < b.key; });
    return out;
}

bool InMemoryEtcdClient::Delete(const std::string& key, std::string*) {
    std::lock_guard lk(mu_);
    auto it = kvs_.find(key);
    if (it == kvs_.end()) return false;
    WatchEvent ev{WatchEventType::kDelete, it->second, it->second};
    NotifyLocked(ev);
    kvs_.erase(it);
    ++revision_;
    return true;
}

bool InMemoryEtcdClient::PutIfRevision(const std::string& key,
                                        const std::string& value,
                                        Revision expected_rev,
                                        LeaseId lease,
                                        Revision* out_rev,
                                        std::string* err) {
    std::lock_guard lk(mu_);
    auto it = kvs_.find(key);
    const Revision actual = (it == kvs_.end()) ? 0 : it->second.mod_revision;
    if (actual != expected_rev) {
        if (err) *err = "etcd: revision mismatch";
        return false;
    }
    if (lease != kNoLease && leases_.find(lease) == leases_.end()) {
        if (err) *err = "etcd: lease not found"; return false;
    }
    ++revision_;
    KeyValue prev = (it != kvs_.end()) ? it->second : KeyValue{};
    KeyValue cur{key, value, revision_,
                  (it != kvs_.end()) ? it->second.create_revision : revision_,
                  lease};
    kvs_[key] = cur;
    WatchEvent ev{WatchEventType::kPut, cur,
                  (it != kvs_.end()) ? prev : KeyValue{}};
    NotifyLocked(ev);
    if (out_rev) *out_rev = revision_;
    return true;
}

LeaseId InMemoryEtcdClient::LeaseGrant(uint32_t ttl_seconds, std::string*) {
    std::lock_guard lk(mu_);
    LeaseId id = next_lease_.fetch_add(1, std::memory_order_relaxed);
    leases_[id] = Lease{
        ttl_seconds,
        std::chrono::steady_clock::now() + std::chrono::seconds(ttl_seconds)
    };
    return id;
}

bool InMemoryEtcdClient::LeaseKeepAlive(LeaseId id, std::string* err) {
    std::lock_guard lk(mu_);
    auto it = leases_.find(id);
    if (it == leases_.end()) {
        if (err) *err = "etcd: lease not found"; return false;
    }
    it->second.deadline =
        std::chrono::steady_clock::now() +
        std::chrono::seconds(it->second.ttl_seconds);
    return true;
}

bool InMemoryEtcdClient::LeaseRevoke(LeaseId id, std::string* err) {
    std::lock_guard lk(mu_);
    auto it = leases_.find(id);
    if (it == leases_.end()) {
        if (err) *err = "etcd: lease not found"; return false;
    }
    ExpireLeaseLocked(id);
    return true;
}

uint32_t InMemoryEtcdClient::LeaseTTLRemaining(LeaseId id) const {
    std::lock_guard lk(mu_);
    auto it = leases_.find(id);
    if (it == leases_.end()) return 0;
    const auto now = std::chrono::steady_clock::now();
    if (it->second.deadline <= now) return 0;
    return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::seconds>(
        it->second.deadline - now).count());
}

WatchHandle InMemoryEtcdClient::WatchPrefix(const std::string& prefix,
                                             WatchCallback cb) {
    std::lock_guard lk(mu_);
    auto id = next_watch_.fetch_add(1, std::memory_order_relaxed);
    watchers_[id] = Watcher{prefix, std::move(cb)};
    return id;
}

void InMemoryEtcdClient::Unwatch(WatchHandle handle) {
    std::lock_guard lk(mu_);
    watchers_.erase(handle);
}

// ===========================================================================
// GrpcEtcdClient — real etcd v3 over gRPC.
// ===========================================================================
//
// Phase F-2. Gated on KVCACHE_HAVE_GRPC: when the build found
// `find_package(gRPC CONFIG)` and the vendored etcd protos under
// `third_party/etcd-proto/`, the file links `kvcache_etcd_proto` and
// compiles the real implementation below. Otherwise the `#else` branch
// keeps a no-op fallback so the tree builds on dev laptops without grpc.
//
// Coverage matches HttpEtcdClient: KV (Put/Get/GetPrefix/Delete/
// PutIfRevision via Txn) + Lease (Grant/KeepAlive/Revoke/TTL). Watch
// is polling-based, identical to the HTTP variant — the bidirectional
// stream lands in Phase F-3.

struct GrpcEtcdClient::Impl {
#if defined(KVCACHE_HAVE_GRPC)
    Options                                   opts;
    std::shared_ptr<grpc::Channel>            channel;
    std::unique_ptr<etcdserverpb::KV::Stub>   kv_stub;
    std::unique_ptr<etcdserverpb::Lease::Stub> lease_stub;

    // Polling watcher (mirrors HttpEtcdClient).
    struct PollWatcher {
        std::string                                  prefix;
        WatchCallback                                cb;
        std::unordered_map<std::string, KeyValue>    last_state;
    };
    std::mutex                                   w_mu;
    std::unordered_map<WatchHandle, PollWatcher> watchers;
    std::atomic<WatchHandle>                     next_watch{1};
    std::atomic<bool>                            stop{false};
    std::thread                                  poller;
#endif
};

#if defined(KVCACHE_HAVE_GRPC)

namespace {

// `range_end` for "all keys with this prefix" is the prefix with the
// last byte incremented (etcd v3 convention; same shape as
// http_etcd_client.cpp).
std::string PrefixRangeEnd(const std::string& prefix) {
    std::string e = prefix;
    if (e.empty()) return std::string(1, '\0');
    for (std::size_t i = e.size(); i > 0; ) {
        --i;
        if (static_cast<unsigned char>(e[i]) < 0xff) {
            ++e[i];
            e.resize(i + 1);
            return e;
        }
    }
    return std::string{};
}

KeyValue FromPbKv(const mvccpb::KeyValue& pb) {
    KeyValue k;
    k.key             = pb.key();
    k.value           = pb.value();
    k.create_revision = static_cast<Revision>(pb.create_revision());
    k.mod_revision    = static_cast<Revision>(pb.mod_revision());
    k.lease           = static_cast<LeaseId>(pb.lease());
    return k;
}

// Strip an http:// prefix so a callers-supplied "http://host:2379"
// works the same way as a bare "host:2379" via clientv3.
std::string StripScheme(const std::string& s) {
    if (s.rfind("http://", 0) == 0)  return s.substr(7);
    if (s.rfind("https://", 0) == 0) return s.substr(8);
    return s;
}

}  // namespace

std::unique_ptr<GrpcEtcdClient>
GrpcEtcdClient::Create(const Options& opts, std::string* err) {
    if (opts.endpoints.empty()) {
        if (err) *err = "GrpcEtcdClient: endpoints required";
        return nullptr;
    }
    auto self  = std::unique_ptr<GrpcEtcdClient>(new GrpcEtcdClient());
    self->impl_ = std::make_unique<Impl>();
    self->impl_->opts = opts;

    // Build a comma-separated target string for the multi-endpoint
    // round-robin balancer. Per gRPC docs the scheme-less host:port
    // list is the canonical form.
    std::string target;
    for (std::size_t i = 0; i < opts.endpoints.size(); ++i) {
        if (i > 0) target.push_back(',');
        target += StripScheme(opts.endpoints[i]);
    }

    std::shared_ptr<grpc::ChannelCredentials> creds;
    if (!opts.ca_pem_path.empty() || !opts.client_cert_pem_path.empty()) {
        // Load the PEM material the operator emits into the Secret.
        auto read_file = [](const std::string& p) -> std::string {
            std::ifstream f(p); std::stringstream ss;
            if (f) ss << f.rdbuf();
            return ss.str();
        };
        grpc::SslCredentialsOptions sslo;
        if (!opts.ca_pem_path.empty()) sslo.pem_root_certs = read_file(opts.ca_pem_path);
        if (!opts.client_key_pem_path.empty()) sslo.pem_private_key = read_file(opts.client_key_pem_path);
        if (!opts.client_cert_pem_path.empty()) sslo.pem_cert_chain = read_file(opts.client_cert_pem_path);
        creds = grpc::SslCredentials(sslo);
    } else {
        creds = grpc::InsecureChannelCredentials();
    }

    self->impl_->channel = grpc::CreateChannel(target, creds);
    self->impl_->kv_stub    = etcdserverpb::KV::NewStub(self->impl_->channel);
    self->impl_->lease_stub = etcdserverpb::Lease::NewStub(self->impl_->channel);

    // Smoke-test: a benign Range(empty key). Fails fast when etcd is
    // unreachable so callers see the error at Create-time.
    grpc::ClientContext ctx;
    auto deadline = std::chrono::system_clock::now() + opts.dial_timeout;
    ctx.set_deadline(deadline);
    etcdserverpb::RangeRequest  req;
    etcdserverpb::RangeResponse resp;
    req.set_key(std::string(1, '\0'));
    auto s = self->impl_->kv_stub->Range(&ctx, req, &resp);
    if (!s.ok()) {
        if (err) *err = std::string("etcd dial: ") + s.error_message();
        return nullptr;
    }
    return self;
}

GrpcEtcdClient::~GrpcEtcdClient() {
    if (impl_) {
        impl_->stop.store(true, std::memory_order_release);
        if (impl_->poller.joinable()) impl_->poller.join();
    }
}

bool GrpcEtcdClient::Put(const std::string& key, const std::string& value,
                          LeaseId lease, Revision* out_rev, std::string* err) {
    etcdserverpb::PutRequest  req;
    etcdserverpb::PutResponse resp;
    req.set_key(key);
    req.set_value(value);
    req.set_lease(static_cast<int64_t>(lease));
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + impl_->opts.dial_timeout);
    auto s = impl_->kv_stub->Put(&ctx, req, &resp);
    if (!s.ok()) {
        if (err) *err = std::string("etcd Put: ") + s.error_message();
        return false;
    }
    if (out_rev && resp.has_header()) {
        *out_rev = static_cast<Revision>(resp.header().revision());
    }
    return true;
}

std::optional<KeyValue> GrpcEtcdClient::Get(const std::string& key,
                                              std::string* err) {
    etcdserverpb::RangeRequest  req;
    etcdserverpb::RangeResponse resp;
    req.set_key(key);
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + impl_->opts.dial_timeout);
    auto s = impl_->kv_stub->Range(&ctx, req, &resp);
    if (!s.ok()) {
        if (err) *err = std::string("etcd Get: ") + s.error_message();
        return std::nullopt;
    }
    if (resp.kvs_size() == 0) return std::nullopt;
    return FromPbKv(resp.kvs(0));
}

std::vector<KeyValue> GrpcEtcdClient::GetPrefix(const std::string& prefix,
                                                  std::string* err) {
    etcdserverpb::RangeRequest  req;
    etcdserverpb::RangeResponse resp;
    req.set_key(prefix);
    req.set_range_end(PrefixRangeEnd(prefix));
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + impl_->opts.dial_timeout);
    auto s = impl_->kv_stub->Range(&ctx, req, &resp);
    std::vector<KeyValue> out;
    if (!s.ok()) {
        if (err) *err = std::string("etcd GetPrefix: ") + s.error_message();
        return out;
    }
    out.reserve(resp.kvs_size());
    for (int i = 0; i < resp.kvs_size(); ++i) out.push_back(FromPbKv(resp.kvs(i)));
    return out;
}

bool GrpcEtcdClient::Delete(const std::string& key, std::string* err) {
    etcdserverpb::DeleteRangeRequest  req;
    etcdserverpb::DeleteRangeResponse resp;
    req.set_key(key);
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + impl_->opts.dial_timeout);
    auto s = impl_->kv_stub->DeleteRange(&ctx, req, &resp);
    if (!s.ok()) {
        if (err) *err = std::string("etcd Delete: ") + s.error_message();
        return false;
    }
    return resp.deleted() > 0;
}

bool GrpcEtcdClient::PutIfRevision(const std::string& key,
                                     const std::string& value,
                                     Revision expected_rev, LeaseId lease,
                                     Revision* out_rev, std::string* err) {
    etcdserverpb::TxnRequest  req;
    etcdserverpb::TxnResponse resp;
    auto* cmp = req.add_compare();
    cmp->set_target(etcdserverpb::Compare::MOD);
    cmp->set_result(etcdserverpb::Compare::EQUAL);
    cmp->set_key(key);
    cmp->set_mod_revision(static_cast<int64_t>(expected_rev));
    auto* op = req.add_success();
    auto* put = op->mutable_request_put();
    put->set_key(key);
    put->set_value(value);
    put->set_lease(static_cast<int64_t>(lease));
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + impl_->opts.dial_timeout);
    auto s = impl_->kv_stub->Txn(&ctx, req, &resp);
    if (!s.ok()) {
        if (err) *err = std::string("etcd Txn: ") + s.error_message();
        return false;
    }
    if (!resp.succeeded()) {
        if (err) *err = "etcd: revision mismatch";
        return false;
    }
    if (out_rev && resp.has_header()) {
        *out_rev = static_cast<Revision>(resp.header().revision());
    }
    return true;
}

LeaseId GrpcEtcdClient::LeaseGrant(uint32_t ttl_seconds, std::string* err) {
    etcdserverpb::LeaseGrantRequest  req;
    etcdserverpb::LeaseGrantResponse resp;
    req.set_ttl(static_cast<int64_t>(ttl_seconds));
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + impl_->opts.dial_timeout);
    auto s = impl_->lease_stub->LeaseGrant(&ctx, req, &resp);
    if (!s.ok()) {
        if (err) *err = std::string("etcd LeaseGrant: ") + s.error_message();
        return kNoLease;
    }
    if (!resp.error().empty()) {
        if (err) *err = "etcd LeaseGrant: " + resp.error();
        return kNoLease;
    }
    return static_cast<LeaseId>(resp.id());
}

bool GrpcEtcdClient::LeaseKeepAlive(LeaseId id, std::string* err) {
    // Single-shot ping via LeaseTimeToLive — matches the convention
    // HttpEtcdClient uses (the bidi keepalive stream is Phase F-3).
    etcdserverpb::LeaseTimeToLiveRequest  req;
    etcdserverpb::LeaseTimeToLiveResponse resp;
    req.set_id(static_cast<int64_t>(id));
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + impl_->opts.dial_timeout);
    auto s = impl_->lease_stub->LeaseTimeToLive(&ctx, req, &resp);
    if (!s.ok()) {
        if (err) *err = std::string("etcd LeaseTimeToLive: ") + s.error_message();
        return false;
    }
    if (resp.ttl() <= 0) {
        if (err) *err = "etcd: lease expired or not found";
        return false;
    }
    return true;
}

bool GrpcEtcdClient::LeaseRevoke(LeaseId id, std::string* err) {
    etcdserverpb::LeaseRevokeRequest  req;
    etcdserverpb::LeaseRevokeResponse resp;
    req.set_id(static_cast<int64_t>(id));
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + impl_->opts.dial_timeout);
    auto s = impl_->lease_stub->LeaseRevoke(&ctx, req, &resp);
    if (!s.ok()) {
        if (err) *err = std::string("etcd LeaseRevoke: ") + s.error_message();
        return false;
    }
    return true;
}

uint32_t GrpcEtcdClient::LeaseTTLRemaining(LeaseId id) const {
    etcdserverpb::LeaseTimeToLiveRequest  req;
    etcdserverpb::LeaseTimeToLiveResponse resp;
    req.set_id(static_cast<int64_t>(id));
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + impl_->opts.dial_timeout);
    auto s = impl_->lease_stub->LeaseTimeToLive(&ctx, req, &resp);
    if (!s.ok() || resp.ttl() < 0) return 0;
    return static_cast<uint32_t>(resp.ttl());
}

WatchHandle GrpcEtcdClient::WatchPrefix(const std::string& prefix,
                                          WatchCallback cb) {
    std::lock_guard<std::mutex> lk(impl_->w_mu);
    const WatchHandle h = impl_->next_watch.fetch_add(1, std::memory_order_relaxed);

    Impl::PollWatcher pw;
    pw.prefix = prefix;
    pw.cb     = std::move(cb);
    // Seed with current state so the first poll only fires diffs.
    std::string err;
    for (const auto& kv : GetPrefix(prefix, &err)) {
        pw.last_state[kv.key] = kv;
    }
    impl_->watchers[h] = std::move(pw);

    if (!impl_->poller.joinable()) {
        impl_->poller = std::thread([this] {
            while (!impl_->stop.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                if (impl_->stop.load(std::memory_order_acquire)) break;

                std::vector<std::pair<WatchHandle, std::string>> snapshot;
                {
                    std::lock_guard<std::mutex> lk2(impl_->w_mu);
                    snapshot.reserve(impl_->watchers.size());
                    for (const auto& [id, w] : impl_->watchers) {
                        snapshot.emplace_back(id, w.prefix);
                    }
                }
                for (const auto& [id, prefix] : snapshot) {
                    std::string e;
                    auto cur = GetPrefix(prefix, &e);
                    if (!e.empty() && cur.empty()) continue;
                    std::lock_guard<std::mutex> lk2(impl_->w_mu);
                    auto it = impl_->watchers.find(id);
                    if (it == impl_->watchers.end()) continue;
                    auto& w = it->second;
                    std::unordered_map<std::string, KeyValue> new_state;
                    new_state.reserve(cur.size());
                    for (const auto& kv : cur) new_state[kv.key] = kv;
                    for (const auto& [k, kv] : new_state) {
                        auto pit = w.last_state.find(k);
                        if (pit == w.last_state.end() ||
                            pit->second.mod_revision != kv.mod_revision) {
                            WatchEvent ev{WatchEventType::kPut, kv,
                                           pit == w.last_state.end()
                                               ? KeyValue{} : pit->second};
                            w.cb(ev);
                        }
                    }
                    for (const auto& [k, kv] : w.last_state) {
                        if (new_state.find(k) == new_state.end()) {
                            WatchEvent ev{WatchEventType::kDelete, kv, kv};
                            w.cb(ev);
                        }
                    }
                    w.last_state = std::move(new_state);
                }
            }
        });
    }
    return h;
}

void GrpcEtcdClient::Unwatch(WatchHandle h) {
    std::lock_guard<std::mutex> lk(impl_->w_mu);
    impl_->watchers.erase(h);
}

#else  // !KVCACHE_HAVE_GRPC — facade returns errors so the tree compiles.

std::unique_ptr<GrpcEtcdClient>
GrpcEtcdClient::Create(const Options&, std::string* err) {
    if (err) *err = "GrpcEtcdClient: built without grpc++. "
                     "Reconfigure with grpc available "
                     "(brew install grpc / apt-get install libgrpc++-dev) "
                     "to enable.";
    return nullptr;
}
GrpcEtcdClient::~GrpcEtcdClient() = default;
bool GrpcEtcdClient::Put(const std::string&, const std::string&, LeaseId,
                          Revision*, std::string* err) {
    if (err) *err = "GrpcEtcdClient: not built"; return false;
}
std::optional<KeyValue> GrpcEtcdClient::Get(const std::string&, std::string*) { return std::nullopt; }
std::vector<KeyValue>   GrpcEtcdClient::GetPrefix(const std::string&, std::string*) { return {}; }
bool GrpcEtcdClient::Delete(const std::string&, std::string*) { return false; }
bool GrpcEtcdClient::PutIfRevision(const std::string&, const std::string&,
                                    Revision, LeaseId, Revision*, std::string*) { return false; }
LeaseId  GrpcEtcdClient::LeaseGrant(uint32_t, std::string*) { return kNoLease; }
bool     GrpcEtcdClient::LeaseKeepAlive(LeaseId, std::string*) { return false; }
bool     GrpcEtcdClient::LeaseRevoke   (LeaseId, std::string*) { return false; }
uint32_t GrpcEtcdClient::LeaseTTLRemaining(LeaseId) const { return 0; }
WatchHandle GrpcEtcdClient::WatchPrefix(const std::string&, WatchCallback) { return 0; }
void        GrpcEtcdClient::Unwatch(WatchHandle) {}

#endif  // KVCACHE_HAVE_GRPC

}  // namespace kvcache::node::cluster
