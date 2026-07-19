# SS-2 Spine Spike — ValuePolicy + StateIdentity Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** De-risk the State Storage pivot's load-bearing assumption (LLD Q3 / SS-2): prove that **one `ValuePolicy` interface + a `state_kind`-keyed registry** can cleanly cover both an A-class policy (KVCache: store-vs-recompute, cost-evictable) and a semantically-opposite B-class policy (unconditional store, NOT_EVICTABLE) — with **zero scattered `if state_kind` on the hot path** and **all existing tests green**.

**Architecture:** Additive, C++-core-only. Introduce `state_identity_t` (128B) as a superset of the frozen 64B `kv_locator_t` (KV projects to it; the 64B struct is untouched). Introduce `ValuePolicy{shouldStore,shouldEvict,onMiss}` + `ValuePolicyRegistry.of(state_kind)` (LLD §3.3.8 / SS-2). Implement `ValuePolicyKv` — which **first-implements the economic `shouldStore`** (D-PERF-1 as designed, currently defined-but-never-produced), behavior-preserving on the hot path — and a `ValuePolicyPersistentStub` (B-stub) that proves the interface admits opposite semantics. Route the three real decision sites (store at `SealCommit`, evict at `DramTier::EvictToFit`, miss at `HeadlessNode::Lookup`/`FetchWithPriority`) through the registry without changing behavior.

**Tech Stack:** C++20, CMake+Ninja, GoogleTest (FetchContent). Existing `src/core/common`, `src/core-abi` (HeadlessNode), `src/kvstore-node/src/tier` (DramTier).

