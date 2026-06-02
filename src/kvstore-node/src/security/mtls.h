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
#include <vector>

namespace kvcache::node::security {

// Parsed identity material lifted out of a peer's X.509 leaf cert.
// CN is the historical Scheme-C lookup key; SANs (esp. the URI SAN
// carrying a SPIFFE ID) are the modern, SAN-first identity surface
// the CN-table maps over. Phase B8 makes this a real OpenSSL parse;
// the legacy string-scan stays as a fallback when the binary is
// built without OpenSSL.
struct CertInfo {
    std::string                cn;        // subject commonName ("" if absent)
    std::vector<std::string>   dns_sans;  // DNS: entries from subjectAltName
    std::vector<std::string>   uri_sans;  // URI: entries from subjectAltName
    // First uri_san that begins with "spiffe://", if any. This is the
    // identity a SPIFFE-aware deployment authenticates on.
    std::optional<std::string> spiffe_id;
};

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

    // Phase B8 — full leaf-cert parse. When built with OpenSSL
    // (KVCACHE_HAVE_OPENSSL), this does a real PEM → X509 decode and
    // pulls the subject CN + every DNS/URI subjectAltName + the first
    // SPIFFE URI. Without OpenSSL it degrades to a CN-only string scan
    // (dns_sans/uri_sans empty, spiffe_id nullopt) so non-OpenSSL
    // builds still compile + the CN path still works. Returns nullopt
    // when the PEM can't be parsed at all (OpenSSL build) or contains
    // no "CN=" marker (fallback build).
    static std::optional<CertInfo> ParsePem(const std::string& pem);

    // True iff this binary was built with real OpenSSL X.509 parsing.
    // Tests gate SAN/SPIFFE assertions on this; the CN-only path is
    // always exercised.
    static bool HasRealParser() noexcept;

    // Convenience wrapper preserved for existing callers — returns
    // just the CN. Delegates to ParsePem.
    static std::optional<std::string> ExtractCnFromPem(const std::string& pem);

   private:
    mutable std::mutex                          mu_;
    std::unordered_map<std::string, Identity>   by_cn_;
};

}  // namespace kvcache::node::security
