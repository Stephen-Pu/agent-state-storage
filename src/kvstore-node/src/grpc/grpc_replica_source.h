// Task 4 — A9 DR warm-standby: GrpcReplicaSource.
//
// A ReplicaSource that pulls chunks from a remote primary over the
// ReplicaFetch RPC. Marshals kv_locator_t → ReplicaFetchRequest and
// ReplicaFetchResponse → ReplicaChunk.
//
// Constructed with a NodeData::Stub built by the caller with the right mTLS
// credentials so the peer authenticates as an internal workload (Phase A11).
//
// Transport / auth failures are mapped to KV_E_NOT_FOUND (benign miss) so
// the consumer's retry logic handles them exactly like a primary eviction.
//
// Gated on KVCACHE_HAVE_GRPC — the class does not exist in non-gRPC builds.
#pragma once

#ifdef KVCACHE_HAVE_GRPC

#include <memory>

#include "node.grpc.pb.h"   // kvcache::proto::NodeData::Stub
#include "replication/replica_source.h"   // ReplicaSource, kv_locator_t

namespace kvcache::node::grpc_client {

// ---------------------------------------------------------------------------
// GrpcReplicaSource — remote ReplicaSource over the NodeData::ReplicaFetch RPC.
//
// Fetch() marshals kv_locator_t → ReplicaFetchRequest, calls
// stub_->ReplicaFetch over the supplied gRPC channel, and unmarshals the
// response → HeadlessNode::ReplicaChunk.  Constructed with a pre-built stub
// so the caller controls channel credentials (typically mTLS with an
// internal-workload SPIFFE cert so VerifyInternalPeer on the server accepts).
// ---------------------------------------------------------------------------
class GrpcReplicaSource final : public replication::ReplicaSource {
   public:
    explicit GrpcReplicaSource(
        std::shared_ptr<kvcache::proto::NodeData::Stub> stub);

    // Fetch the chunk for `locator` from the remote primary via gRPC.
    //
    //   KV_OK          → chunk_path + bytes populated in `out`
    //   KV_E_NOT_FOUND → transport/auth failure or server-side miss
    //   other status   → server returned a non-OK application status
    int Fetch(const kv_locator_t& locator,
              abi::HeadlessNode::ReplicaChunk* out) override;

   private:
    std::shared_ptr<kvcache::proto::NodeData::Stub> stub_;
};

}  // namespace kvcache::node::grpc_client

#endif  // KVCACHE_HAVE_GRPC
