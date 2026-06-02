// LLD §4.2 / §6.1.5 — kvagent request router implementation.
#include "router/router.h"

#include <cstring>

namespace kvcache::agent::router {

namespace {

// Little-endian fixed-width put/get helpers. We don't use memcpy of
// the whole struct because the wire layout must be stable across
// architectures (the engine and agent may differ on padding/endian).
void PutU32(std::vector<uint8_t>& b, uint32_t v) {
    b.push_back(static_cast<uint8_t>(v & 0xff));
    b.push_back(static_cast<uint8_t>((v >> 8) & 0xff));
    b.push_back(static_cast<uint8_t>((v >> 16) & 0xff));
    b.push_back(static_cast<uint8_t>((v >> 24) & 0xff));
}
void PutU64(std::vector<uint8_t>& b, uint64_t v) {
    for (int i = 0; i < 8; ++i) b.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xff));
}
uint32_t GetU32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
uint64_t GetU64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(p[i]) << (8 * i);
    return v;
}

}  // namespace

// ----- Request (de)serialize ------------------------------------------

std::vector<uint8_t> Request::Serialize() const {
    std::vector<uint8_t> b;
    b.reserve(kWireSize);
    PutU32(b, static_cast<uint32_t>(op));
    PutU64(b, request_id);
    b.insert(b.end(), tenant_id.begin(), tenant_id.end());
    b.insert(b.end(), prefix_hash.begin(), prefix_hash.end());
    return b;
}

std::optional<Request> Request::Parse(std::span<const uint8_t> bytes) {
    if (bytes.size() < kWireSize) return std::nullopt;
    const uint8_t* p = bytes.data();
    Request r;
    r.op         = static_cast<OpCode>(GetU32(p)); p += 4;
    r.request_id = GetU64(p);                      p += 8;
    std::memcpy(r.tenant_id.data(),   p, 16);      p += 16;
    std::memcpy(r.prefix_hash.data(), p, 16);
    return r;
}

// ----- Response (de)serialize -----------------------------------------

std::vector<uint8_t> Response::Serialize() const {
    std::vector<uint8_t> b;
    b.reserve(kWireSize);
    PutU64(b, request_id);
    PutU32(b, static_cast<uint32_t>(status));
    PutU32(b, static_cast<uint32_t>(source));
    PutU32(b, matched_tokens);
    // node_id: length-prefixed, zero-padded to kMaxNodeId so the wire
    // size is fixed. Truncate defensively (resolver should never hand
    // us a >40B id, but the wire format is the contract).
    const uint32_t n = static_cast<uint32_t>(
        node_id.size() > kMaxNodeId ? kMaxNodeId : node_id.size());
    PutU32(b, n);
    b.insert(b.end(), node_id.begin(), node_id.begin() + n);
    b.resize(kWireSize, 0);  // zero-pad the node_id tail
    return b;
}

std::optional<Response> Response::Parse(std::span<const uint8_t> bytes) {
    if (bytes.size() < kWireSize) return std::nullopt;
    const uint8_t* p = bytes.data();
    Response r;
    r.request_id     = GetU64(p);                       p += 8;
    r.status         = static_cast<Status>(GetU32(p));  p += 4;
    r.source         = static_cast<Source>(GetU32(p));  p += 4;
    r.matched_tokens = GetU32(p);                        p += 4;
    uint32_t n       = GetU32(p);                        p += 4;
    if (n > kMaxNodeId) return std::nullopt;  // corrupt
    r.node_id.assign(reinterpret_cast<const char*>(p), n);
    return r;
}

// ----- routing ---------------------------------------------------------

routing_cache::Key RequestRouter::MakeKey(
    const std::array<uint8_t, 16>& tenant_id,
    const std::array<uint8_t, 16>& prefix_hash) {
    routing_cache::Key k;
    k.tenant_id.assign(reinterpret_cast<const char*>(tenant_id.data()), tenant_id.size());
    k.prefix.assign(reinterpret_cast<const char*>(prefix_hash.data()), prefix_hash.size());
    return k;
}

