# B-plane DRAM Ingest + Minimal ⑬ Persistence Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A B-class state (`SK_MEMORY`) can be stored (WAL-first, durable), staged into DRAM, served, reclaimed from DRAM under pressure, and replayed back — end-to-end, single node, reusing the A-plane tiering spine.

**Architecture:** New `StateWal` append-only persistence engine (⑬ v0) reusing ArtWal's on-disk discipline. New `HeadlessNode::StatePut/StateGet` C++-core entry points that write the WAL before staging to DRAM, so the DRAM copy is always backed by a durable copy and can be safely evicted (demotion, not discard); a DRAM miss replays from the WAL. A new production B policy `ValuePolicyPersistentWal` makes B `kEvictable` (persist-first) + `kReplayFromPersist`.

**Tech Stack:** C++20, CMake + Ninja, GoogleTest. POSIX `open/write/fsync/ftruncate`.

## Global Constraints

- `kv_locator_t` is frozen at 64 bytes; `StateIdentity` is 128B, C++-core-only, additive only, NEVER on the wire in this slice.
- Include convention: core headers include as `"foo.h"` (the `src/core/common` and node `src/` roots are on the test include path).
- KV path must stay behavior-identical; B is purely additive. Full suite currently 542/542 (15 pre-existing skips) — must stay green.
- No `Co-Authored-By` trailer on commits. `main` is non-forked → push only on explicit user confirmation (the controller handles the batched push at the end, not per task).
- "Durable" in this slice = survives process restart, NOT node loss. No replication / strong consistency / lineage / FFI-wire (all deferred).
- Existing enum values to use verbatim (`src/core/common/state_identity.h`): `SK_KV = 0`, `SK_MEMORY = 16`. Error codes (`src/include/kvcache/kv_errors.h`): `KV_OK = 0`, `KV_E_NOT_FOUND = -3`, `KV_E_TIER_DOWN = -8`.

---

### Task 1: `StateWal` — minimal ⑬ persistence engine

**Files:**
- Create: `src/kvstore-node/src/persist/state_wal.h`
- Create: `src/kvstore-node/src/persist/state_wal.cpp`
- Test: `src/tests/unit/persist/state_wal_test.cpp`
- Modify: `src/kvstore-node/CMakeLists.txt` (add source to the node core lib)
- Modify: `src/tests/unit/CMakeLists.txt` (add `NODE_PERSIST_SRCS` + test target)

**Interfaces:**
- Consumes: `kvcache::node::tier::DramKey` (from `tier/dram_tier.h` — a `struct { std::array<uint8_t,16> bytes; }`), `kvcache::common::StateIdentity` (from `state_identity.h`, 128 bytes).
- Produces:
  - `kvcache::node::persist::StateWal` with:
    - `static std::unique_ptr<StateWal> Open(const std::string& path, std::string* err);`
    - `bool Append(const tier::DramKey& key, const common::StateIdentity& id, const uint8_t* data, std::size_t n, std::string* err);`
    - `bool Get(const tier::DramKey& key, std::vector<uint8_t>* out) const;`
    - `std::size_t Size() const;`

**On-disk record** (little-endian, packed; one per Append):
```
record_len  u32     total bytes of this record INCLUDING these 4 bytes
op          u8      1 = PUT (only value emitted this slice; 2 = DEL reserved)
key         u8[16]  DramKey.bytes
identity    u8[128] StateIdentity (opaque bytes; stored for future lineage/debug)
blob_len    u32     length of blob
blob        u8[blob_len]
crc32       u32     IEEE 802.3 poly over [op .. last blob byte] (i.e. record_len and crc excluded)
```

- [ ] **Step 1: Write the failing test**

Create `src/tests/unit/persist/state_wal_test.cpp`:
```cpp
// Task 1 — StateWal: append-only B-state persistence (⑬ v0).
#include "persist/state_wal.h"

#include <gtest/gtest.h>
#include <cstdio>
#include <string>
#include <vector>

#include "state_identity.h"
#include "tier/dram_tier.h"

using kvcache::common::SK_MEMORY;
using kvcache::common::StateIdentity;
using kvcache::node::persist::StateWal;
using kvcache::node::tier::DramKey;

namespace {
// Unique temp path per test; removed in the fixture teardown.
std::string TempWalPath(const char* tag) {
    return std::string("state_wal_test_") + tag + ".wal";
}
DramKey MakeKey(uint8_t seed) {
    DramKey k{};
    k.bytes[0] = seed;
    return k;
}
StateIdentity MakeId(uint8_t seed) {
    StateIdentity id{};
    id.version    = 2;
    id.state_kind = SK_MEMORY;
    id.content_hash[0] = seed;
    return id;
}
}  // namespace

TEST(StateWal, AppendThenGetRoundTrips) {
    const std::string path = TempWalPath("roundtrip");
    std::remove(path.c_str());

    std::string err;
    auto wal = StateWal::Open(path, &err);
    ASSERT_NE(wal, nullptr) << err;

    std::vector<uint8_t> blob(64, 0xB1);
    ASSERT_TRUE(wal->Append(MakeKey(1), MakeId(1), blob.data(), blob.size(), &err)) << err;

    std::vector<uint8_t> out;
    ASSERT_TRUE(wal->Get(MakeKey(1), &out));
    EXPECT_EQ(out, blob);
    EXPECT_FALSE(wal->Get(MakeKey(2), &out));  // absent key
    EXPECT_EQ(wal->Size(), 1u);

    std::remove(path.c_str());
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd build && cmake --build . -j4 --target test_state_wal`
Expected: FAIL — target does not exist yet / `persist/state_wal.h` not found. (You will add the CMake target in Step 5.)

- [ ] **Step 3: Write the header**

