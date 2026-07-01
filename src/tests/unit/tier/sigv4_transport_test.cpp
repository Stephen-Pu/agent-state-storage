// Phase B5 — SigV4 signing decorator tests.
//
// Two layers:
//   1. Pure signer verified against AWS's published SigV4 test-suite
//      "get-vanilla" vector — the canonical GET with the well-known
//      AKIDEXAMPLE credential; the expected Authorization string is a fixed
//      value from AWS's reference, so a byte-exact match proves the whole
//      canonical-request -> string-to-sign -> signing-key -> signature
//      pipeline is correct.
//   2. Decorator round-trip through a recording fake IHttpTransport: assert it
//      injects x-amz-date / x-amz-content-sha256 / Authorization and that the
//      Authorization re-derives from ComputeSigV4Authorization (consistency),
//      for both an empty-body GET and a body-carrying PUT.
//
// All crypto is OpenSSL-gated; without it MakeSigV4Transport must return an
// error (asserted at the end).
#include "tier/sigv4_transport.h"

#include <gtest/gtest.h>

#include <mutex>
#include <string>
#include <vector>

using kvcache::node::tier::ComputeSigV4Authorization;
using kvcache::node::tier::HttpResult;
using kvcache::node::tier::IHttpTransport;
using kvcache::node::tier::MakeSigV4Transport;
using kvcache::node::tier::Sha256Hex;
using kvcache::node::tier::SigV4Options;

namespace {

// AWS SigV4 test-suite credential (public reference values).
constexpr char kAccess[] = "AKIDEXAMPLE";
constexpr char kSecret[] = "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY";

// Records the last request so tests can inspect the injected signing headers.
class RecordingTransport final : public IHttpTransport {
   public:
    HttpResult Request(const std::string& method, const std::string& url,
                       const std::vector<std::string>& headers,
                       const uint8_t* body, std::size_t n) override {
        std::lock_guard<std::mutex> lk(mu_);
        last_method = method;
        last_url = url;
        last_headers = headers;
        last_body.assign(body, body + n);
        HttpResult r;
        r.status = 200;
        return r;
    }

    std::string HeaderValue(const std::string& name) const {
        std::lock_guard<std::mutex> lk(mu_);
        const std::string pfx = name + ": ";
        for (const auto& h : last_headers) {
            if (h.rfind(pfx, 0) == 0) return h.substr(pfx.size());
        }
        return "";
    }

    mutable std::mutex       mu_;
    std::string              last_method;
    std::string              last_url;
    std::vector<std::string> last_headers;
    std::vector<uint8_t>     last_body;
};

}  // namespace

#if KVCACHE_HAVE_OPENSSL

