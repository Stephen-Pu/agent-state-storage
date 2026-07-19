// SS-2 B-plane spike, Task 1 — Entry carries state_kind; Insert threads it.
// SS-2 B-plane spike, Task 2 — DramTier dispatches eviction by each entry's
// real state_kind via the ValuePolicyRegistry (no more synthetic SK_KV).
#include "tier/dram_tier.h"

#include <gtest/gtest.h>
#include <memory>
#include <vector>

#include "value_policy.h"
#include "value_policy_kv.h"
#include "value_policy_persistent_stub.h"

using kvcache::common::SK_KV;
using kvcache::common::SK_MEMORY;
using kvcache::common::ValuePolicyKv;
using kvcache::common::ValuePolicyPersistentStub;
using kvcache::common::ValuePolicyRegistry;
using kvcache::node::tier::DramKey;
using kvcache::node::tier::DramTier;

namespace {
DramTier::Options SmallOpts() {
    DramTier::Options o;
    o.capacity_bytes    = 1024;
    o.a1out_max_entries = 16;
    return o;
}
}  // namespace

TEST(DramTierKind, InsertRecordsKindDefaultKv) {
    DramTier t(SmallOpts());
    DramKey k{};
    k.bytes[0] = 1;
    std::vector<uint8_t> v(10, 0xAA);
    t.Insert(k, v.data(), v.size());  // default kind
    ASSERT_TRUE(t.KindOf(k).has_value());
    EXPECT_EQ(*t.KindOf(k), static_cast<uint16_t>(SK_KV));

    DramKey k2{};
    k2.bytes[0] = 2;
    t.Insert(k2, v.data(), v.size(), /*state_kind=*/16);  // e.g. SK_MEMORY
    ASSERT_TRUE(t.KindOf(k2).has_value());
    EXPECT_EQ(*t.KindOf(k2), 16u);
}

TEST(DramTierKind, KindOfMissingKeyIsNullopt) {
    DramTier t(SmallOpts());
    DramKey missing{};
    missing.bytes[0] = 99;
    EXPECT_FALSE(t.KindOf(missing).has_value());
}

TEST(DramTierKind, InPlaceReplaceUpdatesKind) {
    DramTier t(SmallOpts());
    DramKey k{};
    k.bytes[0] = 3;
    std::vector<uint8_t> v(10, 0xBB);
    t.Insert(k, v.data(), v.size());  // default SK_KV
    ASSERT_TRUE(t.KindOf(k).has_value());
    EXPECT_EQ(*t.KindOf(k), static_cast<uint16_t>(SK_KV));

    // Replace in place (same key, still resident) with a new kind.
    t.Insert(k, v.data(), v.size(), /*state_kind=*/16);
    ASSERT_TRUE(t.KindOf(k).has_value());
    EXPECT_EQ(*t.KindOf(k), 16u);
}

// SS-2 B-plane spike, Task 2 — the tier now dispatches shouldEvict by each
// entry's own state_kind through the registry, instead of the earlier
// spike's synthetic SK_KV-only projection. IsNotEvictable is private, so we
// assert through eviction behavior (mirrors DramTierTest.GhostHitPromotesToAm
// in dram_tier_test.cpp): with a 2-policy registry wired in
// (SK_KV -> ValuePolicyKv, SK_MEMORY -> ValuePolicyPersistentStub), a KV
// entry must still evict exactly as it did with no registry at all —
// ValuePolicyKv::shouldEvict is unconditionally kEvictable, so the
// registry's presence (and the unrelated SK_MEMORY policy sharing it) must
// not change the KV path's victims. Task 3 exercises the SK_MEMORY
// not-evictable path; this only proves the registry wiring compiles and
// dispatches correctly for the kind that's actually present.
TEST(DramTierKind, KvEntryStillEvictsNormallyWithRegistryWired) {
    ValuePolicyRegistry reg;
    reg.registerPolicy(SK_KV, std::make_unique<ValuePolicyKv>());
    reg.registerPolicy(SK_MEMORY, std::make_unique<ValuePolicyPersistentStub>());

    DramTier::Options o;
    o.capacity_bytes    = 256;   // a1in capacity = 64
    o.a1out_max_entries = 16;
    o.policy_registry   = &reg;
    DramTier t(o);

    DramKey k1{};
    k1.bytes[0] = 1;
    DramKey k2{};
    k2.bytes[0] = 2;
    std::vector<uint8_t> v(64, 0xCC);

    // Default state_kind on Insert() is SK_KV. First insert fits within the
    // 64-byte A1in budget; the second forces the first out into the ghost
    // queue — same victim as the no-registry case (registry-gated
    // shouldEvict for SK_KV is always kEvictable).
    t.Insert(k1, v.data(), v.size());
    t.Insert(k2, v.data(), v.size());

    EXPECT_FALSE(t.KindOf(k1).has_value());  // evicted out of A1in/Am
    ASSERT_TRUE(t.KindOf(k2).has_value());
    EXPECT_EQ(*t.KindOf(k2), static_cast<uint16_t>(SK_KV));

    auto r1 = t.Lookup(k1);
    EXPECT_EQ(r1.where, DramTier::HitWhere::kGhost);
}