Create `src/kvstore-node/src/persist/state_wal.h`:
```cpp
// StateWal — append-only persistence engine for B-class state (⑬ v0).
//
// B-class state (Agent memory, execution state) is irreplaceable: unlike A/KV
// it cannot be recomputed, so it needs a durable ledger. This is the first,
// minimal cut of that ledger — a single append-only WAL file, fsynced on every
// append, replayed into an in-memory key→bytes map on Open. Single node, no
// replication / strong consistency / lineage (all deferred B-plane slices).
//
// On-disk record (little-endian, packed; one per Append):
//   record_len u32    total bytes INCLUDING this field
//   op         u8     1 = PUT (2 = DEL reserved, not emitted this slice)
//   key        u8[16] DramKey
//   identity   u8[128] StateIdentity (opaque; stored for future lineage/debug)
//   blob_len   u32
//   blob       u8[blob_len]
//   crc32      u32    IEEE poly over [op .. last blob byte]
//
// A torn tail record (short read or CRC/len mismatch) is truncated on Open and
// every fully-committed record before it is kept — same discipline as ArtWal.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "state_identity.h"
#include "tier/dram_tier.h"

namespace kvcache::node::persist {

class StateWal {
   public:
    // Open (create if absent) at `path`, replaying existing records into the
    // in-memory map. Missing file is fine. A torn tail is truncated. Only a
    // hard error (cannot open/create the file) returns nullptr with *err set.
    static std::unique_ptr<StateWal> Open(const std::string& path, std::string* err);

    ~StateWal();
    StateWal(const StateWal&)            = delete;
    StateWal& operator=(const StateWal&) = delete;

    // Append a PUT record, fsync, then update the in-memory map. Returns false
    // (and sets *err) if the write or fsync fails; on failure the map is NOT
    // updated (caller must treat the entry as not persisted).
    bool Append(const tier::DramKey& key, const common::StateIdentity& id,
                const uint8_t* data, std::size_t n, std::string* err);

    // Latest blob for `key`. Returns false if absent.
    bool Get(const tier::DramKey& key, std::vector<uint8_t>* out) const;

    std::size_t Size() const;

   private:
    StateWal() = default;

    int                                                          fd_ = -1;
    mutable std::mutex                                           mu_;
    std::unordered_map<tier::DramKey, std::vector<uint8_t>,
                       tier::DramKeyHash>                        map_;
};

}  // namespace kvcache::node::persist
```

- [ ] **Step 4: Write the implementation**

Create `src/kvstore-node/src/persist/state_wal.cpp`:
```cpp
#include "persist/state_wal.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace kvcache::node::persist {
namespace {

// CRC32 (IEEE 802.3), software fallback — local copy of the helper in
// prefix/art_wal.cpp. Kept local to keep this unit self-contained; a shared
// checksum header is a future consolidation, out of scope for this slice.
uint32_t Crc32(const uint8_t* p, std::size_t n) {
    static uint32_t table[256];
    static bool inited = false;
    if (!inited) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & -(c & 1));
            table[i] = c;
        }
        inited = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < n; ++i)
        c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

constexpr uint8_t  kOpPut     = 1;
constexpr std::size_t kKeyLen = 16;
constexpr std::size_t kIdLen  = sizeof(common::StateIdentity);  // 128

// Little-endian append helpers.
void PutU32(std::vector<uint8_t>& b, uint32_t v) {
    b.push_back(static_cast<uint8_t>(v & 0xff));
    b.push_back(static_cast<uint8_t>((v >> 8) & 0xff));
    b.push_back(static_cast<uint8_t>((v >> 16) & 0xff));
    b.push_back(static_cast<uint8_t>((v >> 24) & 0xff));
}
uint32_t GetU32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

}  // namespace

std::unique_ptr<StateWal> StateWal::Open(const std::string& path, std::string* err) {
    auto self = std::unique_ptr<StateWal>(new StateWal());

    // Replay existing records first (read-only pass over the whole file).
    int rfd = ::open(path.c_str(), O_RDONLY);
    if (rfd >= 0) {
        std::vector<uint8_t> buf;
        // Read the whole file into memory (v0 — bounded-memory replay deferred).
        uint8_t chunk[65536];
        ssize_t r;
        while ((r = ::read(rfd, chunk, sizeof(chunk))) > 0)
            buf.insert(buf.end(), chunk, chunk + r);
        ::close(rfd);

        std::size_t off = 0;
        std::size_t good_end = 0;  // byte offset past the last fully-valid record
        while (off + 4 <= buf.size()) {
            const uint32_t rec_len = GetU32(buf.data() + off);
            if (rec_len < 4 + 1 + kKeyLen + kIdLen + 4 + 4) break;  // impossibly short → torn
            if (off + rec_len > buf.size()) break;                   // short tail → torn
            const uint8_t* body     = buf.data() + off + 4;          // [op .. blob]
            const std::size_t body_len = rec_len - 4 - 4;            // exclude len + crc
            const uint32_t want_crc = GetU32(buf.data() + off + rec_len - 4);
            if (Crc32(body, body_len) != want_crc) break;            // corrupt → torn

            const uint8_t op = body[0];
            if (op == kOpPut) {
                const uint8_t* kp = body + 1;
                const uint8_t* bl = kp + kKeyLen + kIdLen;
                const uint32_t blob_len = GetU32(bl);
                const uint8_t* blob = bl + 4;
                tier::DramKey key{};
                std::memcpy(key.bytes.data(), kp, kKeyLen);
                self->map_[key].assign(blob, blob + blob_len);
            }
            off      += rec_len;
            good_end  = off;
        }
        // Truncate any torn tail so future appends start from a clean boundary.
        if (good_end < buf.size()) {
            if (::truncate(path.c_str(), static_cast<off_t>(good_end)) != 0) {
                if (err) *err = std::string("state_wal truncate torn tail: ") + std::strerror(errno);
                return nullptr;
            }
        }
    } else if (errno != ENOENT) {
        if (err) *err = std::string("state_wal open(read) ") + path + ": " + std::strerror(errno);
        return nullptr;
    }

    // Open the append fd for subsequent writes.
    self->fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (self->fd_ < 0) {
        if (err) *err = std::string("state_wal open(append) ") + path + ": " + std::strerror(errno);
        return nullptr;
    }
    return self;
}

StateWal::~StateWal() {
    if (fd_ >= 0) ::close(fd_);
}

bool StateWal::Append(const tier::DramKey& key, const common::StateIdentity& id,
                      const uint8_t* data, std::size_t n, std::string* err) {
    std::lock_guard<std::mutex> lk(mu_);

    const uint32_t rec_len = static_cast<uint32_t>(4 + 1 + kKeyLen + kIdLen + 4 + n + 4);
    std::vector<uint8_t> rec;
    rec.reserve(rec_len);
    PutU32(rec, rec_len);
    rec.push_back(kOpPut);
    rec.insert(rec.end(), key.bytes.begin(), key.bytes.end());
    const uint8_t* idp = reinterpret_cast<const uint8_t*>(&id);
    rec.insert(rec.end(), idp, idp + kIdLen);
    PutU32(rec, static_cast<uint32_t>(n));
    rec.insert(rec.end(), data, data + n);
    const uint32_t crc = Crc32(rec.data() + 4, rec.size() - 4);  // [op .. blob]
    PutU32(rec, crc);

    const ssize_t w = ::write(fd_, rec.data(), rec.size());
    if (w != static_cast<ssize_t>(rec.size())) {
        if (err) *err = std::string("state_wal write: ") + std::strerror(errno);
        return false;
    }
    if (::fsync(fd_) != 0) {
        if (err) *err = std::string("state_wal fsync: ") + std::strerror(errno);
        return false;
    }
    map_[key].assign(data, data + n);  // durable now → safe to index
    return true;
}

bool StateWal::Get(const tier::DramKey& key, std::vector<uint8_t>* out) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = map_.find(key);
    if (it == map_.end()) return false;
    *out = it->second;
    return true;
}

std::size_t StateWal::Size() const {
    std::lock_guard<std::mutex> lk(mu_);
    return map_.size();
}

}  // namespace kvcache::node::persist
```

