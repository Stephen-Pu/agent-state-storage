// Phase Q-1 — NodeRegistrar implementation.
#include "cluster/node_registrar.h"

#include <cstdio>
#include <cstring>

namespace kvcache::node::cluster {

// ---- value codec ---------------------------------------------------------

std::string EncodeNodeValue(const std::string& node_id,
                             const std::string& host,
                             uint16_t            grpc_port) {
    // {"node_id":"<id>","host":"<host>","grpc_port":<port>}
    // No JSON escaping — node_id / host are operator-controlled and we
    // reject characters that would corrupt the parser at admission
    // time. Keep this string-ish so peers can decode without linking a
    // JSON library.
    char port_buf[16];
    std::snprintf(port_buf, sizeof(port_buf), "%u",
                   static_cast<unsigned>(grpc_port));
    std::string out;
    out.reserve(64 + node_id.size() + host.size());
    out += "{\"node_id\":\"";
    out += node_id;
    out += "\",\"host\":\"";
    out += host;
    out += "\",\"grpc_port\":";
    out += port_buf;
    out += "}";
    return out;
}

bool DecodeNodeValue(const std::string& value,
                      std::string*       out_host,
                      uint16_t*          out_port) {
    auto find_quoted = [&](const std::string& field) -> std::string {
        const std::string needle = "\"" + field + "\":\"";
        const auto p = value.find(needle);
        if (p == std::string::npos) return {};
        const auto start = p + needle.size();
        const auto end   = value.find('"', start);
        if (end == std::string::npos) return {};
        return value.substr(start, end - start);
    };
    auto find_int = [&](const std::string& field) -> long {
        const std::string needle = "\"" + field + "\":";
        const auto p = value.find(needle);
        if (p == std::string::npos) return -1;
        return std::strtol(value.c_str() + p + needle.size(), nullptr, 10);
    };

    const std::string host = find_quoted("host");
    const long        port = find_int("grpc_port");
    if (host.empty() || port <= 0 || port > 65535) return false;
    if (out_host) *out_host = host;
    if (out_port) *out_port = static_cast<uint16_t>(port);
    return true;
}

// ---- registrar lifecycle -------------------------------------------------

NodeRegistrar::NodeRegistrar(IEtcdClient* etcd, const Options& opts)
    : etcd_(etcd), opts_(opts) {
    key_   = opts_.key_prefix + opts_.node_id;
    value_ = EncodeNodeValue(opts_.node_id,
                              opts_.advertise_host,
                              opts_.grpc_port);
}

NodeRegistrar::~NodeRegistrar() { Stop(); }

bool NodeRegistrar::Start(std::string* err) {
    if (running_.exchange(true)) return true;
    if (!etcd_) {
        if (err) *err = "node_registrar: null etcd client";
        running_.store(false);
        return false;
    }
    if (opts_.node_id.empty() || opts_.advertise_host.empty() ||
        opts_.grpc_port == 0) {
        if (err) *err = "node_registrar: incomplete identity";
        running_.store(false);
        return false;
    }

    // Grant the lease BEFORE the first PUT so the key is born with TTL
    // already in place — a Put followed by LeaseGrant would briefly
    // expose the key as un-expiring to peers Watching the prefix.
    lease_ = etcd_->LeaseGrant(opts_.lease_ttl_seconds, err);
    if (lease_ == kNoLease) {
        running_.store(false);
        return false;
    }

    if (!etcd_->Put(key_, value_, lease_, nullptr, err)) {
        std::string ignore;
        etcd_->LeaseRevoke(lease_, &ignore);
        lease_ = kNoLease;
        running_.store(false);
        return false;
    }

    stop_.store(false);
    keepalive_thread_ = std::thread([this] { KeepaliveLoop(); });
    return true;
}

void NodeRegistrar::Stop() {
    if (!running_.exchange(false)) return;
    {
        std::lock_guard lk(cv_mu_);
        stop_.store(true);
        cv_.notify_all();
    }
    if (keepalive_thread_.joinable()) keepalive_thread_.join();

    // Best-effort lease revoke. We swallow errors — the lease will
    // expire on its own within `lease_ttl_seconds` anyway, so failing
    // to reach etcd here just delays peer awareness.
    if (lease_ != kNoLease && etcd_) {
        std::string ignore;
        etcd_->LeaseRevoke(lease_, &ignore);
        lease_ = kNoLease;
    }
}

void NodeRegistrar::KeepaliveLoop() {
    while (!stop_.load()) {
        {
            std::unique_lock lk(cv_mu_);
            if (cv_.wait_for(lk, opts_.keepalive_period,
                             [&] { return stop_.load(); })) {
                return;  // Stop() woke us up
            }
        }
        // Renew. Failure here is intentionally non-fatal: a single
        // missed beat shouldn't kill the registration. If two beats in
        // a row are missed the lease expires and the operator-level
        // health probe + StatefulSet restart take over.
        std::string err;
        (void)etcd_->LeaseKeepAlive(lease_, &err);
    }
}

}  // namespace kvcache::node::cluster