// Task 3 — EvictToFit walk-and-skip. A NOT_EVICTABLE entry (SK_MEMORY via
// ValuePolicyPersistentStub) sitting in the middle of the eviction order
// must be skipped, not treated as a wall that halts the whole pass: the
// walk continues past it and evicts the next evictable candidate instead.
TEST(DramTierKind, EvictionSkipsNotEvictableAndDropsEvictable) {
    ValuePolicyRegistry reg;
    reg.registerPolicy(SK_KV, std::make_unique<ValuePolicyKv>());
    reg.registerPolicy(SK_MEMORY, std::make_unique<ValuePolicyPersistentStub>());

    DramTier::Options o;
    o.capacity_bytes    = 256;   // a1in capacity = 64 (25%)
    o.a1out_max_entries = 16;
    o.policy_registry   = &reg;
    DramTier t(o);

    std::vector<uint8_t> v(32, 0xCC);

    // A1in is push_front, so front = newest, back = oldest. A1in budget is
    // 64 bytes = 2 entries of 32 bytes. We insert 3 entries in oldest→newest
    // order so the tail-to-front layout is [evictable_a (tail), not_evictable,
    // evictable_b (front)]; the 3rd insert triggers EvictToFit.
    DramKey evictable_a{};  // will end up at the tail (oldest)
    evictable_a.bytes[0] = 1;
    DramKey not_evictable{};  // middle — must survive
    not_evictable.bytes[0] = 2;
    DramKey evictable_b{};  // newest (front)
    evictable_b.bytes[0] = 3;

    t.Insert(evictable_a, v.data(), v.size(), SK_KV);          // tail (oldest)
    t.Insert(not_evictable, v.data(), v.size(), SK_MEMORY);    // middle
    // At this point A1in holds 64 bytes (at budget). Inserting evictable_b
    // (32 more bytes) forces EvictToFit(32) to free at least 32 bytes from
    // A1in: the tail is evictable_a (SK_KV, evictable) — the walk finds it
    // immediately (byte-identical to old pop_back()) and evicts it first.
    t.Insert(evictable_b, v.data(), v.size(), SK_KV);          // front (newest)

    // evictable_a (tail, evictable) went first; not_evictable and
    // evictable_b both survive this pass.
    EXPECT_FALSE(t.KindOf(evictable_a).has_value());
    ASSERT_TRUE(t.KindOf(not_evictable).has_value());
    EXPECT_EQ(*t.KindOf(not_evictable), static_cast<uint16_t>(SK_MEMORY));
    ASSERT_TRUE(t.KindOf(evictable_b).has_value());

    // Now push another evictable entry in: A1in is again at/over budget
    // (not_evictable + evictable_b = 64 bytes). The walk must skip
    // not_evictable (at the tail) and evict evictable_b's predecessor... to
    // directly exercise "skip a not-evictable tail and take the next
    // evictable one", insert one more entry so eviction is required while
    // not_evictable sits at the tail.
    DramKey evictable_c{};
    evictable_c.bytes[0] = 4;
    t.Insert(evictable_c, v.data(), v.size(), SK_KV);

    // not_evictable is at the tail now (oldest of the two remaining) and
    // must NOT be evicted; evictable_b (next toward the front) must be the
    // one dropped instead — proving the walk skipped over not_evictable.
    ASSERT_TRUE(t.KindOf(not_evictable).has_value());
    EXPECT_EQ(*t.KindOf(not_evictable), static_cast<uint16_t>(SK_MEMORY));
    EXPECT_FALSE(t.KindOf(evictable_b).has_value());
    ASSERT_TRUE(t.KindOf(evictable_c).has_value());
}

