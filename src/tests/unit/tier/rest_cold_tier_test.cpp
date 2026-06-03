// Phase B3 — RestColdTier tests.
//
// Two categories (mirrors http_etcd_client_test):
//   1. Always-on tests driven by an in-memory FakeObjectStore transport —
//      exercise the full IColdTier contract (Put/Get/Delete/Exists, overwrite,
//      missing, idempotent delete, auth header, key layout) with no network.
//   2. Opt-in integration test gated on KVCACHE_REST_UFS_ENDPOINT.
#include "tier/rest_cold_tier.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <map>
#include <mutex>
#include <string>

using kvcache::node::tier::ColdTierOptions;
using kvcache::node::tier::CreateColdTier;
using kvcache::node::tier::DramKey;
using kvcache::node::tier::HttpResult;
using kvcache::node::tier::IHttpTransport;
using kvcache::node::tier::RestColdTier;

namespace {

DramKey Key(uint8_t b) {
    DramKey k{};
    k.bytes[0] = b;
    k.bytes[15] = b;
    return k;
}

// Faithful in-memory object store: PUT/GET/DELETE/HEAD over a url->bytes map,
// returning the same HTTP status codes a real REST UFS would. Records the last
// request's headers so tests can assert the Authorization header is sent.
class FakeObjectStore final : public IHttpTransport {
   public:
    HttpResult Request(const std::string& method, const std::string& url,
                       const std::vector<std::string>& headers,
                       const uint8_t* body, std::size_t n) override {
        std::lock_guard<std::mutex> lk(mu_);
        ++calls;
        last_method = method;
        last_url = url;
        last_headers = headers;

        HttpResult r;
        if (fail_transport) {
            r.transport_err = "injected transport failure";
            return r;
        }
        if (method == "PUT") {
            store_[url] = std::string(reinterpret_cast<const char*>(body), n);
            r.status = 200;
        } else if (method == "GET") {
            auto it = store_.find(url);
            if (it == store_.end()) {
                r.status = 404;
            } else {
                r.status = 200;
                r.body = it->second;
            }
        } else if (method == "HEAD") {
            r.status = store_.count(url) ? 200 : 404;
        } else if (method == "DELETE") {
            r.status = store_.erase(url) ? 204 : 404;
        } else {
            r.status = 405;
        }
        return r;
    }

    bool HasAuthHeader() const {
        for (const auto& h : last_headers)
            if (h.rfind("Authorization:", 0) == 0) return true;
        return false;
    }

    bool             fail_transport = false;
    int              calls = 0;
    std::string      last_method, last_url;
    std::vector<std::string> last_headers;

   private:
    mutable std::mutex                 mu_;
    std::map<std::string, std::string> store_;
};

std::unique_ptr<RestColdTier> MakeTier(std::shared_ptr<FakeObjectStore> fake,
                                       std::string token = "") {
    RestColdTier::Options o;
    o.base_url = "https://s3.test/bucket";
    o.bearer_token = std::move(token);
    std::string err;
    auto t = RestColdTier::CreateWithTransport(o, std::move(fake), &err);
    EXPECT_NE(t, nullptr) << err;
    return t;
}

}  // namespace

TEST(RestColdTierTest, CreateRequiresBaseUrl) {
    RestColdTier::Options o;  // base_url empty
    std::string err;
    EXPECT_EQ(RestColdTier::CreateWithTransport(
                  o, std::make_shared<FakeObjectStore>(), &err),
              nullptr);
    EXPECT_FALSE(err.empty());
}

TEST(RestColdTierTest, CreateRejectsNullTransport) {
    RestColdTier::Options o;
    o.base_url = "https://s3.test/bucket";
    std::string err;
    EXPECT_EQ(RestColdTier::CreateWithTransport(o, nullptr, &err), nullptr);
    EXPECT_FALSE(err.empty());
}

TEST(RestColdTierTest, ObjectKeyLayoutMatchesShardScheme) {
    auto t = MakeTier(std::make_shared<FakeObjectStore>());
    // Key(0xAB): first byte 0xab → shard "ab", URL ends with .kv under prefix.
    const std::string key = t->ObjectKeyFor(Key(0xAB));
    EXPECT_EQ(key.rfind("kvcache/ab/", 0), 0u);
    EXPECT_NE(key.find(".kv"), std::string::npos);
    EXPECT_EQ(t->UrlFor(Key(0xAB)), "https://s3.test/bucket/" + key);
}

TEST(RestColdTierTest, BaseUrlTrailingSlashTrimmed) {
    RestColdTier::Options o;
    o.base_url = "https://s3.test/bucket/";  // trailing slash
    std::string err;
    auto t = RestColdTier::CreateWithTransport(
        o, std::make_shared<FakeObjectStore>(), &err);
    ASSERT_NE(t, nullptr) << err;
    // No doubled slash between bucket and the object key.
    EXPECT_EQ(t->UrlFor(Key(1)).find("bucket//"), std::string::npos);
}