- [ ] **Step 5: Wire CMake (node lib + test target)**

In `src/kvstore-node/CMakeLists.txt`, add to the node core lib source list (alongside the `src/prefix/art_wal.cpp` line):
```cmake
    src/persist/state_wal.cpp
```

In `src/tests/unit/CMakeLists.txt`, add a source group after the `NODE_META_SRCS` block:
```cmake
set(NODE_PERSIST_SRCS
    ${KVCACHE_NODE_SRC}/persist/state_wal.cpp
)
```
and add the test target near the other node-tier tests (after `test_art_wal`):
```cmake
kvcache_add_test(test_state_wal        persist/state_wal_test.cpp       ${NODE_PERSIST_SRCS})
```

- [ ] **Step 6: Run the round-trip test to verify it passes**

Run: `cd build && cmake --build . -j4 --target test_state_wal && ctest -R StateWal --output-on-failure`
Expected: `AppendThenGetRoundTrips` PASS.

- [ ] **Step 7: Add restart-replay + torn-tail tests**

Append to `src/tests/unit/persist/state_wal_test.cpp`:
```cpp
TEST(StateWal, ReopenReplaysCommittedRecords) {
    const std::string path = TempWalPath("replay");
    std::remove(path.c_str());
    std::vector<uint8_t> a(16, 0xAA), b(32, 0xBB);
    {
        std::string err;
        auto wal = StateWal::Open(path, &err);
        ASSERT_NE(wal, nullptr) << err;
        ASSERT_TRUE(wal->Append(MakeKey(1), MakeId(1), a.data(), a.size(), &err)) << err;
        ASSERT_TRUE(wal->Append(MakeKey(2), MakeId(2), b.data(), b.size(), &err)) << err;
    }  // wal closed → simulates process exit
    std::string err;
    auto wal2 = StateWal::Open(path, &err);
    ASSERT_NE(wal2, nullptr) << err;
    EXPECT_EQ(wal2->Size(), 2u);
    std::vector<uint8_t> out;
    ASSERT_TRUE(wal2->Get(MakeKey(1), &out)); EXPECT_EQ(out, a);
    ASSERT_TRUE(wal2->Get(MakeKey(2), &out)); EXPECT_EQ(out, b);
    std::remove(path.c_str());
}

TEST(StateWal, TornTailRecordIsTruncatedOnReplay) {
    const std::string path = TempWalPath("torn");
    std::remove(path.c_str());
    std::vector<uint8_t> a(16, 0xAA);
    {
        std::string err;
        auto wal = StateWal::Open(path, &err);
        ASSERT_NE(wal, nullptr) << err;
        ASSERT_TRUE(wal->Append(MakeKey(1), MakeId(1), a.data(), a.size(), &err)) << err;
    }
    // Corrupt the file by appending a garbage partial record (a bogus length
    // prefix claiming more bytes than follow).
    int fd = ::open(path.c_str(), O_WRONLY | O_APPEND);
    ASSERT_GE(fd, 0);
    const uint8_t junk[8] = {0xFF, 0xFF, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04};
    ASSERT_EQ(::write(fd, junk, sizeof(junk)), static_cast<ssize_t>(sizeof(junk)));
    ::close(fd);

    std::string err;
    auto wal2 = StateWal::Open(path, &err);
    ASSERT_NE(wal2, nullptr) << err;
    EXPECT_EQ(wal2->Size(), 1u);              // torn tail dropped, first record kept
    std::vector<uint8_t> out;
    ASSERT_TRUE(wal2->Get(MakeKey(1), &out)); EXPECT_EQ(out, a);
    // A fresh append after truncation must still round-trip.
    std::vector<uint8_t> c(8, 0xCC);
    ASSERT_TRUE(wal2->Append(MakeKey(3), MakeId(3), c.data(), c.size(), &err)) << err;
    ASSERT_TRUE(wal2->Get(MakeKey(3), &out)); EXPECT_EQ(out, c);
    std::remove(path.c_str());
}
```
Add `#include <fcntl.h>` and `#include <unistd.h>` to the test's includes for the torn-tail test.

- [ ] **Step 8: Run all StateWal tests**

Run: `cd build && cmake --build . -j4 --target test_state_wal && ctest -R StateWal --output-on-failure`
Expected: 3/3 PASS.

- [ ] **Step 9: Commit**
```bash
git add src/kvstore-node/src/persist/state_wal.h src/kvstore-node/src/persist/state_wal.cpp \
        src/tests/unit/persist/state_wal_test.cpp \
        src/kvstore-node/CMakeLists.txt src/tests/unit/CMakeLists.txt
git commit -m "feat(b-ingest): StateWal — append-only B-state persistence engine (13 v0)"
```

---

### Task 2: `StateKeyFromIdentity` + `ValuePolicyPersistentWal`

