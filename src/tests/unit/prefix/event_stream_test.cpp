#include "prefix/kv_event_stream.h"

#include <gtest/gtest.h>
#include <thread>

using kvcache::node::prefix::Event;
using kvcache::node::prefix::EventStream;
using kvcache::node::prefix::EventType;
using kvcache::node::prefix::Tier;

namespace {
Event MakeEvent(EventType t, uint64_t epoch_hint = 0) {
    Event e{};
    e.type  = t;
    e.tier  = Tier::Pinned;
    e.epoch = epoch_hint;
    return e;
}
}  // namespace

TEST(EventStreamTest, SubscribeAndReceive) {
    EventStream s;
    auto h = s.Subscribe(16);

    s.Publish(MakeEvent(EventType::Add));
    s.Publish(MakeEvent(EventType::Evict));

    Event got{};
    ASSERT_TRUE(s.Poll(h, &got));
    EXPECT_EQ(got.type, EventType::Add);
    EXPECT_EQ(got.epoch, 1u);  // first event in a fresh stream

    ASSERT_TRUE(s.Poll(h, &got));
    EXPECT_EQ(got.type, EventType::Evict);
    EXPECT_EQ(got.epoch, 2u);

    EXPECT_FALSE(s.Poll(h, &got));
    s.Unsubscribe(h);
}

TEST(EventStreamTest, MonotonicEpoch) {
    EventStream s;
    auto h = s.Subscribe(1024);
    constexpr int N = 100;
    for (int i = 0; i < N; ++i) s.Publish(MakeEvent(EventType::Add));
    uint64_t prev = 0;
    Event got;
    while (s.Poll(h, &got)) {
        EXPECT_GT(got.epoch, prev);
        prev = got.epoch;
    }
    EXPECT_EQ(prev, static_cast<uint64_t>(N));
    s.Unsubscribe(h);
}

TEST(EventStreamTest, MultiSubscriberIsolation) {
    EventStream s;
    auto a = s.Subscribe(8);
    auto b = s.Subscribe(8);
    s.Publish(MakeEvent(EventType::Add));
    Event ga{}, gb{};
    EXPECT_TRUE(s.Poll(a, &ga));
    EXPECT_TRUE(s.Poll(b, &gb));
    EXPECT_EQ(ga.epoch, gb.epoch);
    s.Unsubscribe(a);
    s.Unsubscribe(b);
}

TEST(EventStreamTest, OverflowIsObservable) {
    EventStream s;
    auto h = s.Subscribe(8);  // rounded up to 8
    // Publish way more than capacity; subscriber never polls.
    for (int i = 0; i < 100; ++i) s.Publish(MakeEvent(EventType::Add));
    // Drain whatever made it in.
    Event got;
    int got_count = 0;
    while (s.Poll(h, &got)) ++got_count;
    EXPECT_LE(got_count, 8);
    s.Unsubscribe(h);
}
