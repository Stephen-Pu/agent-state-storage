#include "metrics.h"

#include <gtest/gtest.h>
#include <array>

using namespace kvcache::metrics;

TEST(MetricsTest, CounterIncAndScrape) {
    Registry r;
    std::array<std::string_view, 1> keys{"tier"};
    auto& c = r.GetOrCreateCounter("kv_test_counter", "test", keys);
    std::array<Label, 1> labs{Label{"tier", "pinned"}};
    c.Inc(1, labs);
    c.Inc(2.5, labs);

    std::string out;
    r.Scrape(out);
    EXPECT_NE(out.find("# HELP kv_test_counter"), std::string::npos);
    EXPECT_NE(out.find("# TYPE kv_test_counter counter"), std::string::npos);
    EXPECT_NE(out.find("kv_test_counter{tier=\"pinned\"} 3.5"), std::string::npos);
}

TEST(MetricsTest, GaugeSetIncDec) {
    Registry r;
    std::array<std::string_view, 0> keys{};
    auto& g = r.GetOrCreateGauge("kv_test_gauge", "g", keys);
    std::array<Label, 0> labs{};
    g.Set(100, labs);
    g.Inc(5, labs);
    g.Dec(2, labs);
    std::string out;
    r.Scrape(out);
    EXPECT_NE(out.find("kv_test_gauge 103"), std::string::npos);
}

TEST(MetricsTest, HistogramBucketing) {
    Registry r;
    std::array<std::string_view, 0> keys{};
    std::array<double, 3> bounds{1.0, 5.0, 10.0};
    auto& h = r.GetOrCreateHistogram("kv_test_h", "h", keys, bounds);
    std::array<Label, 0> labs{};
    h.Observe(0.5, labs);
    h.Observe(3.0, labs);
    h.Observe(8.0, labs);
    h.Observe(20.0, labs);
    std::string out;
    r.Scrape(out);
    EXPECT_NE(out.find("kv_test_h_count 4"), std::string::npos);
    EXPECT_NE(out.find("kv_test_h_bucket{le=\"+Inf\"} 4"), std::string::npos);
    // 1 obs <= 1.0
    EXPECT_NE(out.find("kv_test_h_bucket{le=\"1.000000\"} 1"), std::string::npos);
}

TEST(MetricsTest, GetOrCreateIsIdempotent) {
    Registry r;
    std::array<std::string_view, 0> keys{};
    auto& a = r.GetOrCreateCounter("dup", "", keys);
    auto& b = r.GetOrCreateCounter("dup", "", keys);
    EXPECT_EQ(&a, &b);
}

// Phase B10.2 — scrape hooks append extra lines to the body, run after
// the registered series and outside the registry mutex.
TEST(MetricsTest, ScrapeHookAppendsLines) {
    Registry r;
    std::array<std::string_view, 0> keys{};
    r.GetOrCreateCounter("base_counter", "", keys).Inc(1, {});
    r.RegisterScrapeHook([](std::string& out) {
        out += "kv_extra_metric 42\n";
    });
    std::string out;
    r.Scrape(out);
    EXPECT_NE(out.find("base_counter"), std::string::npos);
    EXPECT_NE(out.find("kv_extra_metric 42"), std::string::npos);
}

TEST(MetricsTest, ScrapeHookMayReenterRegistry) {
    // A hook running outside mu_ may touch the Registry without deadlock.
    Registry r;
    r.RegisterScrapeHook([&r](std::string& out) {
        std::array<std::string_view, 0> keys{};
        // Re-entrant GetOrCreateGauge would deadlock if hooks ran under mu_.
        r.GetOrCreateGauge("hook_made_gauge", "", keys).Set(7, {});
        out += "hook_ran 1\n";
    });
    std::string out;
    r.Scrape(out);
    EXPECT_NE(out.find("hook_ran 1"), std::string::npos);
}