**Files:**
- Modify: `src/core/common/state_identity.h` (add inline `StateKeyFromIdentity` returning a 16-byte key as `std::array<uint8_t,16>` — keeps `state_identity.h` free of a node dependency)
- Create: `src/core/common/value_policy_persistent_wal.h`
- Create: `src/core/common/value_policy_persistent_wal.cpp`
- Modify: `src/core/common/CMakeLists.txt` (add the `.cpp` to `kvcache_common`)
- Test: `src/tests/unit/policy/value_policy_persistent_wal_test.cpp`
- Modify: `src/tests/unit/CMakeLists.txt` (add test target)

**Interfaces:**
- Consumes: `kvcache::common::StateIdentity`, `kvcache::common::ValuePolicy` / `EvictDecision` / `OnMissAction` / `CostModel` (from `value_policy.h`), `SK_MEMORY`.
- Produces:
  - `std::array<uint8_t,16> kvcache::common::StateKeyBytesFromIdentity(const StateIdentity& id);` — first 16 bytes of `content_hash`. (Node code wraps this into a `DramKey`; see Task 3.)
  - `class kvcache::common::ValuePolicyPersistentWal : public ValuePolicy` with `shouldStore→true`, `shouldEvict→kEvictable`, `onMiss→kReplayFromPersist`.

> Why `state_identity.h` (core/common) cannot return a `DramKey`: `DramKey` lives in the node layer (`tier/dram_tier.h`), and `state_identity.h` must not depend on node code. So the pure projection returns raw 16 bytes; Task 3 copies them into a `DramKey`.

- [ ] **Step 1: Write the failing test**

Create `src/tests/unit/policy/value_policy_persistent_wal_test.cpp`:
```cpp
// Task 2 — the production B policy + key projection.
#include "value_policy_persistent_wal.h"

#include <gtest/gtest.h>
#include <array>

#include "state_identity.h"
#include "value_policy.h"
#include "value_policy_kv.h"

using kvcache::common::CostModel;
using kvcache::common::EvictDecision;
using kvcache::common::OnMissAction;
using kvcache::common::SK_MEMORY;
using kvcache::common::StateIdentity;
using kvcache::common::StateKeyBytesFromIdentity;
using kvcache::common::ValuePolicyKv;
using kvcache::common::ValuePolicyPersistentWal;

TEST(StateKeyBytes, TakesFirst16OfContentHash) {
    StateIdentity id{};
    for (int i = 0; i < 32; ++i) id.content_hash[i] = static_cast<uint8_t>(i + 1);
    auto k = StateKeyBytesFromIdentity(id);
    ASSERT_EQ(k.size(), 16u);
    for (int i = 0; i < 16; ++i) EXPECT_EQ(k[i], static_cast<uint8_t>(i + 1));
}

TEST(ValuePolicyPersistentWal, IsEvictableAndReplaysFromPersist) {
    ValuePolicyPersistentWal p;
    StateIdentity id{};
    id.state_kind = SK_MEMORY;
    CostModel cost{};
    EXPECT_TRUE(p.shouldStore(id, cost));                                  // irreplaceable → always store
    EXPECT_EQ(p.shouldEvict(id, /*tier=*/0), EvictDecision::kEvictable);   // persist-first → demotable
    EXPECT_EQ(p.onMiss(id), OnMissAction::kReplayFromPersist);

    // Contrast with KV: opposite miss action, evictable both but for different reasons.
    ValuePolicyKv kv;
    EXPECT_EQ(kv.onMiss(id), OnMissAction::kRecompute);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd build && cmake --build . -j4 --target test_value_policy_persistent_wal`
Expected: FAIL — `value_policy_persistent_wal.h` not found / target missing.

- [ ] **Step 3: Add the key projection to `state_identity.h`**

In `src/core/common/state_identity.h`, add after `StateIdentityFromLocator` (and its `#include <array>` at top if absent — add `#include <array>`):
```cpp
// Project a StateIdentity to a 16-byte content-addressed key (first 16 bytes
// of content_hash). Node code copies this into a tier::DramKey. Mirrors the KV
// LocatorContentKey projection but for the generalized identity.
inline std::array<uint8_t, 16> StateKeyBytesFromIdentity(const StateIdentity& id) {
    std::array<uint8_t, 16> k{};
    std::memcpy(k.data(), id.content_hash, 16);
    return k;
}
```

- [ ] **Step 4: Write the policy header + impl**

Create `src/core/common/value_policy_persistent_wal.h`:
```cpp
// ValuePolicyPersistentWal — production B-class value policy (SK_MEMORY).
//
// Replaces ValuePolicyPersistentStub on a real node once the B-state WAL
// exists. Because StatePut writes the WAL (fsync) BEFORE staging to DRAM,
// every B entry resident in DRAM already has a durable copy, so its DRAM copy
// is safely reclaimable — eviction here is DEMOTION, not discard. A DRAM miss
// is served by replaying from the WAL.
//
//   shouldStore : true        — irreplaceable, always stored (ignores CostModel)
//   shouldEvict : kEvictable  — persist-first ⇒ dropping the DRAM copy is safe
//   onMiss      : kReplayFromPersist — replay the durable copy from the WAL
//
// (ValuePolicyPersistentStub, kNotEvictable, is retained as the pinned-entry
// exemplar + DramTier eviction-skip test fixture.)
#pragma once

#include "value_policy.h"

namespace kvcache::common {

class ValuePolicyPersistentWal final : public ValuePolicy {
 public:
    bool shouldStore(const StateIdentity& /*id*/, const CostModel& /*cost*/) override {
        return true;
    }
    EvictDecision shouldEvict(const StateIdentity& /*id*/, int /*tier*/) override {
        return EvictDecision::kEvictable;
    }
    OnMissAction onMiss(const StateIdentity& /*id*/) override {
        return OnMissAction::kReplayFromPersist;
    }
};

}  // namespace kvcache::common
```

Create `src/core/common/value_policy_persistent_wal.cpp`:
```cpp
// All methods are inline in the header; this TU exists so the class has a
// translation unit in kvcache_common (matching the value_policy_kv.cpp /
// value_policy_tool_result.cpp pattern) and a home for future non-trivial
// logic (e.g. per-tenant persistence-SLA gating).
#include "value_policy_persistent_wal.h"

namespace kvcache::common {
// (intentionally empty — see header)
}  // namespace kvcache::common
```

- [ ] **Step 5: Wire CMake**

