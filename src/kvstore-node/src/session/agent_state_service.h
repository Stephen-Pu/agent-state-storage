// M1 — agentstate.v1.AgentState gRPC service: items, records and blobs.
//
// All state lives in one OrderedLogStore key space plus a BlobStore. Every
// key starts with the tenant (EncodeKey), so tenants never share a key and a
// client never sees another tenant's prefix:
//   item    EncodeKey(tenant, 'I', u32be len | session | u64be seq)
//   record  EncodeKey(tenant, 'R', user key)
//
// Tenant: by default the request's `tenant` field (required). After
// EnableCertTenant() the tenant is resolved from the client certificate
// (MtlsRegistry::ResolveTenant); a request naming a different tenant gets
// PERMISSION_DENIED, and an empty request tenant means "the cert's tenant".
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>

#include <grpcpp/grpcpp.h>

#include "agentstate.grpc.pb.h"
#include "session/blob_store.h"
#include "session/ordered_log_store.h"

namespace kvcache::node::security {
class MtlsRegistry;
}

namespace kvcache::node::session {

namespace asv1 = ::agentstate::v1;

class AgentStateServiceImpl final : public asv1::AgentState::Service {
   public:
    static constexpr std::size_t kMaxValueBytes = 4u << 20;
    static constexpr uint32_t    kMaxScanPage   = 1000;

    // Both stores are borrowed and must outlive the service.
    AgentStateServiceImpl(OrderedLogStore* store, BlobStore* blobs);

    // Resolve the tenant from the client certificate from now on. Call only
    // when the server requires client certificates. `registry` may be null.
    void EnableCertTenant(const security::MtlsRegistry* registry);

    static std::string EncodeKey(const std::string& tenant, char kind, std::string_view rest);

    using Ctx    = ::grpc::ServerContext;
    using Status = ::grpc::Status;

    Status AppendItems(Ctx*, const asv1::AppendItemsRequest*, asv1::AppendItemsResponse*) override;
    Status GetItems(Ctx*, const asv1::GetItemsRequest*, asv1::GetItemsResponse*) override;
    Status PopItem(Ctx*, const asv1::PopItemRequest*, asv1::PopItemResponse*) override;
    Status ClearSession(Ctx*, const asv1::ClearSessionRequest*, asv1::ClearSessionResponse*) override;
    Status ForkSession(Ctx*, const asv1::ForkSessionRequest*, asv1::ForkSessionResponse*) override;

    Status WriteRecords(Ctx*, const asv1::WriteRecordsRequest*, asv1::WriteRecordsResponse*) override;
    Status GetRecords(Ctx*, const asv1::GetRecordsRequest*, asv1::GetRecordsResponse*) override;
    Status ScanRecords(Ctx*, const asv1::ScanRecordsRequest*, asv1::ScanRecordsResponse*) override;
    Status DeleteRecordPrefix(Ctx*, const asv1::DeleteRecordPrefixRequest*,
                              asv1::DeleteRecordPrefixResponse*) override;

    Status PutBlob(Ctx*, ::grpc::ServerReader<asv1::PutBlobChunk>*, asv1::PutBlobResponse*) override;
    Status GetBlob(Ctx*, const asv1::GetBlobRequest*, ::grpc::ServerWriter<asv1::BlobChunk>*) override;
    Status StatBlob(Ctx*, const asv1::StatBlobRequest*, asv1::StatBlobResponse*) override;
    Status DeleteBlob(Ctx*, const asv1::DeleteBlobRequest*, asv1::DeleteBlobResponse*) override;

   private:
    static constexpr std::size_t kStripes = 64;

    Status ResolveTenant(Ctx* ctx, const std::string& requested, std::string* tenant) const;
    std::mutex& StripeFor(const std::string& tenant, const std::string& session);
    std::string ItemPrefix(const std::string& tenant, const std::string& session) const;

    OrderedLogStore*                    store_;
    BlobStore*                          blobs_;
    std::atomic<bool>                   cert_tenant_{false};
    const security::MtlsRegistry*       registry_ = nullptr;
    std::array<std::mutex, kStripes>    stripes_;
};

}  // namespace kvcache::node::session
