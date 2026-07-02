// Task 4 — A9 DR warm-standby: GrpcReplicaSource implementation.
//
// Marshals kv_locator_t ↔ proto::Locator using the same field layout as
// node_data_service.cpp's file-scoped ToProtoLocator / FromProtoLocator,
// replicated minimally here so the client TU has no internal dependency on the
// server's anonymous namespace.
#ifdef KVCACHE_HAVE_GRPC

#include "grpc/grpc_replica_source.h"

#include <grpcpp/grpcpp.h>

#include <array>
#include <cstring>

#include "kvcache/kv_errors.h"
#include "kvcache/kv_types.h"
#include "node.grpc.pb.h"
#include "node.pb.h"
#include "prefix/art_index.h"  // node::prefix::ChunkHash

namespace kvcache::node::grpc_client {

namespace {

// Minimal kv_locator_t → proto::Locator conversion.
// Mirrors node_data_service.cpp's file-scoped ToProtoLocator exactly:
//   tenant_id  — 16 raw bytes
//   model_id_hash — fixed64
//   prefix_hash — 16 raw bytes
//   range fields — layer/head/token start+count
//   version / flags
kvcache::proto::Locator ProtoFromLocator(const kv_locator_t& loc) {
    kvcache::proto::Locator out;
    out.set_tenant_id(
        std::string(reinterpret_cast<const char*>(loc.tenant_id),
                    sizeof(loc.tenant_id)));
    out.set_model_id_hash(loc.model_id_hash);
    out.set_prefix_hash(
        std::string(reinterpret_cast<const char*>(loc.prefix_hash),
                    sizeof(loc.prefix_hash)));
    auto* r = out.mutable_range();
    r->set_layer_start(loc.range.layer_start);
    r->set_layer_count(loc.range.layer_count);
    r->set_head_start(loc.range.head_start);
    r->set_head_count(loc.range.head_count);
    r->set_token_start(loc.range.token_start);
    r->set_token_count(loc.range.token_count);
    out.set_version(loc.version);
    out.set_flags(loc.flags);
    return out;
}

}  // namespace

GrpcReplicaSource::GrpcReplicaSource(
    std::shared_ptr<kvcache::proto::NodeData::Stub> stub)
    : stub_(std::move(stub)) {}

int GrpcReplicaSource::Fetch(const kv_locator_t& locator,
                              abi::HeadlessNode::ReplicaChunk* out) {
    kvcache::proto::ReplicaFetchRequest req;
    *req.mutable_locator() = ProtoFromLocator(locator);

    kvcache::proto::ReplicaFetchResponse resp;
    ::grpc::ClientContext ctx;
    ::grpc::Status st = stub_->ReplicaFetch(&ctx, req, &resp);

    if (!st.ok()) {
        // Transport or auth failure → treat as a benign miss.
        // The consumer's retry / skip logic handles KV_E_NOT_FOUND the
        // same way it handles a primary eviction — the chunk simply isn't
        // available from this source right now.
        return KV_E_NOT_FOUND;
    }

    if (resp.status() != KV_OK) {
        return resp.status();
    }

    // Unmarshal chunk_path: each entry is an 8-byte opaque hash.
    // ChunkHash = node::prefix::ChunkHash = std::array<uint8_t,8>.
    out->chunk_path.clear();
    for (const auto& h : resp.chunk_path()) {
        // Defensively guard against a malformed response carrying a hash
        // that isn't exactly 8 bytes — skip entries that don't fit rather
        // than memcpy past the array boundary.
        node::prefix::ChunkHash ch{};
        static_assert(sizeof(ch) == 8, "ChunkHash must be 8 bytes");
        if (h.size() == sizeof(ch)) {
            std::memcpy(ch.data(), h.data(), sizeof(ch));
        }
        out->chunk_path.push_back(ch);
    }

    // Unmarshal payload bytes.
    out->bytes.assign(resp.data().begin(), resp.data().end());

    return KV_OK;
}

}  // namespace kvcache::node::grpc_client

#endif  // KVCACHE_HAVE_GRPC