In `src/core/common/CMakeLists.txt`, add to the `kvcache_common` source list (after `value_policy_tool_result.cpp`):
```cmake
    value_policy_persistent_wal.cpp  # B-ingest — production SK_MEMORY policy
```

In `src/tests/unit/CMakeLists.txt`, add near the other `policy/` tests (find `hot_path_seam_test` or the `value_policy` tests and add alongside):
```cmake
kvcache_add_test(test_value_policy_persistent_wal policy/value_policy_persistent_wal_test.cpp)
```
(No extra sources needed — `kvcache_common` is linked by `kvcache_add_test`, and `StateKeyBytesFromIdentity` is header-inline.)

- [ ] **Step 6: Run the test to verify it passes**

Run: `cd build && cmake --build . -j4 --target test_value_policy_persistent_wal && ctest -R "StateKeyBytes|ValuePolicyPersistentWal" --output-on-failure`
Expected: 2/2 PASS.

- [ ] **Step 7: Commit**
```bash
git add src/core/common/state_identity.h \
        src/core/common/value_policy_persistent_wal.h src/core/common/value_policy_persistent_wal.cpp \
        src/core/common/CMakeLists.txt \
        src/tests/unit/policy/value_policy_persistent_wal_test.cpp src/tests/unit/CMakeLists.txt
git commit -m "feat(b-ingest): StateKeyBytesFromIdentity + ValuePolicyPersistentWal (SK_MEMORY, evictable+replay)"
```

---

### Task 3: `HeadlessNode::StatePut` / `StateGet` + wiring

**Files:**
- Modify: `src/kvstore-node/src/tier/tier_manager.h` (`StageToDram` gains `state_kind` param)
- Modify: `src/kvstore-node/src/tier/tier_manager.cpp` (forward the kind to `dram_->Insert`)
- Modify: `src/core-abi/src/headless_node.h` (Options field, `wal_` member, register B policy, `StatePut`/`StateGet` decls)
- Modify: `src/core-abi/src/headless_node.cpp` (Init opens `wal_`, `StatePut`/`StateGet` bodies, `StateKeyFromIdentity` helper)
- Test: `src/tests/unit/persist/state_ingest_test.cpp`
- Modify: `src/tests/unit/CMakeLists.txt` (add test target compiling `headless_node.cpp` directly)

**Interfaces:**
- Consumes: `StateWal` (Task 1: `Open`/`Append`/`Get`), `StateKeyBytesFromIdentity` + `ValuePolicyPersistentWal` (Task 2), existing `TierManager::StageToDram`/`LookupDram`, `policy_reg_`, `SK_MEMORY`, `KV_OK`/`KV_E_NOT_FOUND`/`KV_E_TIER_DOWN`.
- Produces:
  - `int HeadlessNode::StatePut(const common::StateIdentity& id, const uint8_t* data, std::size_t n);`
  - `int HeadlessNode::StateGet(const common::StateIdentity& id, std::vector<uint8_t>* out);`

- [ ] **Step 1: Thread `state_kind` through `StageToDram`**

In `src/kvstore-node/src/tier/tier_manager.h`, change the declaration:
```cpp
    void                    StageToDram(const DramKey& key, const uint8_t* data, std::size_t n,
                                        uint16_t state_kind = kvcache::common::SK_KV);
```
Add `#include "state_identity.h"` to `tier_manager.h` if `SK_KV` is not already visible (check includes; `dram_tier.h` already pulls `value_policy.h` which does not define `SK_KV` — add the include).

In `src/kvstore-node/src/tier/tier_manager.cpp`, update the definition:
```cpp
void TierManager::StageToDram(const DramKey& key, const uint8_t* data, std::size_t n,
                              uint16_t state_kind) {
    dram_->Insert(key, data, n, state_kind);
}
```
(The two promotion `Insert` sites at lines ~87 and ~118 stay unchanged — they are KV promotion, default `SK_KV`.)

- [ ] **Step 2: Verify KV regression compiles & passes (no behavior change)**

Run: `cd build && cmake --build . -j4 --target test_tier_manager && ctest -R TierManager --output-on-failure`
Expected: PASS — default arg keeps every existing caller identical.

- [ ] **Step 3: Write the failing integration test**

Create `src/tests/unit/persist/state_ingest_test.cpp`:
```cpp
// Task 3 — B-plane ingest end-to-end through HeadlessNode: StatePut/StateGet,
// WAL-first durability, DRAM demotion + replay.
#include "headless_node.h"

#include <gtest/gtest.h>
#include <cstdio>
#include <string>
#include <vector>

#include "kvcache/kv_errors.h"
#include "state_identity.h"

using kvcache::common::SK_MEMORY;
using kvcache::common::StateIdentity;

namespace {
StateIdentity BId(uint8_t seed) {
    StateIdentity id{};
    id.version    = 2;
    id.state_kind = SK_MEMORY;
    for (int i = 0; i < 16; ++i) id.content_hash[i] = static_cast<uint8_t>(seed + i);
    return id;
}

// Build an isolated node (its own singleton reset is not available; each test
// process gets one). Small DRAM capacity so we can force demotion.
kvcache::HeadlessNode::Options MakeOpts(const std::string& wal_path, uint64_t dram_cap) {
    kvcache::HeadlessNode::Options o{};
    o.tier.dram.capacity_bytes = dram_cap;
    o.state_wal_path           = wal_path;
    return o;
}
}  // namespace

TEST(StateIngest, PutThenGetServesFromDram) {
    const std::string wal = "state_ingest_put.wal";
    std::remove(wal.c_str());
    std::string err;
    auto* node = kvcache::HeadlessNode::GetOrCreate(MakeOpts(wal, 1u << 20), &err);
    ASSERT_NE(node, nullptr) << err;

    std::vector<uint8_t> blob(128, 0xB1);
    ASSERT_EQ(node->StatePut(BId(1), blob.data(), blob.size()), KV_OK);

    std::vector<uint8_t> out;
    ASSERT_EQ(node->StateGet(BId(1), &out), KV_OK);
    EXPECT_EQ(out, blob);
    EXPECT_EQ(node->StateGet(BId(99), &out), KV_E_NOT_FOUND);
    std::remove(wal.c_str());
}
```

