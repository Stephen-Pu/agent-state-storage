// Phase O-4 — MetricsColdTier counter tests.
//
// Drives the decorator over an in-memory FakeColdTier (with miss + error
// injection) against a fresh metrics::Registry, then asserts the kv_cold_*
// series via the Registry's Prometheus scrape text.
#include "tier/metrics_cold_tier.h"

#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

#include "metrics.h"
#include "tier/cold_tier.h"

using kvcache::metrics::Registry;
using kvcache::node::tier::DramKey;
using kvcache::node::tier::IColdTier;
using kvcache::node::tier::MetricsColdTier;

namespace {

DramKey Key(uint8_t b) {
    DramKey k{};
    k.bytes[0] = b;
    k.bytes[15] = b;
    return k;
}

// In-memory IColdTier with injectable miss / error behaviour.
class FakeColdTier final : public IColdTier {
   public:
    std::string Name() const override { return "fake"; }
    bool Put(const DramKey& k, const uint8_t* d, std::size_t n, std::string* err) override {
        if (fail_put) { if (err) *err = "injected put error"; return false; }
        store_[KeyStr(k)] = std::string(reinterpret_cast<const char*>(d), n);
        return true;
    }
    bool Get(const DramKey& k, std::vector<uint8_t>* out, std::string* err) override {
        if (fail_get) { if (err) *err = "injected get error"; return false; }
        auto it = store_.find(KeyStr(k));
        if (it == store_.end()) { if (err) err->clear(); return false; }  // clean miss
        out->assign(it->second.begin(), it->second.end());
        return true;
    }
    bool Delete(const DramKey& k, std::string* err) override {
        if (fail_delete) { if (err) *err = "injected delete error"; return false; }
        store_.erase(KeyStr(k));
        return true;
    }
    bool Exists(const DramKey& k) const override { return store_.count(KeyStr(k)) != 0; }

    bool fail_put = false, fail_get = false, fail_delete = false;

   private:
    static std::string KeyStr(const DramKey& k) {
        return std::string(reinterpret_cast<const char*>(k.bytes.data()), k.bytes.size());
    }
    std::map<std::string, std::string> store_;
};

// Pull `kv_cold_x <value>` (or `kv_cold_x{} <value>`) out of a scrape blob.
double Sample(Registry& r, const std::string& metric) {
    std::string blob;
    r.Scrape(blob);
    blob.insert(blob.begin(), '\n');
    for (const std::string p : {"\n" + metric + " ", "\n" + metric + "{} "}) {
        auto pos = blob.find(p);
        if (pos == std::string::npos) continue;
        auto start = pos + p.size();
        return std::stod(blob.substr(start, blob.find('\n', start) - start));
    }
    return -1.0;
}

std::unique_ptr<MetricsColdTier> Wrap(Registry& r, FakeColdTier** borrow) {
    auto fake = std::make_unique<FakeColdTier>();
    *borrow = fake.get();
    std::string err;
    auto t = MetricsColdTier::Create(std::move(fake), r, &err);
    EXPECT_NE(t, nullptr) << err;
    return t;
}

}  // namespace

TEST(MetricsColdTier, RejectsNullInner) {
    Registry r;
    std::string err;
    EXPECT_EQ(MetricsColdTier::Create(nullptr, r, &err), nullptr);
    EXPECT_FALSE(err.empty());
}

TEST(MetricsColdTier, SeriesSeededAtZero) {
    Registry r;
    FakeColdTier* fake = nullptr;
    auto t = Wrap(r, &fake);
    // Before any op, every series is present at 0 (not absent).
    EXPECT_EQ(Sample(r, "kv_cold_put_total"), 0.0);
    EXPECT_EQ(Sample(r, "kv_cold_get_miss_total"), 0.0);
    EXPECT_EQ(Sample(r, "kv_cold_get_errors_total"), 0.0);
    EXPECT_EQ(t->Name(), "fake+metrics");
}

TEST(MetricsColdTier, PutGetCountAndBytes) {
    Registry r;
    FakeColdTier* fake = nullptr;
    auto t = Wrap(r, &fake);
    std::string err;
    std::vector<uint8_t> data(100, 0xAB);

    ASSERT_TRUE(t->Put(Key(1), data.data(), data.size(), &err)) << err;
    ASSERT_TRUE(t->Put(Key(2), data.data(), data.size(), &err)) << err;
    EXPECT_EQ(Sample(r, "kv_cold_put_total"), 2.0);
    EXPECT_EQ(Sample(r, "kv_cold_put_bytes_total"), 200.0);

    std::vector<uint8_t> out;
    ASSERT_TRUE(t->Get(Key(1), &out, &err)) << err;
    EXPECT_EQ(Sample(r, "kv_cold_get_total"), 1.0);
    EXPECT_EQ(Sample(r, "kv_cold_get_bytes_total"), 100.0);
}

