// LLD §3.3 T4 — AWS Signature Version 4 signing decorator (Phase B5).
//
// RestColdTier (B3) speaks plain HTTP behind an injectable `IHttpTransport`
// seam and supports only a static `Authorization: Bearer <token>` header —
// enough for MinIO / Alluxio S3-REST gateways with a pre-shared token, but not
// for talking directly to AWS S3, which requires per-request SigV4 signing.
//
// B5 adds that as a *transport decorator* (the extension point the B3 header
// promised) — no change to RestColdTier itself. `SigV4Transport` wraps any
// inner `IHttpTransport`; on each Request it computes the SigV4 Authorization
// header from the (method, url, body) and injects it (plus the required
// `x-amz-date` / `x-amz-content-sha256`, and `x-amz-security-token` for
// temporary credentials) before delegating to the inner transport.
//
// Compose it around the libcurl transport:
//   auto curl = MakeCurlHttpTransport();
//   auto signed = MakeSigV4Transport(curl, {access, secret, "us-east-1"}, &err);
//   auto tier = RestColdTier::CreateWithTransport(opts, signed, &err);
//
// The signing math (canonical request → string-to-sign → signing key →
// signature) is exposed as a pure `ComputeSigV4Authorization` so it can be
// verified against AWS's published SigV4 test-suite vectors independently of
// any network or the decorator.
//
// Compiled only under KVCACHE_HAVE_OPENSSL (needs SHA-256 + HMAC-SHA-256);
// without it the factory returns an error and the rest of the tier still
// builds — same gating as EncryptingColdTier.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "tier/rest_cold_tier.h"  // IHttpTransport

namespace kvcache::node::tier {

struct SigV4Options {
    std::string access_key;            // required (AWS access key id)
    std::string secret_key;            // required (AWS secret access key)
    std::string region;                // required, e.g. "us-east-1"
    std::string service = "s3";        // signing service name
    std::string session_token;         // optional — adds x-amz-security-token

    // Injectable clock for deterministic tests. Returns the ISO8601 basic UTC
    // timestamp "YYYYMMDDTHHMMSSZ". If null, real UTC now is used.
    std::function<std::string()> amz_date_fn;
};

// Wrap `inner` so every Request is SigV4-signed. Returns nullptr + *err if
// OpenSSL isn't compiled in, `inner` is null, or a required credential field
// is empty.
std::shared_ptr<IHttpTransport> MakeSigV4Transport(
    std::shared_ptr<IHttpTransport> inner, const SigV4Options& opts,
    std::string* err);

// Pure SigV4 signer — computes the full `Authorization` header value.
//
//   signed_headers : (lowercased-name, value) pairs, ALREADY SORTED by name.
//                    These exact headers form CanonicalHeaders + SignedHeaders.
//   payload_hash_hex : hex(SHA-256(body)) — "UNSIGNED-PAYLOAD" is also legal.
//   amz_date       : "YYYYMMDDTHHMMSSZ"; the date-stamp is its first 8 chars.
//
// Returns the "AWS4-HMAC-SHA256 Credential=…, SignedHeaders=…, Signature=…"
// string, or empty on a crypto failure (only possible if OpenSSL is absent).
std::string ComputeSigV4Authorization(
    const std::string& method,
    const std::string& canonical_uri,
    const std::string& canonical_query,
    const std::vector<std::pair<std::string, std::string>>& signed_headers,
    const std::string& payload_hash_hex,
    const std::string& access_key,
    const std::string& secret_key,
    const std::string& region,
    const std::string& service,
    const std::string& amz_date);

// hex(SHA-256(data[0..n])). Exposed for tests + the decorator's payload hash.
// Empty string on a crypto failure / OpenSSL absent.
std::string Sha256Hex(const uint8_t* data, std::size_t n);

}  // namespace kvcache::node::tier