> **Singleton note:** `HeadlessNode` is a process singleton (`GetOrCreate` returns `g_singleton` if set). GoogleTest runs each `TEST` in the same process, so **all tests in this file share one node** — the first `GetOrCreate` wins and later `MakeOpts` args are ignored. To keep the demotion test independent of DRAM capacity set by an earlier test, use a **distinct key set per test** and have the demotion test force eviction by staging enough bytes itself rather than relying on a specific capacity. (This matches how `replica_fetch_test.cpp` treats the singleton.)

- [ ] **Step 4: Run test to verify it fails**

Run: `cd build && cmake --build . -j4 --target test_state_ingest`
Expected: FAIL — `StatePut`/`StateGet`/`Options::state_wal_path` don't exist yet.

- [ ] **Step 5: Add Options field + `wal_` member + register B policy (header)**

In `src/core-abi/src/headless_node.h`:

Add the include near the top (with the other core/common includes):
```cpp
#include "persist/state_wal.h"   // B-ingest — StateWal (13 v0)
#include "value_policy_persistent_wal.h"  // B-ingest — SK_MEMORY policy
```
Add to the `Options` struct (near `art_wal_path`):
```cpp
        // B-ingest — path to the append-only B-state WAL. Empty ⇒ B ingest
        // disabled (StatePut returns KV_E_TIER_DOWN). Distinct from art_wal_path.
        std::string state_wal_path;
```
Register the B policy in the ctor (after the `SK_TOOL_RESULT` line):
```cpp
        policy_reg_.registerPolicy(
            kvcache::common::SK_MEMORY,
            std::make_unique<kvcache::common::ValuePolicyPersistentWal>());
```
Add the public method declarations (near `Seal`):
```cpp
    // B-plane ingest (C++-core; not on the C ABI/wire this slice). StatePut is
    // WAL-first then stages to DRAM; StateGet is DRAM-then-replay. Keys are
    // derived from id.content_hash[0:16). Returns KV_OK / KV_E_NOT_FOUND /
    // KV_E_TIER_DOWN (no WAL configured).
    int StatePut(const kvcache::common::StateIdentity& id,
                 const uint8_t* data, std::size_t n);
    int StateGet(const kvcache::common::StateIdentity& id,
                 std::vector<uint8_t>* out);
```
Add the member (near `art_wal_`):
```cpp
    std::unique_ptr<node::persist::StateWal>           wal_;   // B-ingest (13 v0)
```

- [ ] **Step 6: Open `wal_` in `Init`, add the key helper + method bodies (impl)**

In `src/core-abi/src/headless_node.cpp`:

In `Init(...)`, after the `art_wal_` open block (~line 264), add:
```cpp
    // B-ingest — open the B-state WAL if a path was configured.
    if (!opts.state_wal_path.empty()) {
        std::string wal_err;
        wal_ = node::persist::StateWal::Open(opts.state_wal_path, &wal_err);
        if (!wal_) {
            if (err) *err = "state_wal open failed: " + wal_err;
            return false;
        }
    }
```

Add a key helper next to `LocatorContentKey` (in the anonymous namespace, ~line 108):
```cpp
node::tier::DramKey StateKeyFromIdentity(const kvcache::common::StateIdentity& id) {
    node::tier::DramKey k{};
    auto bytes = kvcache::common::StateKeyBytesFromIdentity(id);  // first 16 of content_hash
    std::memcpy(k.bytes.data(), bytes.data(), 16);
    return k;
}
```

Add the method bodies (near `SealCommit`, after the Seal section):
```cpp
int HeadlessNode::StatePut(const kvcache::common::StateIdentity& id,
                           const uint8_t* data, std::size_t n) {
    if (!wal_) return KV_E_TIER_DOWN;         // B ingest requires a durable WAL
    if (data == nullptr && n != 0) return KV_E_INVAL;

    const auto key = StateKeyFromIdentity(id);

    // WAL-FIRST: the durable copy must exist before the DRAM copy, so a crash
    // can never leave a DRAM-only (unrecoverable) B entry.
    std::string werr;
    if (!wal_->Append(key, id, data, n, &werr)) {
        return KV_E_INTERNAL;                 // nothing staged — no DRAM-only entry
    }
    // Now cache in DRAM with the real kind so the evict seam dispatches the B
    // policy (evictable-because-persisted).
    tm_->StageToDram(key, data, n, kvcache::common::SK_MEMORY);
    return KV_OK;
}

int HeadlessNode::StateGet(const kvcache::common::StateIdentity& id,
                           std::vector<uint8_t>* out) {
    if (out == nullptr) return KV_E_INVAL;
    if (!wal_) return KV_E_TIER_DOWN;

    const auto key = StateKeyFromIdentity(id);

    // DRAM first.
    auto hit = tm_->LookupDram(key);
    if (hit.where == node::tier::DramTier::HitWhere::kA1in ||
        hit.where == node::tier::DramTier::HitWhere::kAm) {
        out->assign(hit.data, hit.data + hit.data_bytes);
        return KV_OK;
    }

    // Miss → consult the B policy. For SK_MEMORY this is kReplayFromPersist.
    const auto action = policy_reg_.of(kvcache::common::SK_MEMORY).onMiss(id);
    if (action == kvcache::common::OnMissAction::kReplayFromPersist) {
        if (wal_->Get(key, out)) {
            // Re-stage the replayed copy so subsequent reads hit DRAM again.
            tm_->StageToDram(key, out->data(), out->size(), kvcache::common::SK_MEMORY);
            return KV_OK;
        }
    }
    return KV_E_NOT_FOUND;
}
```

> `KV_E_INVAL` and `KV_E_INTERNAL` are existing codes (`kv_errors.h`: -1 and -99).

- [ ] **Step 7: Wire the integration test target (CMake)**