TEST(MetricsColdTier, GetMissVsErrorDistinguished) {
    Registry r;
    FakeColdTier* fake = nullptr;
    auto t = Wrap(r, &fake);
    std::string err;
    std::vector<uint8_t> out;

    // Clean miss (key absent) → miss counter, not error.
    EXPECT_FALSE(t->Get(Key(9), &out, &err));
    EXPECT_TRUE(err.empty());
    EXPECT_EQ(Sample(r, "kv_cold_get_miss_total"), 1.0);
    EXPECT_EQ(Sample(r, "kv_cold_get_errors_total"), 0.0);

    // Injected backend error → error counter, miss unchanged.
    fake->fail_get = true;
    EXPECT_FALSE(t->Get(Key(9), &out, &err));
    EXPECT_FALSE(err.empty());
    EXPECT_EQ(Sample(r, "kv_cold_get_errors_total"), 1.0);
    EXPECT_EQ(Sample(r, "kv_cold_get_miss_total"), 1.0);
}

TEST(MetricsColdTier, PutErrorCounted) {
    Registry r;
    FakeColdTier* fake = nullptr;
    auto t = Wrap(r, &fake);
    fake->fail_put = true;
    std::string err;
    std::vector<uint8_t> data{1, 2, 3};
    EXPECT_FALSE(t->Put(Key(1), data.data(), data.size(), &err));
    EXPECT_EQ(Sample(r, "kv_cold_put_total"), 1.0);       // attempt counted
    EXPECT_EQ(Sample(r, "kv_cold_put_errors_total"), 1.0);
    EXPECT_EQ(Sample(r, "kv_cold_put_bytes_total"), 0.0); // no bytes on failure
}

TEST(MetricsColdTier, DeleteAndExistsCounted) {
    Registry r;
    FakeColdTier* fake = nullptr;
    auto t = Wrap(r, &fake);
    std::string err;
    std::vector<uint8_t> data{1};
    ASSERT_TRUE(t->Put(Key(3), data.data(), data.size(), &err)) << err;

    EXPECT_TRUE(t->Exists(Key(3)));
    EXPECT_EQ(Sample(r, "kv_cold_exists_total"), 1.0);

    EXPECT_TRUE(t->Delete(Key(3), &err)) << err;
    EXPECT_EQ(Sample(r, "kv_cold_delete_total"), 1.0);
    EXPECT_EQ(Sample(r, "kv_cold_delete_errors_total"), 0.0);

    fake->fail_delete = true;
    EXPECT_FALSE(t->Delete(Key(3), &err));
    EXPECT_EQ(Sample(r, "kv_cold_delete_total"), 2.0);
    EXPECT_EQ(Sample(r, "kv_cold_delete_errors_total"), 1.0);
}

// ---- factory wiring -------------------------------------------------------

TEST(ColdTierFactoryTest, MetricsRegistryWrapsOutermost) {
    using kvcache::node::tier::ColdTierOptions;
    using kvcache::node::tier::CreateColdTier;
    Registry r;
    ColdTierOptions o;
    o.type = "fs";
    o.fs.root = std::string(::testing::TempDir()) + "/kvc_o4_metrics";
    o.metrics_registry = &r;
    std::string err;
    auto t = CreateColdTier(o, &err);
    ASSERT_NE(t, nullptr) << err;
    EXPECT_EQ(t->Name(), "filesystem+metrics");  // metrics outermost

    std::vector<uint8_t> data(64, 0x5A);
    ASSERT_TRUE(t->Put(Key(7), data.data(), data.size(), &err)) << err;
    EXPECT_EQ(Sample(r, "kv_cold_put_total"), 1.0);
    EXPECT_EQ(Sample(r, "kv_cold_put_bytes_total"), 64.0);
}

TEST(ColdTierFactoryTest, NoMetricsRegistryLeavesTierBare) {
    using kvcache::node::tier::ColdTierOptions;
    using kvcache::node::tier::CreateColdTier;
    ColdTierOptions o;
    o.type = "fs";
    o.fs.root = std::string(::testing::TempDir()) + "/kvc_o4_nometrics";
    std::string err;
    auto t = CreateColdTier(o, &err);
    ASSERT_NE(t, nullptr) << err;
    EXPECT_EQ(t->Name(), "filesystem");  // not wrapped
}
