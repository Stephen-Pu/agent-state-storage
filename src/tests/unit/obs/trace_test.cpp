// trace.h / trace.cpp — facade unit tests.
//
// The tracer is a process-wide singleton, so each test installs its own
// InMemoryExporter, exercises the facade, captures the exported spans,
// then resets the exporter back to the default no-op.
#include "trace.h"

#include <gtest/gtest.h>

#include <thread>
#include <vector>

using namespace kvcache::trace;

namespace {

// Fixture: each test gets its own fresh InMemoryExporter installed; the
// teardown leaves the tracer with the default no-op exporter so later
// tests in the same binary don't see leaked spans.
class TraceTest : public ::testing::Test {
   protected:
    void SetUp() override {
        exp_ = std::make_shared<InMemoryExporter>();
        Tracer::Get().SetExporter(exp_);
    }
    void TearDown() override {
        Tracer::Get().SetExporter(nullptr);
    }
    std::shared_ptr<InMemoryExporter> exp_;
};

}  // namespace

TEST_F(TraceTest, RootSpanHasNoParentAndExportsOnDestroy) {
    {
        auto s = Tracer::Get().StartSpan("root");
        EXPECT_TRUE(s.Active());
        EXPECT_EQ(exp_->Size(), 0u);  // not yet ended
    }
    ASSERT_EQ(exp_->Size(), 1u);
    const auto spans = exp_->spans();
    const auto& span = spans.front();
    EXPECT_EQ(span.name, "root");
    EXPECT_FALSE(span.parent.IsValid());
    EXPECT_TRUE(span.context.IsValid());
    EXPECT_EQ(span.status, StatusCode::Ok);
}

TEST_F(TraceTest, ChildAdoptsParentTraceIdButNewSpanId) {
    SpanContext parent_ctx;
    SpanContext child_ctx;
    {
        auto parent = Tracer::Get().StartSpan("parent");
        parent_ctx = parent.Context();
        {
            auto child = Tracer::Get().StartSpan("child");
            child_ctx  = child.Context();
        }
    }
    ASSERT_EQ(exp_->Size(), 2u);
    auto spans = exp_->spans();
    // Child is exported first (it ends first), then parent.
    EXPECT_EQ(spans[0].name, "child");
    EXPECT_EQ(spans[1].name, "parent");
    EXPECT_EQ(spans[0].parent.span_id, parent_ctx.span_id);
    EXPECT_EQ(spans[0].context.trace_id, parent_ctx.trace_id);
    EXPECT_NE(spans[0].context.span_id, parent_ctx.span_id);
    (void)child_ctx;
}

TEST_F(TraceTest, AttributesAndErrorStatusAreCaptured) {
    {
        auto s = Tracer::Get().StartSpan("op");
        s.SetAttribute("str", std::string_view("hello"));
        s.SetAttribute("i64", int64_t{42});
        s.SetAttribute("f64", 3.14);
        s.SetAttribute("flag", true);
        s.SetError("kaboom");
    }
    ASSERT_EQ(exp_->Size(), 1u);
    const auto spans = exp_->spans();
    const auto& sp = spans.front();
    EXPECT_EQ(sp.status, StatusCode::Error);
    EXPECT_EQ(sp.status_message, "kaboom");
    ASSERT_EQ(sp.attributes.size(), 4u);
    EXPECT_EQ(sp.attributes[0].key, "str");
    EXPECT_EQ(std::get<std::string>(sp.attributes[0].value), "hello");
    EXPECT_EQ(std::get<int64_t>(sp.attributes[1].value), 42);
    EXPECT_DOUBLE_EQ(std::get<double>(sp.attributes[2].value), 3.14);
    EXPECT_EQ(std::get<bool>(sp.attributes[3].value), true);
}

TEST_F(TraceTest, MutationsAfterEndAreNoops) {
    auto s = Tracer::Get().StartSpan("op");
    s.End();
    EXPECT_EQ(exp_->Size(), 1u);
    // No-ops; do not throw.
    s.SetAttribute("late", std::string_view("nope"));
    s.SetError("late");
    // Destructor must not double-export.
    s = Tracer::Get().StartSpan("other");
    EXPECT_EQ(exp_->Size(), 1u);  // first span exported on End, no extras
    s.End();
    EXPECT_EQ(exp_->Size(), 2u);
}

TEST_F(TraceTest, ThreadsGetIndependentActiveStacks) {
    // The active-stack is thread_local, so a child started on thread B
    // should NOT see thread A's open parent as its parent.
    constexpr int kThreads = 4;
    std::vector<std::thread> ts;
    for (int i = 0; i < kThreads; ++i) {
        ts.emplace_back([] {
            auto s = Tracer::Get().StartSpan("per-thread");
        });
    }
    for (auto& t : ts) t.join();

    auto spans = exp_->spans();
    ASSERT_EQ(spans.size(), static_cast<std::size_t>(kThreads));
    // Each span is a root — no parent set.
    for (const auto& sp : spans) EXPECT_FALSE(sp.parent.IsValid());
    // Distinct trace_ids (overwhelmingly likely).
    std::array<std::array<uint8_t, 16>, kThreads> ids{};
    for (int i = 0; i < kThreads; ++i) ids[i] = spans[i].context.trace_id;
    for (int i = 0; i < kThreads; ++i)
        for (int j = i + 1; j < kThreads; ++j)
            EXPECT_NE(ids[i], ids[j]);
}

TEST_F(TraceTest, MovedSpanDoesNotDoubleExport) {
    {
        auto a = Tracer::Get().StartSpan("moved");
        auto b = std::move(a);
        EXPECT_FALSE(a.Active());
        EXPECT_TRUE (b.Active());
    }
    EXPECT_EQ(exp_->Size(), 1u);
}

TEST_F(TraceTest, NoopExporterByDefault) {
    Tracer::Get().SetExporter(nullptr);
    // Without an exporter the facade still runs, just discards.
    { auto s = Tracer::Get().StartSpan("dropped"); }
    EXPECT_EQ(exp_->Size(), 0u);
}