// AWS "get-vanilla" reference vector — the canonical Authorization value is
// fixed by AWS, so this pins the entire signing pipeline.
TEST(SigV4, MatchesAwsGetVanillaVector) {
    const std::string empty_hash = Sha256Hex(nullptr, 0);
    EXPECT_EQ(empty_hash,
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    const std::vector<std::pair<std::string, std::string>> signed_headers = {
        {"host", "example.amazonaws.com"},
        {"x-amz-date", "20150830T123600Z"},
    };
    const std::string auth = ComputeSigV4Authorization(
        "GET", "/", "", signed_headers, empty_hash, kAccess, kSecret,
        "us-east-1", "service", "20150830T123600Z");

    EXPECT_EQ(auth,
              "AWS4-HMAC-SHA256 "
              "Credential=AKIDEXAMPLE/20150830/us-east-1/service/aws4_request, "
              "SignedHeaders=host;x-amz-date, "
              "Signature=5fa00fa31553b73ebf1942676e86291e8372ff2a2260956d9b8aae1d"
              "763fbf31");
}

TEST(SigV4, DecoratorSignsEmptyGetConsistently) {
    auto rec = std::make_shared<RecordingTransport>();
    SigV4Options opts;
    opts.access_key = kAccess;
    opts.secret_key = kSecret;
    opts.region = "us-east-1";
    opts.service = "service";
    opts.amz_date_fn = [] { return std::string("20150830T123600Z"); };

    std::string err;
    auto signer = MakeSigV4Transport(rec, opts, &err);
    ASSERT_NE(signer, nullptr) << err;

    const HttpResult r = signer->Request("GET", "https://example.amazonaws.com/",
                                         {}, nullptr, 0);
    EXPECT_EQ(r.status, 200);

    const std::string empty_hash = Sha256Hex(nullptr, 0);
    EXPECT_EQ(rec->HeaderValue("x-amz-date"), "20150830T123600Z");
    EXPECT_EQ(rec->HeaderValue("x-amz-content-sha256"), empty_hash);

    // The decorator signs host;x-amz-content-sha256;x-amz-date (S3 style);
    // re-derive with the same inputs and assert byte-equality.
    const std::vector<std::pair<std::string, std::string>> sh = {
        {"host", "example.amazonaws.com"},
        {"x-amz-content-sha256", empty_hash},
        {"x-amz-date", "20150830T123600Z"},
    };
    const std::string expect = ComputeSigV4Authorization(
        "GET", "/", "", sh, empty_hash, kAccess, kSecret, "us-east-1",
        "service", "20150830T123600Z");
    EXPECT_EQ(rec->HeaderValue("Authorization"), expect);
    // Sanity: SignedHeaders lists all three, alphabetically.
    EXPECT_NE(expect.find("SignedHeaders=host;x-amz-content-sha256;x-amz-date"),
              std::string::npos);
}

TEST(SigV4, DecoratorHashesPutBodyAndSigns) {
    auto rec = std::make_shared<RecordingTransport>();
    SigV4Options opts;
    opts.access_key = kAccess;
    opts.secret_key = kSecret;
    opts.region = "us-west-2";
    opts.service = "s3";
    opts.amz_date_fn = [] { return std::string("20200101T000000Z"); };

    std::string err;
    auto signer = MakeSigV4Transport(rec, opts, &err);
    ASSERT_NE(signer, nullptr) << err;

    const std::string payload = "hello kv cache";
    const auto* body = reinterpret_cast<const uint8_t*>(payload.data());
    signer->Request("PUT", "https://bucket.s3.us-west-2.amazonaws.com/kvcache/ab/cd.kv",
                    {"Content-Type: application/octet-stream"}, body,
                    payload.size());

    const std::string body_hash = Sha256Hex(body, payload.size());
    EXPECT_EQ(rec->HeaderValue("x-amz-content-sha256"), body_hash);
    EXPECT_EQ(rec->HeaderValue("x-amz-date"), "20200101T000000Z");
    // The caller's own header is preserved alongside the injected ones.
    EXPECT_EQ(rec->HeaderValue("Content-Type"), "application/octet-stream");
    EXPECT_NE(rec->HeaderValue("Authorization").find(
                  "Credential=AKIDEXAMPLE/20200101/us-west-2/s3/aws4_request"),
              std::string::npos);
}

TEST(SigV4, SessionTokenIsSignedAndSent) {
    auto rec = std::make_shared<RecordingTransport>();
    SigV4Options opts;
    opts.access_key = kAccess;
    opts.secret_key = kSecret;
    opts.region = "us-east-1";
    opts.service = "s3";
    opts.session_token = "FQoGZXIvYXdzEBc-token";
    opts.amz_date_fn = [] { return std::string("20200101T000000Z"); };

    std::string err;
    auto signer = MakeSigV4Transport(rec, opts, &err);
    ASSERT_NE(signer, nullptr) << err;
    signer->Request("GET", "https://bucket.s3.amazonaws.com/k.kv", {}, nullptr, 0);

    EXPECT_EQ(rec->HeaderValue("x-amz-security-token"), "FQoGZXIvYXdzEBc-token");
    EXPECT_NE(rec->HeaderValue("Authorization").find("x-amz-security-token"),
              std::string::npos)
        << "session token must be a signed header";
}

TEST(SigV4, RejectsMissingCredentials) {
    auto rec = std::make_shared<RecordingTransport>();
    std::string err;
    SigV4Options opts;  // all empty
    EXPECT_EQ(MakeSigV4Transport(rec, opts, &err), nullptr);
    EXPECT_FALSE(err.empty());

    opts.access_key = kAccess;
    opts.secret_key = kSecret;
    opts.region = "us-east-1";
    EXPECT_EQ(MakeSigV4Transport(nullptr, opts, &err), nullptr) << "null inner";
}

#else  // !KVCACHE_HAVE_OPENSSL

TEST(SigV4, FactoryReportsOpenSslAbsent) {
    auto rec = std::make_shared<RecordingTransport>();
    SigV4Options opts;
    opts.access_key = kAccess;
    opts.secret_key = kSecret;
    opts.region = "us-east-1";
    std::string err;
    EXPECT_EQ(MakeSigV4Transport(rec, opts, &err), nullptr);
    EXPECT_FALSE(err.empty());
}

#endif  // KVCACHE_HAVE_OPENSSL