// Task 3 — the critical no-hang case. If DRAM holds ONLY NOT_EVICTABLE
// entries and capacity is exceeded, the walk must find nothing evictable,
// stop cleanly (capacity left unsatisfied), and return — never spin.
TEST(DramTierKind, AllNotEvictableStopsCleanlyNoHang) {
    ValuePolicyRegistry reg;
    reg.registerPolicy(SK_MEMORY, std::make_unique<ValuePolicyPersistentStub>());

    DramTier::Options o;
    o.capacity_bytes    = 128;   // a1in capacity = 32 (25%)
    o.a1out_max_entries = 16;
    o.policy_registry   = &reg;
    DramTier t(o);

    std::vector<uint8_t> v(32, 0xEE);

    DramKey k1{};
    k1.bytes[0] = 1;
    DramKey k2{};
    k2.bytes[0] = 2;

    // Fill A1in to exactly its 32-byte budget with one NOT_EVICTABLE entry.
    t.Insert(k1, v.data(), v.size(), SK_MEMORY);
    ASSERT_TRUE(t.KindOf(k1).has_value());

    // Insert a second NOT_EVICTABLE entry that would need eviction to fit
    // (A1in budget is 32 bytes; this would push usage to 64). EvictToFit
    // must return (not hang) even though nothing is evictable, leaving
    // capacity intentionally unsatisfied.
    t.Insert(k2, v.data(), v.size(), SK_MEMORY);

    // Both entries remain resident — nothing was evictable, so nothing was
    // evicted, and the call returned instead of spinning forever.
    ASSERT_TRUE(t.KindOf(k1).has_value());
    EXPECT_EQ(*t.KindOf(k1), static_cast<uint16_t>(SK_MEMORY));
    ASSERT_TRUE(t.KindOf(k2).has_value());
    EXPECT_EQ(*t.KindOf(k2), static_cast<uint16_t>(SK_MEMORY));
}

// Task 5 — white-box peer. A NOT_EVICTABLE (B-class) entry cannot reach Am
// through the public API today (Am admission requires a ghost hit, which
// requires a prior A1in eviction, and B is never evicted from A1in), so the
// second EvictToFit loop's Am→A1in fallback is unreachable via Insert/Lookup.
// This peer injects entries directly into the private a1in_/am_ lists and
// drives EvictToFit so the fallback can be exercised. DramTier befriends it.
namespace kvcache::node::tier {
class DramTierTestPeer {
   public:
    explicit DramTierTestPeer(DramTier& t) : t_(t) {}

    void PushAm(const DramKey& k, std::size_t n, uint16_t kind) {
        t_.am_.push_front(DramTier::Entry{
            k, DramTier::Queue::Am, std::vector<uint8_t>(n, 0), kind});
        t_.index_[k] = t_.am_.begin();
        t_.am_bytes_used_ += n;
    }
    void PushA1in(const DramKey& k, std::size_t n, uint16_t kind) {
        t_.a1in_.push_front(DramTier::Entry{
            k, DramTier::Queue::A1in, std::vector<uint8_t>(n, 0), kind});
        t_.index_[k] = t_.a1in_.begin();
        t_.a1in_bytes_used_ += n;
    }
    void CallEvictToFit(std::size_t incoming) { t_.EvictToFit(incoming); }

    uint64_t AmBytes()   const { return t_.am_bytes_used_; }
    uint64_t A1inBytes() const { return t_.a1in_bytes_used_; }

   private:
    DramTier& t_;
};
}  // namespace kvcache::node::tier

using kvcache::node::tier::DramTierTestPeer;