Response RequestRouter::Handle(const Request& req) {
    Response resp;
    resp.request_id = req.request_id;

    switch (req.op) {
        case OpCode::kLookup:
        case OpCode::kFetch:
            // Reads. The agent resolves WHERE the prefix lives and
            // returns the node; the engine then server-pulls (NIXL)
            // from it (Fetch) or just records the hit (Lookup). A
            // bloom-negative is a true miss for both — nothing to pull.
            return ResolveRead(req);
        case OpCode::kPublish:
            // Write. The prefix may not exist yet, so the bloom gate
            // doesn't apply — resolve WHERE the new chunk should be
            // sealed and return that node; the engine issues the seal
            // RPC there.
            return ResolveWrite(req);
        default:
            // Corrupt / unknown opcode.
            resp.status = Status::kError;
            resp.source = Source::kNone;
            return resp;
    }
}

Response RequestRouter::ResolveRead(const Request& req) {
    Response resp;
    resp.request_id = req.request_id;

    // 1. Bloom fast-negative. tenant_id as the bloom tenant key; the
    //    prefix_hash bytes as the membership key.
    const std::string tenant(reinterpret_cast<const char*>(req.tenant_id.data()),
                             req.tenant_id.size());
    if (!bloom_.MaybeContains(
            tenant, {req.prefix_hash.data(), req.prefix_hash.size()})) {
        resp.status = Status::kMiss;
        resp.source = Source::kBloomNegative;
        return resp;
    }

    const auto key = MakeKey(req.tenant_id, req.prefix_hash);

    // 2. Routing-cache hit.
    if (auto node = cache_.Get(key)) {
        resp.status = Status::kOk;
        resp.source = Source::kCache;
        resp.node_id = *node;
        // matched_tokens isn't cached — a cached hit means "this prefix
        // is primary on node X"; the engine re-confirms the length via
        // the subsequent server-pull. 0 = "ask the node".
        return resp;
    }

    // 3. Slow path (HRW over the cluster node set).
    if (auto rr = resolver_(req.tenant_id, req.prefix_hash)) {
        cache_.Put(key, rr->node_id);
        resp.status = Status::kOk;
        resp.source = Source::kResolver;
        resp.node_id = rr->node_id;
        resp.matched_tokens = rr->matched_tokens;
        return resp;
    }

    resp.status = Status::kMiss;
    resp.source = Source::kNone;
    return resp;
}

Response RequestRouter::ResolveWrite(const Request& req) {
    Response resp;
    resp.request_id = req.request_id;

    const auto key = MakeKey(req.tenant_id, req.prefix_hash);

    // A Publish doesn't consult the bloom (the chunk is new) but it can
    // still reuse a cached primary so repeat publishes of the same
    // prefix land on the same node (locality).
    if (auto node = cache_.Get(key)) {
        resp.status = Status::kOk;
        resp.source = Source::kCache;
        resp.node_id = *node;
        return resp;
    }
    if (auto rr = resolver_(req.tenant_id, req.prefix_hash)) {
        cache_.Put(key, rr->node_id);
        resp.status = Status::kOk;
        resp.source = Source::kResolver;
        resp.node_id = rr->node_id;
        return resp;
    }
    // No cluster view yet → nowhere to seal.
    resp.status = Status::kMiss;
    resp.source = Source::kNone;
    return resp;
}

std::vector<uint8_t> RequestRouter::HandleRaw(std::span<const uint8_t> sq_bytes) {
    auto req = Request::Parse(sq_bytes);
    if (!req) {
        Response err;
        err.request_id = 0;
        err.status = Status::kError;
        return err.Serialize();
    }
    return Handle(*req).Serialize();
}

}  // namespace kvcache::agent::router