**Design source:** `KV_Cache_Design/KV_Cache_LLD_详细设计.md` — SS-2 (§ line 169), `state_identity_t`/`state_kind_e`/`si_flags_e` (§2.1b, line 227), `ValuePolicyRegistry` (§3.3.8, line 758), per-state_kind policy table (line 772). This spike implements the **minimal closed loop** of that design; **explicitly out of scope** (per agreed scope): the §3.3.8.1 A→B promotion channel (θ unset, 未决 #31), other A-class `state_kind` connectors (sandbox/embedding/tool-result), the real ⑬ persistence engine, BLAKE3 for non-KV, and production activation of `shouldStore` with real cost telemetry.

## Global Constraints

- **The 64-byte `kv_locator_t` is FROZEN.** It is a wire/ABI format consumed by the C ABI, shmem ring, ART leaf, RocksDB key derivation, two `.proto` files, and C++/Python/Rust FFI. `static_assert(sizeof(kv_locator_t)==64)` lives at `src/include/kvcache/kv_types.h:90-94` and is mirrored in Rust (`src/adapters/rust/src/lib.rs:428-429`). **Do NOT change it.** `state_identity_t` is a NEW, additive, **C++-core-only** superset (lives in `src/core/common/`, NOT in the C ABI header `kv_types.h`); KV projects onto it. Nothing crosses the FFI/proto/wire boundary in this spike.
- **All existing tests stay green** (run the full suite at the end of the wiring task). Behavior is preserved: `ValuePolicyKv::shouldStore` returns "store" for the inputs the hot path supplies today (no real cost telemetry yet → behavior-preserving default); `shouldEvict` picks the same victims `EvictToFit` picks today; `onMiss` yields the same `KV_E_NOT_FOUND`.
- **No scattered `if state_kind`.** The decision sites call `registry.of(kind).<method>(...)`. Any inline `if (state_kind == ...)` branch on the hot path that a policy call should own is a Q3 failure.
- No new third-party dependency. New headers are header-only where practical; `.cpp` only when a translation unit is needed.
- New C++ core code lives under `src/core/common/` (same module as `locator.{h,cpp}`); tests under `src/tests/unit/common/` and `src/tests/unit/policy/`, wired in `src/tests/unit/CMakeLists.txt` following neighboring targets.
- TDD: failing test → run-fail → minimal impl → run-pass → commit, per step. Build: CMake+Ninja into `build/`; `ctest --test-dir build`.

## File Structure

- `src/core/common/state_identity.h` — `state_kind_e`, `si_flags_e`, `state_identity_t` (128B) + `StateIdentityFromLocator()`. Header-only. Task 1.
- `src/core/common/value_policy.h` — `EvictDecision`, `OnMissAction`, `CostModel`, abstract `ValuePolicy`, `ValuePolicyRegistry`. Header-only. Task 2.
- `src/core/common/value_policy_kv.h` / `.cpp` — `ValuePolicyKv` (incl. the economic `shouldStore`). Task 3.
- `src/core/common/value_policy_persistent_stub.h` — `ValuePolicyPersistentStub` (B-stub, header-only). Task 4.
- `src/core-abi/src/headless_node.{h,cpp}` — own a `ValuePolicyRegistry`; route `SealCommit` store + the miss path through it. Task 5.
- `src/kvstore-node/src/tier/dram_tier.{h,cpp}` — route `EvictToFit` through a `ValuePolicy` (injected). Task 5.
- Tests: `state_identity_test.cpp`, `value_policy_registry_test.cpp`, `value_policy_kv_test.cpp`, `value_policy_ab_contrast_test.cpp` (the Q3 core), plus full-suite regression. Tasks 1–6.
- `docs/` note on D-PERF-1 status (SS-3 honesty). Task 6.

---

### Task 1: `state_identity_t` (128B) + KV projection — additive, C++-core-only

**Files:**
- Create: `src/core/common/state_identity.h`
- Test: `src/tests/unit/common/state_identity_test.cpp`
- Modify: `src/tests/unit/CMakeLists.txt`

**Interfaces:**
- Consumes: `kv_locator_t` (`src/include/kvcache/kv_types.h` — the frozen 64B struct: `uint8_t tenant_id[16]; uint64_t model_id_hash; uint8_t prefix_hash[16]; kv_range_t range; uint32_t version; uint32_t flags;`).
- Produces:
  ```cpp
  namespace kvcache::common {
  enum state_kind_e : uint16_t {
      SK_KV = 0, SK_SANDBOX = 1, SK_EMBEDDING = 2, SK_TOOL_RESULT = 3,   // A
      SK_MEMORY = 16, SK_EXEC_STATE = 17,                                 // B
      SK_MAX = 18
  };
  enum si_flags_e : uint16_t {
      SIF_IDEMPOTENT = 1u << 0, SIF_PERSISTENT_B = 1u << 1, SIF_HAS_RECIPE = 1u << 2
  };
  struct StateIdentity {                 // 128 bytes, C++-core-only superset of kv_locator_t
      uint32_t version;                  // = 2
      uint16_t state_kind;               // state_kind_e
      uint16_t flags;                    // si_flags_e bits
      uint64_t tenant_id_lo;             // low 8 bytes of tenant (KV: first 8 of tenant_id[16])
      uint8_t  content_hash[32];         // KV: model_id_hash(8) || prefix_hash(16) || pad
      uint64_t recipe_ref;               // 0 = trivial (identity≈lineage)
      uint8_t  shape[40];                // KV: layer/head/token range bytes
      uint8_t  reserved[32];
  };
  static_assert(sizeof(StateIdentity) == 128, "StateIdentity must be 128 bytes (LLD §2.1b)");
  // KV projection: build a StateIdentity from the frozen 64B locator. state_kind=SK_KV,
  // recipe_ref=0. Does NOT modify the locator.
  StateIdentity StateIdentityFromLocator(const kv_locator_t& loc);
  }  // namespace
  ```

- [ ] **Step 1: Write the failing test**

```cpp
// src/tests/unit/common/state_identity_test.cpp
#include "common/state_identity.h"
#include "kvcache/kv_types.h"
#include <gtest/gtest.h>
#include <cstring>
using namespace kvcache::common;

TEST(StateIdentity, IsExactly128Bytes) {
    static_assert(sizeof(StateIdentity) == 128, "");
    EXPECT_EQ(sizeof(StateIdentity), 128u);
}

TEST(StateIdentity, LocatorIsStillFrozen64) {
    // Guard: the spike must not have perturbed the frozen wire struct.
    EXPECT_EQ(sizeof(kv_locator_t), 64u);
}

TEST(StateIdentity, KvProjectionCarriesIdentityFields) {
    kv_locator_t loc{};
    std::memset(loc.tenant_id, 0xAB, sizeof(loc.tenant_id));
    loc.model_id_hash = 0x1122334455667788ull;
    std::memset(loc.prefix_hash, 0xCD, sizeof(loc.prefix_hash));
    loc.version = 1;

    StateIdentity id = StateIdentityFromLocator(loc);
    EXPECT_EQ(id.version, 2u);
    EXPECT_EQ(id.state_kind, static_cast<uint16_t>(SK_KV));
    EXPECT_EQ(id.recipe_ref, 0u) << "KV: identity≈lineage, trivial recipe";
    // content_hash starts with model_id_hash (LE 8 bytes) then prefix_hash (16 bytes).
    uint64_t got_model = 0;
    std::memcpy(&got_model, id.content_hash, sizeof(got_model));
    EXPECT_EQ(got_model, loc.model_id_hash);
    EXPECT_EQ(std::memcmp(id.content_hash + 8, loc.prefix_hash, 16), 0);
}
```

- [ ] **Step 2: Run to verify fail** — `cmake --build build --target test_state_identity && ./build/tests/unit/test_state_identity` — FAIL (header missing).

- [ ] **Step 3: Implement `state_identity.h`**

```cpp
// src/core/common/state_identity.h
#pragma once
#include <cstdint>
#include <cstring>
#include "kvcache/kv_types.h"   // kv_locator_t (frozen 64B)
namespace kvcache::common {
enum state_kind_e : uint16_t {
    SK_KV = 0, SK_SANDBOX = 1, SK_EMBEDDING = 2, SK_TOOL_RESULT = 3,
    SK_MEMORY = 16, SK_EXEC_STATE = 17, SK_MAX = 18
};
enum si_flags_e : uint16_t {
    SIF_IDEMPOTENT = 1u << 0, SIF_PERSISTENT_B = 1u << 1, SIF_HAS_RECIPE = 1u << 2
};
struct StateIdentity {
    uint32_t version;
    uint16_t state_kind;
    uint16_t flags;
    uint64_t tenant_id_lo;
    uint8_t  content_hash[32];
    uint64_t recipe_ref;
    uint8_t  shape[40];
    uint8_t  reserved[32];
};
static_assert(sizeof(StateIdentity) == 128, "StateIdentity must be 128 bytes (LLD §2.1b)");

inline StateIdentity StateIdentityFromLocator(const kv_locator_t& loc) {
    StateIdentity id{};
    id.version    = 2;
    id.state_kind = SK_KV;
    id.flags      = 0;
    std::memcpy(&id.tenant_id_lo, loc.tenant_id, sizeof(id.tenant_id_lo));   // low 8B
    std::memcpy(id.content_hash,      &loc.model_id_hash, sizeof(loc.model_id_hash)); // 8B
    std::memcpy(id.content_hash + 8,  loc.prefix_hash,    sizeof(loc.prefix_hash));   // 16B
    id.recipe_ref = 0;
    std::memcpy(id.shape, &loc.range, sizeof(loc.range) <= sizeof(id.shape)
                                        ? sizeof(loc.range) : sizeof(id.shape));
    return id;
}
}  // namespace kvcache::common
```
*(Verify the exact `kv_locator_t` field names + `kv_range_t` size by reading `src/include/kvcache/kv_types.h` before writing; the memcpy sizes must match the real fields. If `sizeof(kv_range_t) > 40` adjust the shape copy — it is 16B per the mapper, so it fits.)*

- [ ] **Step 4: Wire the test target** — add `test_state_identity` to `src/tests/unit/CMakeLists.txt`, mirroring the existing `test_locator` / `common/`-module test target (it needs only `state_identity.h` + the `kv_types.h` include path; header-only, no extra link).

- [ ] **Step 5: Run to verify pass** — `./build/tests/unit/test_state_identity` — PASS (3 tests). Confirm the full common suite still builds: `cmake --build build --target test_locator && ./build/tests/unit/test_locator`.

- [ ] **Step 6: Commit**

```bash
git add src/core/common/state_identity.h src/tests/unit/common/state_identity_test.cpp src/tests/unit/CMakeLists.txt
git commit -m "feat(SS-2): StateIdentity (128B) + KV projection — additive superset, 64B locator frozen"
```

---

### Task 2: `ValuePolicy` interface + `ValuePolicyRegistry`

**Files:**
- Create: `src/core/common/value_policy.h`
- Test: `src/tests/unit/policy/value_policy_registry_test.cpp`
- Modify: `src/tests/unit/CMakeLists.txt`

**Interfaces:**
- Consumes: Task 1 `StateIdentity`, `state_kind_e`, `SK_MAX`.
- Produces:
  ```cpp
  namespace kvcache::common {
  enum class EvictDecision { kEvictable, kNotEvictable };   // B: kNotEvictable (demote-only)
  enum class OnMissAction { kRecompute, kReplayFromPersist, kNone };
  struct CostModel {                 // inputs to the store-vs-recompute economic decision
      double fetch_cost_ms    = 0.0; // est. cost to fetch/store+retrieve this state
      double recompute_cost_ms= 0.0; // est. cost to regenerate it
      uint32_t reuse_hint     = 0;   // observed/expected reuse count (0 = unknown)
  };
  class ValuePolicy {
   public:
    virtual ~ValuePolicy() = default;
    virtual bool          shouldStore(const StateIdentity&, const CostModel&) = 0;
    virtual EvictDecision shouldEvict(const StateIdentity&, int tier)         = 0;
    virtual OnMissAction  onMiss(const StateIdentity&)                        = 0;
  };
  // state_kind-keyed registry; the tiering engine / evictor / fetch-decider all call of(kind).
  class ValuePolicyRegistry {
   public:
    void  registerPolicy(uint16_t state_kind, std::unique_ptr<ValuePolicy> p);
    ValuePolicy& of(uint16_t state_kind);   // precondition: a policy was registered for state_kind
    bool  has(uint16_t state_kind) const;
   private:
    std::array<std::unique_ptr<ValuePolicy>, SK_MAX> policies_{};
  };
  }
  ```

- [ ] **Step 1: Write the failing test**

```cpp
// src/tests/unit/policy/value_policy_registry_test.cpp
#include "common/value_policy.h"
#include <gtest/gtest.h>
using namespace kvcache::common;
namespace {
struct FakePolicy : ValuePolicy {
    bool store; EvictDecision ev; OnMissAction miss;
    FakePolicy(bool s, EvictDecision e, OnMissAction m) : store(s), ev(e), miss(m) {}
    bool shouldStore(const StateIdentity&, const CostModel&) override { return store; }
    EvictDecision shouldEvict(const StateIdentity&, int) override { return ev; }
    OnMissAction onMiss(const StateIdentity&) override { return miss; }
};
}  // namespace

TEST(ValuePolicyRegistry, RegisterAndRetrieveByStateKind) {
    ValuePolicyRegistry reg;
    EXPECT_FALSE(reg.has(SK_KV));
    reg.registerPolicy(SK_KV, std::make_unique<FakePolicy>(true, EvictDecision::kEvictable, OnMissAction::kRecompute));
    ASSERT_TRUE(reg.has(SK_KV));
    StateIdentity id{}; id.state_kind = SK_KV;
    EXPECT_TRUE(reg.of(SK_KV).shouldStore(id, CostModel{}));
    EXPECT_EQ(reg.of(SK_KV).shouldEvict(id, /*tier*/2), EvictDecision::kEvictable);
    EXPECT_EQ(reg.of(SK_KV).onMiss(id), OnMissAction::kRecompute);
}
```

- [ ] **Step 2: Run to verify fail** — `cmake --build build --target test_value_policy_registry && ./build/tests/unit/test_value_policy_registry` — FAIL (header missing).

- [ ] **Step 3: Implement `value_policy.h`** — the types above, plus:
```cpp
#include <array>
#include <memory>
#include <cassert>
#include "common/state_identity.h"
// ... types ...
inline void ValuePolicyRegistry::registerPolicy(uint16_t sk, std::unique_ptr<ValuePolicy> p) {
    assert(sk < SK_MAX); policies_[sk] = std::move(p);
}
inline ValuePolicy& ValuePolicyRegistry::of(uint16_t sk) {
    assert(sk < SK_MAX && policies_[sk] && "no ValuePolicy registered for this state_kind");
    return *policies_[sk];
}
inline bool ValuePolicyRegistry::has(uint16_t sk) const { return sk < SK_MAX && policies_[sk] != nullptr; }
```

- [ ] **Step 4: Wire target + run** — add `test_value_policy_registry` to `src/tests/unit/CMakeLists.txt`. Run — PASS.

- [ ] **Step 5: Commit**
```bash
git add src/core/common/value_policy.h src/tests/unit/policy/value_policy_registry_test.cpp src/tests/unit/CMakeLists.txt
git commit -m "feat(SS-2): ValuePolicy interface + state_kind registry (LLD §3.3.8)"
```

---

### Task 3: `ValuePolicyKv` — first-implement the economic `shouldStore` (+ evict/miss)

> **Framing note (important):** the store-vs-recompute decision (D-PERF-1 / `KV_E_SAFETY_NET`) is **defined but never produced** in the codebase today — `SealCommit` stores unconditionally. This task **first-implements** it behind the interface. `shouldStore` is behavior-preserving on the hot path: with the default `CostModel` the hot path supplies (Task 5 passes zeroed costs → "unknown"), it returns `true` (store), so existing behavior is unchanged; the economic logic is real and unit-tested here with synthetic costs.

**Files:**
- Create: `src/core/common/value_policy_kv.h`, `src/core/common/value_policy_kv.cpp`
- Test: `src/tests/unit/policy/value_policy_kv_test.cpp`
- Modify: `src/tests/unit/CMakeLists.txt`

**Interfaces:**
- Consumes: Task 2 `ValuePolicy`, `CostModel`, `EvictDecision`, `OnMissAction`; Task 1 `StateIdentity`.
- Produces: `class ValuePolicyKv final : public ValuePolicy` in `kvcache::common`.

- [ ] **Step 1: Write the failing test**

```cpp
// src/tests/unit/policy/value_policy_kv_test.cpp
#include "common/value_policy_kv.h"
#include <gtest/gtest.h>
using namespace kvcache::common;

TEST(ValuePolicyKv, ShouldStoreIsBehaviorPreservingOnUnknownCost) {
    ValuePolicyKv p;
    StateIdentity id{}; id.state_kind = SK_KV;
    // Hot-path default (no telemetry): both costs 0 / unknown → store (preserves today's behavior).
    EXPECT_TRUE(p.shouldStore(id, CostModel{}));
}

TEST(ValuePolicyKv, ShouldStoreDeclinesWhenRecomputeIsCheaper) {
    ValuePolicyKv p;
    StateIdentity id{}; id.state_kind = SK_KV;
    // D-PERF-1: if fetching the stored copy costs as much as (or more than) recomputing,
    // storing is not worth it → decline. (fetch >= recompute → false)
    EXPECT_FALSE(p.shouldStore(id, CostModel{.fetch_cost_ms = 10.0, .recompute_cost_ms = 5.0}));
    // fetch clearly cheaper than recompute → store.
    EXPECT_TRUE(p.shouldStore(id, CostModel{.fetch_cost_ms = 1.0, .recompute_cost_ms = 20.0}));
}

TEST(ValuePolicyKv, EvictableAndRecomputeOnMiss) {
    ValuePolicyKv p;
    StateIdentity id{}; id.state_kind = SK_KV;
    EXPECT_EQ(p.shouldEvict(id, /*tier*/2), EvictDecision::kEvictable);  // A-class: cost-evictable
    EXPECT_EQ(p.onMiss(id), OnMissAction::kRecompute);
}
```

- [ ] **Step 2: Run to verify fail** — `cmake --build build --target test_value_policy_kv && ./build/tests/unit/test_value_policy_kv` — FAIL.

- [ ] **Step 3: Implement**

```cpp
// src/core/common/value_policy_kv.h
#pragma once
#include "common/value_policy.h"
namespace kvcache::common {
class ValuePolicyKv final : public ValuePolicy {
 public:
  bool          shouldStore(const StateIdentity&, const CostModel&) override;
  EvictDecision shouldEvict(const StateIdentity&, int tier) override;
  OnMissAction  onMiss(const StateIdentity&) override;
};
}  // namespace kvcache::common
```
```cpp
// src/core/common/value_policy_kv.cpp
#include "common/value_policy_kv.h"
namespace kvcache::common {
// D-PERF-1 economic decision. Behavior-preserving when costs are unknown (both 0):
// storing is the default (returns true). When a real CostModel is supplied, decline to
// store iff fetching the stored copy is not cheaper than recomputing it.
bool ValuePolicyKv::shouldStore(const StateIdentity&, const CostModel& c) {
    if (c.recompute_cost_ms <= 0.0) return true;          // unknown recompute cost → store (safe default)
    return c.fetch_cost_ms < c.recompute_cost_ms;         // store only if fetch beats recompute
}
EvictDecision ValuePolicyKv::shouldEvict(const StateIdentity&, int /*tier*/) {
    return EvictDecision::kEvictable;                     // A-class: cost-driven eviction may proceed
}
OnMissAction ValuePolicyKv::onMiss(const StateIdentity&) {
    return OnMissAction::kRecompute;                      // engine recomputes on miss
}
}  // namespace kvcache::common
```

- [ ] **Step 4: Wire target + run** — add `test_value_policy_kv` (sources: `value_policy_kv.cpp` + test) to CMake. Run — PASS (3 tests).

- [ ] **Step 5: Commit**
```bash
git add src/core/common/value_policy_kv.h src/core/common/value_policy_kv.cpp \
        src/tests/unit/policy/value_policy_kv_test.cpp src/tests/unit/CMakeLists.txt
git commit -m "feat(SS-2): ValuePolicyKv — first-implement D-PERF-1 economic shouldStore (behavior-preserving default)"
```

---

### Task 4: `ValuePolicyPersistentStub` (B-stub) + A/B contrast test — **the Q3 core**

**Files:**
- Create: `src/core/common/value_policy_persistent_stub.h`
- Test: `src/tests/unit/policy/value_policy_ab_contrast_test.cpp`
- Modify: `src/tests/unit/CMakeLists.txt`

**Interfaces:**
- Consumes: Task 2 `ValuePolicy`/registry, Task 3 `ValuePolicyKv`.
- Produces: `class ValuePolicyPersistentStub final : public ValuePolicy` — B-class semantics: unconditional store, NOT_EVICTABLE, replay-from-persist onMiss (stub, no ⑬ backing).

- [ ] **Step 1: Write the failing test (the Q3 validation)**

```cpp
// src/tests/unit/policy/value_policy_ab_contrast_test.cpp
#include "common/value_policy.h"
#include "common/value_policy_kv.h"
#include "common/value_policy_persistent_stub.h"
#include <gtest/gtest.h>
using namespace kvcache::common;

// Q3: one interface + one registry admits two OPPOSITE-semantics policies, registered by one
// line each, with zero spine change. Same StateIdentity content, different state_kind → opposite
// store/evict/miss decisions — proving "one spine, swap plugin, covers A and B".
TEST(ValuePolicyABContrast, OneRegistryCoversAandBWithOppositeSemantics) {
    ValuePolicyRegistry reg;
    reg.registerPolicy(SK_KV,     std::make_unique<ValuePolicyKv>());              // A
    reg.registerPolicy(SK_MEMORY, std::make_unique<ValuePolicyPersistentStub>());  // B

    StateIdentity a{}; a.state_kind = SK_KV;
    StateIdentity b{}; b.state_kind = SK_MEMORY; b.flags = SIF_PERSISTENT_B;

    // shouldStore: A declines when recompute is cheaper; B stores unconditionally.
    EXPECT_FALSE(reg.of(SK_KV).shouldStore(a, CostModel{.fetch_cost_ms=10, .recompute_cost_ms=1}));
    EXPECT_TRUE (reg.of(SK_MEMORY).shouldStore(b, CostModel{.fetch_cost_ms=10, .recompute_cost_ms=1}))
        << "B is irreplaceable — stored regardless of economics";

    // shouldEvict: A is cost-evictable; B is NOT_EVICTABLE (demote-only).
    EXPECT_EQ(reg.of(SK_KV).shouldEvict(a, 2),     EvictDecision::kEvictable);
    EXPECT_EQ(reg.of(SK_MEMORY).shouldEvict(b, 2), EvictDecision::kNotEvictable);

    // onMiss: A recomputes; B replays from persistence.
    EXPECT_EQ(reg.of(SK_KV).onMiss(a),     OnMissAction::kRecompute);
    EXPECT_EQ(reg.of(SK_MEMORY).onMiss(b), OnMissAction::kReplayFromPersist);
}
```

- [ ] **Step 2: Run to verify fail** — `cmake --build build --target test_value_policy_ab_contrast && ./build/tests/unit/test_value_policy_ab_contrast` — FAIL (stub header missing).

- [ ] **Step 3: Implement the B-stub**
```cpp
// src/core/common/value_policy_persistent_stub.h
#pragma once
#include "common/value_policy.h"
namespace kvcache::common {
// B-class stub: proves the ValuePolicy interface admits persistence semantics OPPOSITE to KV,
// WITHOUT building the ⑬ persistence engine. Not for production — onMiss's replay is a stub.
class ValuePolicyPersistentStub final : public ValuePolicy {
 public:
  bool shouldStore(const StateIdentity&, const CostModel&) override { return true; } // irreplaceable
  EvictDecision shouldEvict(const StateIdentity&, int) override { return EvictDecision::kNotEvictable; }
  OnMissAction onMiss(const StateIdentity&) override { return OnMissAction::kReplayFromPersist; }
};
}  // namespace kvcache::common
```

- [ ] **Step 4: Wire target + run** — add `test_value_policy_ab_contrast` (sources: `value_policy_kv.cpp` + test; stub is header-only) to CMake. Run — PASS. **This green is the Q3 pass signal at the policy layer.**

- [ ] **Step 5: Commit**
```bash
git add src/core/common/value_policy_persistent_stub.h \
        src/tests/unit/policy/value_policy_ab_contrast_test.cpp src/tests/unit/CMakeLists.txt
git commit -m "feat(SS-2): B-stub ValuePolicy + A/B contrast test — validates Q3 at the policy layer"
```

---

### Task 5: Wire the registry into the hot path (behavior-preserving) + full regression

> This is the delicate task: it inserts the interface seam at the three real decision sites so the hot path calls `registry.of(kind).<method>` instead of inline logic — **without changing behavior** (517 green). This is where "no scattered `if state_kind`" is proven on real code.

**Files:**
- Modify: `src/core-abi/src/headless_node.h` / `.cpp` (own a `ValuePolicyRegistry`; route store + miss)
- Modify: `src/kvstore-node/src/tier/dram_tier.h` / `.cpp` (route evict through an injected `ValuePolicy`)
- Test: extend the full suite; add one hot-path seam test (below)
- Modify: `src/kvstore-node/CMakeLists.txt` / `src/core-abi/CMakeLists.txt` if the new `value_policy_kv.cpp` must link into those libs.

**Interfaces:**
- Consumes: Tasks 1–3 (`StateIdentity`, `ValuePolicyRegistry`, `ValuePolicyKv`, `CostModel`).
- Produces: `HeadlessNode` gains a private `ValuePolicyRegistry policy_reg_;` with `SK_KV → ValuePolicyKv` registered in its constructor. `DramTier::EvictToFit` consults a `ValuePolicy*` (injected via `DramTier` ctor/opts, defaulting to a KV policy) before evicting.

- [ ] **Step 1: Ground the three sites, then write the seam test**

Read first (do NOT guess): `HeadlessNode::SealCommit` (`src/core-abi/src/headless_node.cpp:301-411`, the unconditional store), `HeadlessNode::FetchWithPriority` (`:476-479`, `return KV_E_NOT_FOUND`) and `Lookup` (`:291-341`), and `DramTier::EvictToFit` (`src/kvstore-node/src/tier/dram_tier.cpp:138-176`). Confirm exact signatures + how `HeadlessNode` constructs `DramTier`/`TierManager` (to know where to register the policy + inject it into the evictor).

```cpp
// Add to an existing headless_node unit test file (or a new src/tests/unit/policy/hot_path_seam_test.cpp):
// After a normal Reserve→Publish→Seal, the chunk is still stored (shouldStore returned true on the
// hot path's default CostModel) and Lookup finds it — proving the seam is behavior-preserving.
TEST(HotPathSeam, SealStillStoresAndLookupHitsThroughPolicySeam) {
    /* Reserve/Publish/Seal a chunk on a HeadlessNode as existing seal tests do,
       then Lookup the same tokens and assert a hit + full match count. */
}
```

- [ ] **Step 2: Run to verify fail** — build & run — FAIL until the seam compiles/links (or trivially passes if you assert new behavior not yet wired; the real signal is Step 4's full-suite green after wiring).

- [ ] **Step 3: Insert the store seam (behavior-preserving) in `SealCommit`**

In `headless_node.cpp` `SealCommit`, before the unconditional `StageToDram`, build the KV identity + a default CostModel and consult the policy:
```cpp
// build the projected identity for this locator (KV)
kvcache::common::StateIdentity sid = kvcache::common::StateIdentityFromLocator(st.locator);
kvcache::common::CostModel cost{};   // no telemetry yet → {0,0}: shouldStore returns true (preserves behavior)
if (!policy_reg_.of(kvcache::common::SK_KV).shouldStore(sid, cost)) {
    // Economic decline path (never taken today: cost is unknown → store). Wired for future activation.
    // Release the reserved slot without staging, return KV_E_SAFETY_NET.
    // (Mirror SealCommit's existing cleanup tail: wm_->Drop / buffers_->Release / handles_.erase.)
    return KV_E_SAFETY_NET;
}
// ... existing unconditional StageToDram + ART/RocksDB commit unchanged ...
```
*(The decline branch is unreachable with `{0,0}` cost, so behavior is preserved and 517 stay green; but it is real, compiled, and correct — activation is a follow-on when cost telemetry exists. Reuse SealCommit's existing cleanup tail for the decline path so no slot leaks.)*

- [ ] **Step 4: Insert the evict + miss seams; register the policy; full regression**

- `HeadlessNode` ctor: `policy_reg_.registerPolicy(SK_KV, std::make_unique<ValuePolicyKv>());`
- `DramTier`: add an optional `const ValuePolicy* evict_policy_` (injected; default a static KV policy). In `EvictToFit`, before evicting a chosen victim, `if (evict_policy_ && evict_policy_->shouldEvict(sid, tier) == EvictDecision::kNotEvictable) skip;` — for KV it returns `kEvictable`, so the SAME victims are evicted as today (behavior-preserving); the seam proves the evictor asks the policy. *(The victim's `StateIdentity` is projected from its stored locator; if `DramTier` entries don't carry a locator, inject `SK_KV` as the kind for this spike and note it — the KV policy is kind-agnostic in `shouldEvict`.)*
- Miss path: at the two `return KV_E_NOT_FOUND` sites, consult `policy_reg_.of(SK_KV).onMiss(sid)` — for KV it returns `kRecompute`, and the return code stays `KV_E_NOT_FOUND` (engine recomputes). The call documents the seam; behavior is identical.
- Run the FULL suite: `cd build && cmake --build . -j4 && ctest --output-on-failure` — **all prior tests + the new ones green, 0 failures.** Capture the summary line. If any pre-existing test flips, the seam changed behavior — fix until identical.

- [ ] **Step 5: Commit**
```bash
git add src/core-abi/src/headless_node.h src/core-abi/src/headless_node.cpp \
        src/kvstore-node/src/tier/dram_tier.h src/kvstore-node/src/tier/dram_tier.cpp \
        src/tests/unit/policy/hot_path_seam_test.cpp src/tests/unit/CMakeLists.txt \
        src/core-abi/CMakeLists.txt src/kvstore-node/CMakeLists.txt
git commit -m "feat(SS-2): route store/evict/miss through ValuePolicy registry — seam inserted, behavior preserved, full suite green"
```

---

### Task 6: Q3 verdict + SS-3 honesty note + final full-suite

**Files:**
- Create: `docs/design/ss2-spike-q3-verdict.md`
- Modify: `README.md` (correct any "D-PERF-1 safety-net shipped" wording — SS-3 honesty)

- [ ] **Step 1: Write the Q3 verdict doc** — `docs/design/ss2-spike-q3-verdict.md` recording: (a) the pass criteria and the evidence — full suite green (cite the count), the A/B contrast test green (one registry, two opposite-semantics policies, one-line registration each, zero spine change), and no `if state_kind` on the hot path (the three sites call `registry.of(kind)`); (b) the **honest finding** that `shouldStore` (D-PERF-1) was defined-but-never-produced and is **first-implemented** here, behavior-preserving, with production activation gated on cost telemetry (follow-on); (c) the explicit deferrals (promotion channel §3.3.8.1, other state_kinds, real ⑬, non-KV BLAKE3).

- [ ] **Step 2: Correct SS-3 honesty wording in `README.md`** — grep the repo README for any claim that the store-vs-recompute safety net / D-PERF-1 is a shipped runtime feature; reword to "economic `shouldStore` implemented behind ValuePolicy (SS-2); production activation pending cost telemetry." Do not overstate. (The strategy-doc capability matrix in `KV_Cache_Design/` is corrected separately by the maintainer — out of this repo.)

- [ ] **Step 3: Final full regression** — `cd build && cmake --build . -j4 && ctest --output-on-failure`; capture the summary line into the verdict doc.

- [ ] **Step 4: Commit**
```bash
git add docs/design/ss2-spike-q3-verdict.md README.md
git commit -m "docs(SS-2): Q3 verdict + SS-3 honesty note (D-PERF-1 first-implemented, not pre-existing)"
```

---

### Deferred / follow-on (NOT in this spike)

- §3.3.8.1 A→B controlled promotion (`SIF_PERSISTENT` on high-reuse A blocks; θ standardization is 未决 #31).
- Production activation of `shouldStore` — needs real fetch/recompute cost telemetry feeding `CostModel`.
- Other A-class `state_kind` connectors (sandbox/embedding/tool-result) — that is the A-plane generalization proper (Phase 2).
- The real ⑬ persistence engine + ⑭ lineage engine (B-plane, Phase 3); the B-stub's `onMiss` replay is a placeholder.
- BLAKE3 `content_hash` for non-KV state_kinds (KV keeps its 64B xxhash3 fast path unchanged).
- Threading `StateIdentity` across the FFI/proto/wire boundary (this spike keeps it C++-core-only).

---

## Self-Review

**Spec coverage (LLD SS-2 / §2.1b / §3.3.8):** `state_identity_t` 128B + KV projection → Task 1. `ValuePolicy{shouldStore,shouldEvict,onMiss}` + `ValuePolicyRegistry.of(state_kind)` → Task 2. `ValuePolicy_KV` (D-PERF-1 = shouldStore) → Task 3. A/B coverage proof (the SS-2 thesis "one spine, swap plugin") → Task 4 (policy layer) + Task 5 (hot-path seam). "No scattered if state_kind" + 517 green → Task 5. Q3 verdict + SS-3 honesty → Task 6. **Deferred items match the agreed scope** (promotion channel, other state_kinds, real ⑬, production activation).

**Placeholder scan:** every code step carries real code; the "read/verify against actual X" notes (kv_range_t size, SealCommit cleanup tail, DramTier ctor) are grounding instructions (the pattern used successfully on prior plans), not TBDs. No "implement later" in a deliverable. Task 5 Step 1's seam-test body references "as existing seal tests do" — the implementer copies the real Reserve/Publish/Seal setup from `seal`/`node_data` tests; that is a concrete pointer, not a placeholder.

**Type consistency:** `StateIdentity`/`state_kind_e`/`si_flags_e` (Task 1) consumed unchanged by Tasks 2–5. `ValuePolicy` 3-method signature + `CostModel{fetch_cost_ms,recompute_cost_ms,reuse_hint}` + `EvictDecision{kEvictable,kNotEvictable}` + `OnMissAction{kRecompute,kReplayFromPersist,kNone}` (Task 2) used identically by `ValuePolicyKv` (Task 3), `ValuePolicyPersistentStub` (Task 4), and the hot-path seams (Task 5). `ValuePolicyRegistry.registerPolicy/of/has` consistent across Tasks 2/4/5. `StateIdentityFromLocator` (Task 1) called in Task 5's seams.

**Behavior-preservation invariant (the spike's spine):** every hot-path seam in Task 5 is provably behavior-identical — `shouldStore` with `{0,0}` cost returns true (stores), `shouldEvict` for KV returns `kEvictable` (same victims), `onMiss` returns `kRecompute` (same `KV_E_NOT_FOUND`). The 517-green gate in Task 5 Step 4 is the enforcement.