// Task 5 — the Am-fallback correctness case (the whole-branch review's one
// Important finding). Loop 2 of EvictToFit prefers Am (true LRU); when Am is
// non-empty but EVERY Am entry is NOT_EVICTABLE, it must fall through to A1in
// and evict an evictable entry there — NOT break out of the loop leaving
// reclaimable A1in space untouched (which would keep the tier over budget).
//
// Setup is chosen so loop 1 (A1in budget) does NOT fire — only loop 2 does —
// isolating the Am→A1in fallback:
//   capacity = 400, a1in budget = 100 (25%).
//   A1in: one evictable SK_KV entry of 50 bytes  (a1in_used = 50).
//   Am:   one NOT_EVICTABLE SK_MEMORY entry of 340 bytes (am_used = 340).
//   EvictToFit(incoming = 40):
//     loop 1: 50 + 40 = 90 <= 100 → does not fire (A1in entry survives loop 1).
//     loop 2: 50 + 340 + 40 = 430 > 400 → fires. Am's only entry is
//             NOT_EVICTABLE → old code would break here (BUG). New code falls
//             through to A1in, evicts the 50-byte SK_KV entry → 0 + 340 + 40 =
//             380 <= 400 → loop exits.
// Under the OLD code this test fails: the A1in entry stays resident.
TEST(DramTierKind, AmAllNotEvictableFallsBackToA1in) {
    ValuePolicyRegistry reg;
    reg.registerPolicy(SK_MEMORY, std::make_unique<ValuePolicyPersistentStub>());
    reg.registerPolicy(SK_KV, std::make_unique<ValuePolicyKv>());

    DramTier::Options o;
    o.capacity_bytes    = 400;
    o.a1out_max_entries = 16;
    o.policy_registry   = &reg;
    DramTier t(o);

    DramTierTestPeer peer(t);
    DramKey am_b{};    // NOT_EVICTABLE, resides in Am
    am_b.bytes[0] = 1;
    DramKey a1_a{};    // evictable, resides in A1in
    a1_a.bytes[0] = 2;

    peer.PushAm(am_b, 340, SK_MEMORY);
    peer.PushA1in(a1_a, 50, SK_KV);
    ASSERT_EQ(peer.AmBytes(), 340u);
    ASSERT_EQ(peer.A1inBytes(), 50u);

    // Drives loop 2 only (loop 1 stays within the 100-byte A1in budget).
    peer.CallEvictToFit(40);

    // The NOT_EVICTABLE Am entry survives; the evictable A1in entry is the
    // fallback victim — proving loop 2 did not break on Am-not-found.
    ASSERT_TRUE(t.KindOf(am_b).has_value());
    EXPECT_EQ(*t.KindOf(am_b), static_cast<uint16_t>(SK_MEMORY));
    EXPECT_FALSE(t.KindOf(a1_a).has_value());
    EXPECT_EQ(peer.AmBytes(), 340u);
    EXPECT_EQ(peer.A1inBytes(), 0u);
}

// Task 5 — companion no-hang case for the fallback: BOTH queues hold only
// NOT_EVICTABLE entries and capacity is exceeded via loop 2. Neither Am nor
// the A1in fallback yields a victim, so EvictToFit must stop cleanly (capacity
// unsatisfied) rather than spin. Same non-loop-1 setup as above.
TEST(DramTierKind, AmAndA1inAllNotEvictableStopsCleanlyNoHang) {
    ValuePolicyRegistry reg;
    reg.registerPolicy(SK_MEMORY, std::make_unique<ValuePolicyPersistentStub>());

    DramTier::Options o;
    o.capacity_bytes    = 400;
    o.a1out_max_entries = 16;
    o.policy_registry   = &reg;
    DramTier t(o);

    DramTierTestPeer peer(t);
    DramKey am_b{};
    am_b.bytes[0] = 1;
    DramKey a1_b{};
    a1_b.bytes[0] = 2;

    peer.PushAm(am_b, 340, SK_MEMORY);
    peer.PushA1in(a1_b, 50, SK_MEMORY);  // also NOT_EVICTABLE

    peer.CallEvictToFit(40);  // must return, not hang

    // Both survive — nothing was evictable in either queue.
    ASSERT_TRUE(t.KindOf(am_b).has_value());
    ASSERT_TRUE(t.KindOf(a1_b).has_value());
    EXPECT_EQ(peer.AmBytes(), 340u);
    EXPECT_EQ(peer.A1inBytes(), 50u);
}
