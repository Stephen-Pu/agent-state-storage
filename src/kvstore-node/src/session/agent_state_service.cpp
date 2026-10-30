// AgentStateServiceImpl — see agent_state_service.h.
#include "session/agent_state_service.h"

#include <algorithm>
#include <functional>
#include <vector>

#include "security/mtls.h"

namespace kvcache::node::session {

namespace {

using ::grpc::Status;
using ::grpc::StatusCode;

void PutU32BE(std::string* out, uint32_t v) {
    for (int i = 3; i >= 0; --i) out->push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}

void PutU64BE(std::string* out, uint64_t v) {
    for (int i = 7; i >= 0; --i) out->push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}

uint64_t GetU64BE(std::string_view s) {
    uint64_t v = 0;
    for (char c : s) v = (v << 8) | static_cast<uint8_t>(c);
    return v;
}

// The client certificate as the mTLS layer sees it; same extraction as the
// NodeData service so both resolve the same tenant for the same cert.
security::CertInfo ClientCertInfo(::grpc::ServerContext* ctx) {
    security::CertInfo info;
    auto ac = ctx->auth_context();
    if (!ac) return info;
    if (auto cn = ac->FindPropertyValues("x509_common_name"); !cn.empty()) {
        info.cn.assign(cn.front().data(), cn.front().size());
    }
    for (const auto& san : ac->FindPropertyValues("x509_subject_alternative_name")) {
        std::string v(san.data(), san.size());
        if (v.rfind("spiffe://", 0) == 0) {
            if (!info.spiffe_id) info.spiffe_id = v;
            info.uri_sans.push_back(std::move(v));
        }
    }
    return info;
}

Status Invalid(const std::string& msg) { return {StatusCode::INVALID_ARGUMENT, msg}; }

Status Internal(const std::string& what, const std::string& err) {
    return {StatusCode::INTERNAL, what + ": " + err};
}

}  // namespace

AgentStateServiceImpl::AgentStateServiceImpl(OrderedLogStore* store, BlobStore* blobs)
    : store_(store), blobs_(blobs) {}

void AgentStateServiceImpl::EnableCertTenant(const security::MtlsRegistry* registry) {
    registry_ = registry;
    cert_tenant_.store(true, std::memory_order_release);
}

std::string AgentStateServiceImpl::EncodeKey(const std::string& tenant, char kind,
                                             std::string_view rest) {
    std::string k;
    k.reserve(4 + tenant.size() + 1 + rest.size());
    PutU32BE(&k, static_cast<uint32_t>(tenant.size()));
    k.append(tenant);
    k.push_back(kind);
    k.append(rest);
    return k;
}

Status AgentStateServiceImpl::ResolveTenant(Ctx* ctx, const std::string& requested,
                                            std::string* tenant) const {
    if (cert_tenant_.load(std::memory_order_acquire)) {
        auto from_cert = security::MtlsRegistry::ResolveTenant(ClientCertInfo(ctx), registry_);
        if (!from_cert || from_cert->empty()) {
            return {StatusCode::UNAUTHENTICATED, "client certificate carries no tenant identity"};
        }
        if (!requested.empty() && requested != *from_cert) {
            return {StatusCode::PERMISSION_DENIED,
                    "request tenant '" + requested + "' does not match the client certificate"};
        }
        *tenant = *from_cert;
        return Status::OK;
    }
    if (requested.empty()) return Invalid("tenant is required");
    *tenant = requested;
    return Status::OK;
}

std::mutex& AgentStateServiceImpl::StripeFor(const std::string& tenant,
                                             const std::string& session) {
    const std::size_t h = std::hash<std::string>{}(tenant) * 31 + std::hash<std::string>{}(session);
    return stripes_[h % kStripes];
}

std::string AgentStateServiceImpl::ItemPrefix(const std::string& tenant,
                                              const std::string& session) const {
    std::string rest;
    PutU32BE(&rest, static_cast<uint32_t>(session.size()));
    rest.append(session);
    return EncodeKey(tenant, 'I', rest);
}

// ---- Items ----------------------------------------------------------------

Status AgentStateServiceImpl::AppendItems(Ctx* ctx, const asv1::AppendItemsRequest* req,
                                          asv1::AppendItemsResponse* resp) {
    std::string tenant;
    if (auto st = ResolveTenant(ctx, req->tenant(), &tenant); !st.ok()) return st;
    if (req->session().empty()) return Invalid("session is required");
    for (const auto& item : req->items()) {
        if (item.size() > kMaxValueBytes) return Invalid("item exceeds 4 MiB");
    }
    const std::string prefix = ItemPrefix(tenant, req->session());
    std::lock_guard<std::mutex> lk(StripeFor(tenant, req->session()));
    ScanOptions last;
    last.prefix  = prefix;
    last.limit   = 1;
    last.reverse = true;
    bool more    = false;
    const auto tail = store_->Scan(last, &more);
    uint64_t seq =
        tail.empty() ? 1 : GetU64BE(std::string_view(tail[0].key).substr(prefix.size())) + 1;
    std::vector<Mutation> batch;
    batch.reserve(req->items_size());
    for (const auto& item : req->items()) {
        std::string key = prefix;
        PutU64BE(&key, seq++);
        batch.push_back({Mutation::kPut, std::move(key), item});
    }
    std::string err;
    if (!store_->Apply(batch, &err)) return Internal("append", err);
    resp->set_next_seq(seq);
    return Status::OK;
}

Status AgentStateServiceImpl::GetItems(Ctx* ctx, const asv1::GetItemsRequest* req,
                                       asv1::GetItemsResponse* resp) {
    std::string tenant;
    if (auto st = ResolveTenant(ctx, req->tenant(), &tenant); !st.ok()) return st;
    if (req->session().empty()) return Invalid("session is required");
    ScanOptions o;
    o.prefix  = ItemPrefix(tenant, req->session());
    o.limit   = req->limit();
    o.reverse = req->limit() != 0;
    bool more = false;
    auto kvs  = store_->Scan(o, &more);
    if (o.reverse) std::reverse(kvs.begin(), kvs.end());
    for (auto& kv : kvs) resp->add_items(std::move(kv.value));
    return Status::OK;
}

Status AgentStateServiceImpl::PopItem(Ctx* ctx, const asv1::PopItemRequest* req,
                                      asv1::PopItemResponse* resp) {
    std::string tenant;
    if (auto st = ResolveTenant(ctx, req->tenant(), &tenant); !st.ok()) return st;
    if (req->session().empty()) return Invalid("session is required");
    std::lock_guard<std::mutex> lk(StripeFor(tenant, req->session()));
    ScanOptions o;
    o.prefix  = ItemPrefix(tenant, req->session());
    o.limit   = 1;
    o.reverse = true;
    bool more = false;
    auto kvs  = store_->Scan(o, &more);
    if (kvs.empty()) {
        resp->set_found(false);
        return Status::OK;
    }
    std::string err;
    if (!store_->Apply({{Mutation::kDel, kvs[0].key, {}}}, &err)) return Internal("pop", err);
    resp->set_found(true);
    resp->set_item(std::move(kvs[0].value));
    return Status::OK;
}

Status AgentStateServiceImpl::ClearSession(Ctx* ctx, const asv1::ClearSessionRequest* req,
                                           asv1::ClearSessionResponse* resp) {
    std::string tenant;
    if (auto st = ResolveTenant(ctx, req->tenant(), &tenant); !st.ok()) return st;
    if (req->session().empty()) return Invalid("session is required");
    std::lock_guard<std::mutex> lk(StripeFor(tenant, req->session()));
    std::string err;
    const std::size_t n = store_->DeletePrefix(ItemPrefix(tenant, req->session()), &err);
    if (!err.empty()) return Internal("clear", err);
    resp->set_removed(n);
    return Status::OK;
}

Status AgentStateServiceImpl::ForkSession(Ctx* ctx, const asv1::ForkSessionRequest* req,
                                          asv1::ForkSessionResponse* resp) {
    std::string tenant;
    if (auto st = ResolveTenant(ctx, req->tenant(), &tenant); !st.ok()) return st;
    if (req->source().empty() || req->target().empty()) {
        return Invalid("source and target are required");
    }
    if (req->source() == req->target()) return Invalid("source and target must differ");
    // Lock both sessions in address order so two opposite forks cannot deadlock.
    std::mutex* a = &StripeFor(tenant, req->source());
    std::mutex* b = &StripeFor(tenant, req->target());
    if (b < a) std::swap(a, b);
    std::unique_lock<std::mutex> la(*a);
    std::unique_lock<std::mutex> lb;
    if (b != a) lb = std::unique_lock<std::mutex>(*b);

    const std::string src = ItemPrefix(tenant, req->source());
    const std::string dst = ItemPrefix(tenant, req->target());
    bool more = false;
    ScanOptions probe;
    probe.prefix = dst;
    probe.limit  = 1;
    if (!store_->Scan(probe, &more).empty()) {
        return {StatusCode::FAILED_PRECONDITION, "target session is not empty"};
    }
    ScanOptions all;
    all.prefix = src;
    std::vector<Mutation> batch;
    for (auto& kv : store_->Scan(all, &more)) {
        batch.push_back({Mutation::kPut, dst + kv.key.substr(src.size()), std::move(kv.value)});
    }
    std::string err;
    if (!store_->Apply(batch, &err)) return Internal("fork", err);
    resp->set_copied(batch.size());
    return Status::OK;
}

// ---- Records --------------------------------------------------------------

Status AgentStateServiceImpl::WriteRecords(Ctx* ctx, const asv1::WriteRecordsRequest* req,
                                           asv1::WriteRecordsResponse*) {
    std::string tenant;
    if (auto st = ResolveTenant(ctx, req->tenant(), &tenant); !st.ok()) return st;
    std::vector<Mutation> batch;
    batch.reserve(req->puts_size() + req->deletes_size());
    for (const auto& r : req->puts()) {
        if (r.key().empty()) return Invalid("record key is required");
        if (r.value().size() > kMaxValueBytes) return Invalid("record value exceeds 4 MiB");
        batch.push_back({Mutation::kPut, EncodeKey(tenant, 'R', r.key()), r.value()});
    }
    for (const auto& k : req->deletes()) {
        batch.push_back({Mutation::kDel, EncodeKey(tenant, 'R', k), {}});
    }
    std::string err;
    if (!store_->Apply(batch, &err)) return Internal("write", err);
    return Status::OK;
}

Status AgentStateServiceImpl::GetRecords(Ctx* ctx, const asv1::GetRecordsRequest* req,
                                         asv1::GetRecordsResponse* resp) {
    std::string tenant;
    if (auto st = ResolveTenant(ctx, req->tenant(), &tenant); !st.ok()) return st;
    for (const auto& k : req->keys()) {
        if (auto v = store_->Get(EncodeKey(tenant, 'R', k))) {
            auto* r = resp->add_found();
            r->set_key(k);
            r->set_value(std::move(*v));
        }
    }
    return Status::OK;
}

Status AgentStateServiceImpl::ScanRecords(Ctx* ctx, const asv1::ScanRecordsRequest* req,
                                          asv1::ScanRecordsResponse* resp) {
    std::string tenant;
    if (auto st = ResolveTenant(ctx, req->tenant(), &tenant); !st.ok()) return st;
    const std::string base = EncodeKey(tenant, 'R', "");
    ScanOptions o;
    o.prefix = base + req->prefix();
    if (!req->start_after().empty()) o.start_after = base + req->start_after();
    if (!req->end_before().empty()) o.end_before = base + req->end_before();
    o.limit   = req->limit() == 0 || req->limit() > kMaxScanPage ? kMaxScanPage : req->limit();
    o.reverse = req->reverse();
    bool more = false;
    for (auto& kv : store_->Scan(o, &more)) {
        auto* r = resp->add_records();
        r->set_key(kv.key.substr(base.size()));
        r->set_value(std::move(kv.value));
    }
    resp->set_has_more(more);
    return Status::OK;
}

Status AgentStateServiceImpl::DeleteRecordPrefix(Ctx* ctx,
                                                 const asv1::DeleteRecordPrefixRequest* req,
                                                 asv1::DeleteRecordPrefixResponse* resp) {
    std::string tenant;
    if (auto st = ResolveTenant(ctx, req->tenant(), &tenant); !st.ok()) return st;
    std::string err;
    const std::size_t n = store_->DeletePrefix(EncodeKey(tenant, 'R', req->prefix()), &err);
    if (!err.empty()) return Internal("delete", err);
    resp->set_removed(n);
    return Status::OK;
}

// ---- Blobs ----------------------------------------------------------------

Status AgentStateServiceImpl::PutBlob(Ctx* ctx, ::grpc::ServerReader<asv1::PutBlobChunk>* reader,
                                      asv1::PutBlobResponse* resp) {
    asv1::PutBlobChunk chunk;
    if (!reader->Read(&chunk)) return Invalid("empty blob stream");
    std::string tenant;
    if (auto st = ResolveTenant(ctx, chunk.tenant(), &tenant); !st.ok()) return st;
    if (chunk.name().empty()) return Invalid("blob name is required");
    std::string err;
    auto w = blobs_->Create(tenant, chunk.name(), &err);
    if (!w) return Internal("create blob", err);
    uint64_t size = 0;
    do {
        if (!w->Write(chunk.data().data(), chunk.data().size(), &err)) {
            const bool too_big = size + chunk.data().size() > BlobStore::kMaxBlobBytes;
            return {too_big ? StatusCode::INVALID_ARGUMENT : StatusCode::INTERNAL, err};
        }
        size += chunk.data().size();
    } while (reader->Read(&chunk));
    if (ctx->IsCancelled()) return {StatusCode::CANCELLED, "client cancelled the upload"};
    if (!w->Commit(&err)) return Internal("commit blob", err);
    resp->set_size(size);
    return Status::OK;
}

Status AgentStateServiceImpl::GetBlob(Ctx* ctx, const asv1::GetBlobRequest* req,
                                      ::grpc::ServerWriter<asv1::BlobChunk>* writer) {
    std::string tenant;
    if (auto st = ResolveTenant(ctx, req->tenant(), &tenant); !st.ok()) return st;
    if (req->name().empty()) return Invalid("blob name is required");
    std::string err;
    asv1::BlobChunk chunk;
    const bool ok = blobs_->Read(
        tenant, req->name(),
        [&](const char* p, std::size_t n) {
            chunk.set_data(p, n);
            return writer->Write(chunk);
        },
        &err);
    if (!ok) {
        if (err.empty()) return {StatusCode::NOT_FOUND, "no such blob"};
        return Internal("read blob", err);
    }
    return Status::OK;
}

Status AgentStateServiceImpl::StatBlob(Ctx* ctx, const asv1::StatBlobRequest* req,
                                       asv1::StatBlobResponse* resp) {
    std::string tenant;
    if (auto st = ResolveTenant(ctx, req->tenant(), &tenant); !st.ok()) return st;
    if (auto size = blobs_->Size(tenant, req->name())) {
        resp->set_exists(true);
        resp->set_size(*size);
    }
    return Status::OK;
}

Status AgentStateServiceImpl::DeleteBlob(Ctx* ctx, const asv1::DeleteBlobRequest* req,
                                         asv1::DeleteBlobResponse* resp) {
    std::string tenant;
    if (auto st = ResolveTenant(ctx, req->tenant(), &tenant); !st.ok()) return st;
    resp->set_existed(blobs_->Delete(tenant, req->name()));
    return Status::OK;
}

}  // namespace kvcache::node::session
