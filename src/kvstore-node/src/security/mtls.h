// LLD §5.2 — mTLS termination + CN → Tenant lookup (Scheme C).
//
// At connection time the gRPC server peer's TLS certificate is parsed and its
// CN string extracted. The CN may map to:
//   * a Tenant identity (service-to-service traffic on behalf of a tenant)
//   * an internal-component identity (cert-manager-issued, rotating every 30d)
//   * an admin identity (a small set of operator certs)
//
// Scheme C: we do NOT force the PKI to encode the tenant_id inside the cert.
// Instead the runtime keeps a CN → Tenant lookup table that the CP refreshes.
#pragma once

#include <array>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace kvcache::node::security {

enum class IdentityKind : uint8_t {
    kUnknown  = 0,
    kTenant   = 1,
    kInternal = 2,
    kAdmin    = 3,
};

const char* IdentityKindName(IdentityKind k);

struct Identity {
    IdentityKind            kind = IdentityKind::kUnknown;
    std::string             cn;
    std::array<uint8_t, 16> tenant_id{};
    std::string             display_name;
};

class MtlsRegistry {
   public:
    MtlsRegistry()  = default;
    ~MtlsRegistry() = default;

    bool UpsertMapping(const std::string& cn, const Identity& id);
    bool RemoveMapping(const std::string& cn);
    std::optional<Identity> Resolve(const std::string& cn) const;
    std::size_t Size() const noexcept;

    // Minimal helper for tests; production uses OpenSSL parsing.
    static std::optional<std::string> ExtractCnFromPem(const std::string& pem);

   private:
    mutable std::mutex                          mu_;
    std::unordered_map<std::string, Identity>   by_cn_;
};

}  // namespace kvcache::node::security