TEST(RestColdTierTest, PutGetExistsDelete) {
    auto fake = std::make_shared<FakeObjectStore>();
    auto t = MakeTier(fake);
    std::string err;

    EXPECT_FALSE(t->Exists(Key(1)));
    std::vector<uint8_t> data{1, 2, 3, 4, 5};
    ASSERT_TRUE(t->Put(Key(1), data.data(), data.size(), &err)) << err;
    EXPECT_TRUE(t->Exists(Key(1)));

    std::vector<uint8_t> out;
    ASSERT_TRUE(t->Get(Key(1), &out, &err)) << err;
    EXPECT_EQ(out, data);

    EXPECT_TRUE(t->Delete(Key(1), &err)) << err;
    EXPECT_FALSE(t->Exists(Key(1)));
}

TEST(RestColdTierTest, GetMissingReturnsFalseWithEmptyErr) {
    auto t = MakeTier(std::make_shared<FakeObjectStore>());
    std::vector<uint8_t> out{0xFF};
    std::string err = "preexisting";
    EXPECT_FALSE(t->Get(Key(7), &out, &err));
    EXPECT_TRUE(err.empty());  // 404 is not an error
}

TEST(RestColdTierTest, OverwriteReplacesBytes) {
    auto t = MakeTier(std::make_shared<FakeObjectStore>());
    std::string err;
    std::vector<uint8_t> a(8, 0x11), b(4, 0x22);
    ASSERT_TRUE(t->Put(Key(3), a.data(), a.size(), &err)) << err;
    ASSERT_TRUE(t->Put(Key(3), b.data(), b.size(), &err)) << err;
    std::vector<uint8_t> out;
    ASSERT_TRUE(t->Get(Key(3), &out, &err)) << err;
    EXPECT_EQ(out, b);
}

TEST(RestColdTierTest, DeleteMissingIsIdempotentSuccess) {
    auto t = MakeTier(std::make_shared<FakeObjectStore>());
    std::string err;
    EXPECT_TRUE(t->Delete(Key(9), &err)) << err;  // 404 → ok
}

TEST(RestColdTierTest, BearerTokenSentWhenConfigured) {
    auto fake = std::make_shared<FakeObjectStore>();
    auto t = MakeTier(fake, "sekret-token");
    std::string err;
    std::vector<uint8_t> data{0xAA};
    ASSERT_TRUE(t->Put(Key(1), data.data(), data.size(), &err)) << err;
    EXPECT_TRUE(fake->HasAuthHeader());
}

TEST(RestColdTierTest, NoAuthHeaderWhenTokenEmpty) {
    auto fake = std::make_shared<FakeObjectStore>();
    auto t = MakeTier(fake);  // no token
    std::string err;
    EXPECT_FALSE(t->Exists(Key(1)));
    EXPECT_FALSE(fake->HasAuthHeader());
}

TEST(RestColdTierTest, TransportErrorSurfacesOnPutAndGet) {
    auto fake = std::make_shared<FakeObjectStore>();
    fake->fail_transport = true;
    auto t = MakeTier(fake);
    std::string err;
    std::vector<uint8_t> data{1};
    EXPECT_FALSE(t->Put(Key(1), data.data(), data.size(), &err));
    EXPECT_FALSE(err.empty());
    std::vector<uint8_t> out;
    err.clear();
    EXPECT_FALSE(t->Get(Key(1), &out, &err));
    EXPECT_FALSE(err.empty());
    EXPECT_FALSE(t->Exists(Key(1)));  // transport error → not present
}

// ---- factory wiring -------------------------------------------------------

TEST(ColdTierFactoryTest, NativeRestRequiresBaseUrl) {
    ColdTierOptions o;
    o.type = "native-rest";  // base_url empty → error
    std::string err;
    EXPECT_EQ(CreateColdTier(o, &err), nullptr);
    EXPECT_FALSE(err.empty());
}

TEST(ColdTierFactoryTest, NativeRestBuildsWithBaseUrl) {
    ColdTierOptions o;
    o.type = "native-rest";
    o.rest.base_url = "https://s3.test/bucket";
    std::string err;
    auto t = CreateColdTier(o, &err);
    ASSERT_NE(t, nullptr) << err;
    EXPECT_EQ(t->Name(), "native-rest");
}

// ---- opt-in integration ---------------------------------------------------

TEST(RestColdTierIntegration, RoundTripAgainstRealEndpoint) {
    const char* base = std::getenv("KVCACHE_REST_UFS_ENDPOINT");
    if (!base || !*base) {
        GTEST_SKIP() << "set KVCACHE_REST_UFS_ENDPOINT to run";
    }
    RestColdTier::Options o;
    o.base_url = base;
    if (const char* tok = std::getenv("KVCACHE_REST_UFS_TOKEN")) o.bearer_token = tok;
    std::string err;
    auto t = RestColdTier::Create(o, &err);
    ASSERT_NE(t, nullptr) << err;

    std::vector<uint8_t> data(256, 0x5A);
    ASSERT_TRUE(t->Put(Key(42), data.data(), data.size(), &err)) << err;
    EXPECT_TRUE(t->Exists(Key(42)));
    std::vector<uint8_t> out;
    ASSERT_TRUE(t->Get(Key(42), &out, &err)) << err;
    EXPECT_EQ(out, data);
    EXPECT_TRUE(t->Delete(Key(42), &err)) << err;
    EXPECT_FALSE(t->Exists(Key(42)));
}