In `src/tests/unit/CMakeLists.txt`, add (mirroring `test_replica_fetch` at ~line 546 — compile `headless_node.cpp` directly so C++ methods are symbol-visible):
```cmake
# B-ingest — StatePut/StateGet on HeadlessNode. Compiles headless_node.cpp
# directly (its C++ methods are hidden-visibility in the kvcache dylib).
kvcache_add_test(test_state_ingest    persist/state_ingest_test.cpp
                  ${CMAKE_SOURCE_DIR}/core-abi/src/headless_node.cpp
                  ${CMAKE_SOURCE_DIR}/core-abi/src/ctx_options.cpp
                  ${CMAKE_SOURCE_DIR}/core-abi/src/kv_status.cpp
                  ${NODE_INGEST_SRCS} ${NODE_TIER_SRCS}
                  ${NODE_META_SRCS}   ${NODE_PREFIX_SRCS} ${NODE_PERSIST_SRCS})
target_include_directories(test_state_ingest PRIVATE
                  ${CMAKE_SOURCE_DIR}/core-abi/src)
target_link_libraries(test_state_ingest PRIVATE CURL::libcurl)
if(KVCACHE_ENABLE_ROCKSDB)
    target_link_libraries(test_state_ingest PRIVATE RocksDB::rocksdb)
endif()
if(OpenSSL_FOUND)
    target_link_libraries(test_state_ingest PRIVATE OpenSSL::Crypto)
endif()
```
> Copy the exact link-guard lines (`CURL`, `RocksDB`, `OpenSSL`, and any `CUDA`/`SPDK` blocks) from the `test_replica_fetch` target immediately above/below it in the same file — the source sets are identical, so the required links are too. Match that target verbatim.

- [ ] **Step 8: Build + run the put/get test**

Run: `cd build && cmake --build . -j4 --target test_state_ingest && ctest -R StateIngest --output-on-failure`
Expected: `PutThenGetServesFromDram` PASS.

- [ ] **Step 9: Add the demotion + replay test and the no-WAL guard test**

Append to `src/tests/unit/persist/state_ingest_test.cpp`:
```cpp
TEST(StateIngest, EvictedBEntryReplaysFromWal) {
    const std::string wal = "state_ingest_demote.wal";
    std::remove(wal.c_str());
    std::string err;
    // Reuse the process singleton; use a fresh key set. Force demotion by
    // putting many B entries so DRAM capacity (set by whichever test ran
    // first) is exceeded and the LRU tail (our first key) is evicted.
    auto* node = kvcache::HeadlessNode::GetOrCreate(MakeOpts(wal, 1u << 20), &err);
    ASSERT_NE(node, nullptr) << err;

    std::vector<uint8_t> first(4096, 0xEE);
    ASSERT_EQ(node->StatePut(BId(200), first.data(), first.size()), KV_OK);

    // Flood DRAM to force the first entry out (values are large; count is high
    // enough to exceed any reasonable test DRAM capacity).
    std::vector<uint8_t> filler(4096, 0x11);
    for (int i = 0; i < 4096; ++i) {
        ASSERT_EQ(node->StatePut(BId(static_cast<uint8_t>(i)  /*rotates*/ ), filler.data(), filler.size()),
                  KV_OK) << i;
    }

    // First entry is (almost certainly) no longer in DRAM, but StateGet must
    // still return it byte-identical by replaying from the WAL.
    std::vector<uint8_t> out;
    ASSERT_EQ(node->StateGet(BId(200), &out), KV_OK);
    EXPECT_EQ(out, first);
    std::remove(wal.c_str());
}
```

> **Determinism note for the demotion test:** rather than depend on the exact capacity, this test proves the *contract* — `StateGet` returns the value whether it was served from DRAM or replayed from the WAL. Because the WAL is authoritative, the assertion holds regardless of whether the first entry was actually evicted. To make the eviction path *observable* (not just the contract), keep the filler loop large; the demotion mechanics themselves are already unit-tested by the DramTier eviction tests. Do NOT add a brittle "assert it was evicted" check that depends on capacity.

For the no-WAL guard, this needs a node with an empty `state_wal_path`, which the singleton prevents in the same process. Put it in its **own test file** so it runs in a separate test binary:

Create `src/tests/unit/persist/state_ingest_nowal_test.cpp`:
```cpp
// Task 3 — StatePut/StateGet refuse to operate without a durable WAL, and
// never create a DRAM-only B entry.
#include "headless_node.h"

#include <gtest/gtest.h>
#include <vector>

#include "kvcache/kv_errors.h"
#include "state_identity.h"

using kvcache::common::SK_MEMORY;
using kvcache::common::StateIdentity;

TEST(StateIngestNoWal, RefusesWithoutWalAndStagesNothing) {
    kvcache::HeadlessNode::Options o{};
    o.tier.dram.capacity_bytes = 1u << 20;
    // state_wal_path deliberately left empty → B ingest disabled.
    std::string err;
    auto* node = kvcache::HeadlessNode::GetOrCreate(o, &err);
    ASSERT_NE(node, nullptr) << err;

    StateIdentity id{};
    id.state_kind = SK_MEMORY;
    id.content_hash[0] = 7;
    std::vector<uint8_t> blob(64, 0xAB);

    EXPECT_EQ(node->StatePut(id, blob.data(), blob.size()), KV_E_TIER_DOWN);
    std::vector<uint8_t> out;
    EXPECT_EQ(node->StateGet(id, &out), KV_E_TIER_DOWN);  // nothing to read; no DRAM-only entry
}
```
Add its target in `src/tests/unit/CMakeLists.txt` mirroring `test_state_ingest` (same source set + links, different test/source names):
```cmake
kvcache_add_test(test_state_ingest_nowal persist/state_ingest_nowal_test.cpp
                  ${CMAKE_SOURCE_DIR}/core-abi/src/headless_node.cpp
                  ${CMAKE_SOURCE_DIR}/core-abi/src/ctx_options.cpp
                  ${CMAKE_SOURCE_DIR}/core-abi/src/kv_status.cpp
                  ${NODE_INGEST_SRCS} ${NODE_TIER_SRCS}
                  ${NODE_META_SRCS}   ${NODE_PREFIX_SRCS} ${NODE_PERSIST_SRCS})
target_include_directories(test_state_ingest_nowal PRIVATE
                  ${CMAKE_SOURCE_DIR}/core-abi/src)
target_link_libraries(test_state_ingest_nowal PRIVATE CURL::libcurl)
if(KVCACHE_ENABLE_ROCKSDB)
    target_link_libraries(test_state_ingest_nowal PRIVATE RocksDB::rocksdb)
endif()
if(OpenSSL_FOUND)
    target_link_libraries(test_state_ingest_nowal PRIVATE OpenSSL::Crypto)
endif()
```

- [ ] **Step 10: Build + run both ingest test binaries**

