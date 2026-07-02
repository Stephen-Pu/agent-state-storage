// LLD §3.3 T4 — Cold tier native REST / S3-compatible UFS client (Phase B3).
//
// `FilesystemColdTier` (cold_tier.h) covers the FUSE-mounted-UFS default,
// where the multi-cloud problem is outsourced to a POSIX mount. B3 adds the
// direct path: a `RestColdTier` that speaks HTTP to an object store / REST
// UFS gateway without a kernel mount in the way. Useful when:
//   * no FUSE driver is available (locked-down hosts, distroless containers)
//   * the operator wants the data plane to hold the UFS connection itself
//     (latency, credential scoping, per-tenant prefixes)
//
// Object mapping (mirrors FilesystemColdTier's on-disk layout so the two
// backends can be swapped without re-keying):
//   {base_url}/{key_prefix}{first_2_hex(key)}/{rest_of_hex(key)}.kv
//
//   Put    -> HTTP PUT    (body = bytes)
//   Get    -> HTTP GET    (200 -> bytes; 404 -> not-found, empty *err)
//   Delete -> HTTP DELETE (200/202/204 -> ok; 404 -> already-gone, ok)
//   Exists -> HTTP HEAD   (200 -> true)
//
// Transport is behind an injectable `IHttpTransport` seam: production uses the
// libcurl-backed `CurlHttpTransport`; unit tests inject an in-memory fake so
// the full IColdTier contract is exercised deterministically with no network.
//
// Auth: optional `Authorization: Bearer <token>` header + optional TLS
// material (CA / client cert+key), mirroring HttpEtcdClient::Options. This
// covers generic REST UFS gateways (MinIO/S3 path-style with a pre-shared
// token, Alluxio's S3 REST endpoint, etc.). For talking directly to AWS S3,
// full SigV4 request signing is provided as a transport decorator
// (`sigv4_transport.h`, Phase B5) — wrap the transport passed to
// `CreateWithTransport`; RestColdTier itself stays unaware of signing.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "tier/cold_tier.h"  // IColdTier, DramKey

namespace kvcache::node::tier {

// Result of one HTTP exchange. `transport_err` non-empty means the request
// never produced an HTTP response (DNS / connect / TLS failure); in that case
// `status` is 0 and `body` is empty. Otherwise `status` is the HTTP code.
struct HttpResult {
    long        status = 0;
    std::string body;
    std::string transport_err;
};

// Injectable HTTP transport. `method` is one of "PUT" | "GET" | "DELETE" |
// "HEAD". `body`/`n` are used only for PUT (ignored otherwise). Headers are
// raw "Key: Value" strings.
class IHttpTransport {
   public:
    virtual ~IHttpTransport() = default;
    virtual HttpResult Request(const std::string&              method,
                               const std::string&              url,
                               const std::vector<std::string>& headers,
                               const uint8_t*                  body,
                               std::size_t                     n) = 0;
};

// libcurl-backed transport (production default). Thread-safe: each Request
// uses its own easy handle.
std::shared_ptr<IHttpTransport> MakeCurlHttpTransport();

class RestColdTier final : public IColdTier {
   public:
    struct Options {
        std::string base_url;              // required, e.g. "https://s3.local/bucket"
        std::string key_prefix = "kvcache/";  // object-key namespace under the bucket
        std::string bearer_token;          // optional -> Authorization: Bearer
        std::string ca_pem_path;           // optional TLS server-verify CA
        std::string client_cert_pem_path;  // optional mTLS client cert
        std::string client_key_pem_path;   // optional mTLS client key
        long        timeout_ms = 30000;
    };

    // Production factory — uses the libcurl transport.
    static std::unique_ptr<RestColdTier> Create(const Options& opts,
                                                 std::string*   err);

    // Test / advanced factory — inject a transport (fake or decorated).
    static std::unique_ptr<RestColdTier> CreateWithTransport(
        const Options& opts, std::shared_ptr<IHttpTransport> http,
        std::string* err);

    ~RestColdTier() override = default;

    std::string Name() const override { return "native-rest"; }

    bool Put   (const DramKey&, const uint8_t* data, std::size_t n, std::string* err) override;
    bool Get   (const DramKey&, std::vector<uint8_t>* out, std::string* err) override;
    bool Delete(const DramKey&, std::string* err) override;
    bool Exists(const DramKey&) const override;

    // Object key under the bucket: "{key_prefix}{first2}/{tail}.kv".
    std::string ObjectKeyFor(const DramKey& key) const;
    // Full request URL: "{base_url}/{ObjectKeyFor(key)}".
    std::string UrlFor(const DramKey& key) const;

   private:
    RestColdTier() = default;
    std::vector<std::string> Headers() const;  // auth header(s), shared per request

    Options                         opts_;
    std::shared_ptr<IHttpTransport> http_;
};

// TLS/timeout-aware overload: builds a CurlHttpTransport and applies the
// timeout_ms, ca_pem_path, client_cert_pem_path, and client_key_pem_path
// fields from opts before returning. Used by RestColdTier::Create and by
// cold_tier.cpp's guarded branch so both paths carry the same TLS knobs.
std::shared_ptr<IHttpTransport> MakeCurlHttpTransport(const RestColdTier::Options& opts);

}  // namespace kvcache::node::tier
