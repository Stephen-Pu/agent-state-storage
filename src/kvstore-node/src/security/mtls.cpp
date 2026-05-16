// LLD §5.2 — MtlsRegistry implementation.
#include "security/mtls.h"

#include <cstddef>

namespace kvcache::node::security {

const char* IdentityKindName(IdentityKind k) {
    switch (k) {
        case IdentityKind::kUnknown:  return "unknown";
        case IdentityKind::kTenant:   return "tenant";
        case IdentityKind::kInternal: return "internal";
        case IdentityKind::kAdmin:    return "admin";
    }
    return "?";
}

bool MtlsRegistry::UpsertMapping(const std::string& cn, const Identity& id) {
    std::lock_guard lk(mu_);
    auto [it, inserted] = by_cn_.emplace(cn, id);
    if (!inserted) it->second = id;
    return inserted;
}

bool MtlsRegistry::RemoveMapping(const std::string& cn) {
    std::lock_guard lk(mu_);
    return by_cn_.erase(cn) > 0;
}

std::optional<Identity> MtlsRegistry::Resolve(const std::string& cn) const {
    std::lock_guard lk(mu_);
    auto it = by_cn_.find(cn);
    if (it == by_cn_.end()) return std::nullopt;
    return it->second;
}

std::size_t MtlsRegistry::Size() const noexcept {
    std::lock_guard lk(mu_);
    return by_cn_.size();
}

// Minimal X.509 CN extractor for tests. TODO(stephen): swap to
// X509_NAME_get_text_by_NID once OpenSSL is linked in.
std::optional<std::string> MtlsRegistry::ExtractCnFromPem(const std::string& pem) {
    static constexpr const char* kMarker = "CN=";
    const auto pos = pem.find(kMarker);
    if (pos == std::string::npos) return std::nullopt;
    const std::size_t start = pos + 3;
    const std::size_t end = pem.find_first_of(",/\n\r", start);
    if (end == std::string::npos) return pem.substr(start);
    if (end == start) return std::nullopt;
    return pem.substr(start, end - start);
}

}  // namespace kvcache::node::security
