// Task 4 — A9 DR warm-standby: ReplicationCursor.
//
// Header-only monotonic epoch cursor: tracks the last-applied epoch on the
// standby so it can dedup/resume replication on reconnect. ShouldApply(epoch)
// returns true iff epoch > last_; Advance(epoch) moves last_ forward monotonically
// (max(last_, epoch), never backwards).
#pragma once

#include <algorithm>
#include <cstdint>

namespace kvcache::node::replication {

class ReplicationCursor {
 public:
  // Returns true iff epoch > last_ (not yet applied).
  bool ShouldApply(uint64_t epoch) const { return epoch > last_; }

  // Advances the cursor monotonically: last_ = max(last_, epoch).
  // Safe to call multiple times with out-of-order epochs; the cursor always
  // advances forward or stays in place, never backwards.
  void Advance(uint64_t epoch) { last_ = std::max(last_, epoch); }

  // Returns the last-applied epoch.
  uint64_t Last() const { return last_; }

 private:
  uint64_t last_ = 0;
};

}  // namespace kvcache::node::replication