Run: `cd build && cmake --build . -j4 --target test_state_ingest test_state_ingest_nowal && ctest -R "StateIngest" --output-on-failure`
Expected: all PASS (`PutThenGetServesFromDram`, `EvictedBEntryReplaysFromWal`, `RefusesWithoutWalAndStagesNothing`).

- [ ] **Step 11: Full regression**

Run: `cd build && cmake --build . -j4 && ctest --output-on-failure`
Expected: `100% tests passed, 0 tests failed out of 547` (542 prior + StateWal 3 + policy 2 + ingest 3 = 550... see note). Record the actual total; the requirement is **0 failed** and the KV/DramTier suites unchanged.

> Test-count note: exact totals depend on `gtest_discover_tests` per-`TEST` counting. The pass/fail gate is **0 failures** + every previously-green test still green, not a specific integer.

- [ ] **Step 12: Commit**
```bash
git add src/kvstore-node/src/tier/tier_manager.h src/kvstore-node/src/tier/tier_manager.cpp \
        src/core-abi/src/headless_node.h src/core-abi/src/headless_node.cpp \
        src/tests/unit/persist/state_ingest_test.cpp \
        src/tests/unit/persist/state_ingest_nowal_test.cpp \
        src/tests/unit/CMakeLists.txt
git commit -m "feat(b-ingest): HeadlessNode StatePut/StateGet — WAL-first ingest, DRAM demotion + replay"
```

---

### Task 4: Docs + capability-matrix honesty + final regression

**Files:**
- Modify: `docs/design/ss2-spike-q3-verdict.md` (flip the "DRAM store path" deferral → landed for single-node WAL; keep replication/lineage deferred)
- Modify: `README.md` (capability matrix: B ingest single-node WAL row — honest status)
- Create: `changelog.d/b-ingest.md` (per repo versioning rules)

- [ ] **Step 1: Update the verdict doc deferral**

In `docs/design/ss2-spike-q3-verdict.md`, find the "**Still deferred:** the DRAM *store* path that would admit a B-class entry" line (~line 228) and reword to record that single-node WAL-backed ingest has landed, while replication / strong consistency / lineage / FFI-wire remain deferred. Cite the new spec: `docs/superpowers/specs/2026-07-20-b-plane-dram-ingest-design.md`.

- [ ] **Step 2: Update the README capability matrix — honestly**

In `README.md`, find the capability-matrix rows for B-plane (Agent Memory / Durable Exec). Add/adjust an honest note that **single-node B-state ingest + WAL persistence (survives restart)** is now wired, and that replication, strong consistency, lineage (⑭), and the B1/B2 subsystems remain 🚧. Do NOT mark anything more than what landed (SS-3 discipline: never mark in-built as built).

- [ ] **Step 3: Add changelog fragment**

Run:
```bash
cat > changelog.d/b-ingest.md << 'EOF'
- **StateWal** (NEW): append-only B-state persistence engine (⑬ v0) — fsync-per-append, torn-tail-safe replay.
- **HeadlessNode.StatePut/StateGet** (NEW): C++-core B-plane ingest — WAL-first durability, DRAM staging with real state_kind, demotion + replay-from-WAL on miss.
- **ValuePolicyPersistentWal** (NEW): production SK_MEMORY policy (evictable-because-persisted + replay-from-persist).
EOF
```

- [ ] **Step 4: Final full regression**

Run: `cd build && cmake --build . -j4 && ctest --output-on-failure`
Expected: 0 failed. Record the summary line.

- [ ] **Step 5: Commit**
```bash
git add docs/design/ss2-spike-q3-verdict.md README.md changelog.d/b-ingest.md
git commit -m "docs(b-ingest): single-node B WAL ingest landed; replication/lineage still deferred"
```

---

## Self-Review

**1. Spec coverage:**
- Ingest API (C++-core StatePut/StateGet) → Task 3. ✓
- StateWal / ⑬ v0 (on-disk format, CRC, torn-tail, replay) → Task 1. ✓
- ValuePolicyPersistentWal (evictable + replay) → Task 2. ✓
- Key projection → Task 2 (`StateKeyBytesFromIdentity`) + Task 3 (`StateKeyFromIdentity` wrapper). ✓
- Persist-first ordering / demotable DRAM → Task 3 StatePut (WAL before StageToDram) + demotion test. ✓
- StageToDram gains state_kind → Task 3 Step 1. ✓
- Error handling (WAL-first, append-failure→nothing staged, torn tail, not-found) → Task 1 (torn) + Task 3 (guards, no-WAL). ✓
- Test matrix items 1–8 → mapped across Tasks 1–3. ✓
- Docs/deferral update → Task 4. ✓

**2. Placeholder scan:** No TBD/TODO/"handle edge cases". Every code step shows complete code. The one judgment call left to the implementer (exact per-target link guards in Step 7) points at a concrete existing target (`test_replica_fetch`) to copy verbatim — not a placeholder.

**3. Type consistency:**
- `StateWal::Open/Append/Get/Size` signatures identical in Task 1 header, its impl, and Task 3 call sites. ✓
- `StateKeyBytesFromIdentity` returns `std::array<uint8_t,16>` (Task 2) → wrapped by `StateKeyFromIdentity` → `DramKey` (Task 3). ✓
- `StageToDram(..., uint16_t state_kind = SK_KV)` decl (Task 3 Step 1) matches the `dram_->Insert(key,data,n,state_kind)` call and DramTier's existing `Insert(..., uint16_t state_kind = SK_KV)`. ✓
- Error codes `KV_OK / KV_E_NOT_FOUND / KV_E_TIER_DOWN / KV_E_INVAL / KV_E_INTERNAL` all exist in `kv_errors.h`. ✓
- `DramTier::HitWhere::kA1in/kAm`, `LookupResult{where,data,data_bytes}` match `dram_tier.h`. ✓
- `OnMissAction::kReplayFromPersist`, `EvictDecision::kEvictable` match `value_policy.h`. ✓

## Global Constraints recap (for reviewers)

- KV path behavior-identical (B additive; `StageToDram` default arg preserves all KV callers).
- WAL-first is the correctness invariant: fsync before DRAM staging; no DRAM-only B entry ever.
- `StateIdentity` stays C++-core-only (no ABI/wire).
- Deferred (must remain unbuilt + documented as such): replication, strong consistency, lineage ⑭, B1/B2 subsystems, FFI-wire, bounded-memory WAL, DEL/GC.
