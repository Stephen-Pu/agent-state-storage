// Phase Q-1 — etcd-backed kvstore-node self-registration.
//
// Each node, on startup, grants an etcd lease (TTL ~10s), PUTs its
// identity at `/kvcache/nodes/<node-id>` with the lease attached, then
// runs a background keepalive every ~5s. When the lease expires (TTL
// times two missed renewals) etcd drops the key — peers stop routing
// to us. On graceful shutdown we revoke the lease explicitly so peers
// learn faster.
//
// Value payload (JSON-ish, host-endian-agnostic):
//
//   {"node_id":"<id>","host":"<advertise-host>","grpc_port":<port>}
//
// We deliberately keep this string-ish and tiny — peers parse it with
// a hand-rolled extractor (no JSON library link). The `joined_at`
// timestamp is omitted because Watch already carries the etcd
// mod_revision which gives a total order across all node events.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "cluster/etcd_client.h"

namespace kvcache::node::cluster {

class NodeRegistrar {
   public:
    struct Options {
        std::string node_id;            // unique per pod; typically POD_NAME
        std::string advertise_host;     // pod IP or DNS the others should dial
        uint16_t    grpc_port    = 0;   // advertised gRPC port (NodeData)
        uint32_t    lease_ttl_seconds = 10;        // etcd lease TTL
        // Keepalive cadence. Should be < lease_ttl_seconds / 2 so a single
        // missed beat doesn't expire the lease.
        std::chrono::milliseconds keepalive_period{3000};
        // Key prefix; the full key is `<prefix><node_id>`.
        std::string key_prefix = "/kvcache/nodes/";
    };

    // The registrar holds a non-owning pointer to the etcd client; the
    // client must outlive the registrar.
    NodeRegistrar(IEtcdClient* etcd, const Options& opts);
    ~NodeRegistrar();

    NodeRegistrar(const NodeRegistrar&)            = delete;
    NodeRegistrar& operator=(const NodeRegistrar&) = delete;

    // Grants a lease, PUTs the registration key, and starts the
    // keepalive thread. Returns true on success; sets `err` otherwise.
    // Calling Start() twice (without an intervening Stop) returns true
    // but is a no-op.
    bool Start(std::string* err);

    // Best-effort lease revoke + thread join. Idempotent; safe to call
    // from a SIGTERM handler.
    void Stop();

    bool Running() const noexcept { return running_.load(); }

    // Lease id and key — exposed for tests / introspection.
    LeaseId            Lease() const noexcept { return lease_; }
    const std::string& Key()   const noexcept { return key_; }

   private:
    void KeepaliveLoop();

    IEtcdClient*                    etcd_;
    Options                         opts_;
    std::string                     key_;
    std::string                     value_;
    LeaseId                         lease_      = kNoLease;
    std::atomic<bool>               running_    {false};
    std::atomic<bool>               stop_       {false};

    std::mutex                      cv_mu_;
    std::condition_variable         cv_;
    std::thread                     keepalive_thread_;
};

// Helper exposed for the registrar AND the directory — keeps the
// node-payload encoding in one place. Returns the encoded value.
std::string EncodeNodeValue(const std::string& node_id,
                             const std::string& host,
                             uint16_t            grpc_port);

// Pulls (host, grpc_port) out of a NodeRegistrar-shaped value. Returns
// false if the input is malformed. On success `out_host` and
// `out_port` are filled.
bool DecodeNodeValue(const std::string& value,
                      std::string*       out_host,
                      uint16_t*          out_port);

}  // namespace kvcache::node::cluster
