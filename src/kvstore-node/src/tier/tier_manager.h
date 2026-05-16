// LLD §3.3 — Cross-tier coordinator.
//
// The TierManager owns all five tiers (HBM is engine-owned and surfaces only
// for promotion accounting) and exposes the surface that ingest and fetch
// paths need:
//
//   T1 Pinned : AcquirePinnedSlot / ReleasePinnedSlot           (LLD §3.3 T1)
//   T2 DRAM   : StageToDram / LookupDram / EraseDram             (LLD §3.3 T2)
//   T3 NVMe   : PutNvme / GetNvme / EraseNvme                    (LLD §3.3 T3)
//   T4 Cold   : PutCold / GetCold / EraseCold                    (LLD §3.3 T4)
//
//   Fetch     : Unified "DRAM → NVMe → Cold" lookup. Returns the first hit
//               and (configurably) promotes the bytes back into hotter tiers.
//
// Promotion / demotion across all five tiers — the ROI model (LLD §3.3) is
// intentionally NOT implemented in MVP. The unified Fetch path performs the
// minimum useful promotion: on a Cold hit, copy into DRAM so the next Lookup
// is fast.  T3 ↔ T2 promotion on first DRAM miss is also enabled by default.
#pragma once

#include <memory>
#include <optional>
#include <string>

#include "tier/cold_tier.h"
#include "tier/dram_tier.h"
#include "tier/nvme_tier.h"
#include "tier/pinned_tier.h"

namespace kvcache::node::tier {

class TierManager {
   public:
    struct Options {
        PinnedTier::Options pinned;
        DramTier::Options   dram;

        // Optional: omit `nvme.path` to disable T3.
        NvmeTier::Options   nvme;
        bool                enable_nvme = false;

        // Optional: omit `cold` to disable T4.
        ColdTierOptions     cold;
        bool                enable_cold = false;

        // Promotion controls on the unified Fetch path.
        bool                promote_cold_to_dram = true;
        bool                promote_cold_to_nvme = true;
        bool                promote_nvme_to_dram = true;
    };

    static std::unique_ptr<TierManager> Create(const Options& opts, std::string* err);

    ~TierManager() = default;
    TierManager(const TierManager&)            = delete;
    TierManager& operator=(const TierManager&) = delete;

    // ---- T1 (Pinned) ----
    std::optional<SlotDesc> AcquirePinnedSlot();
    void                    ReleasePinnedSlot(SlotId id);

    // ---- T2 (DRAM) ----
    void                    StageToDram(const DramKey& key, const uint8_t* data, std::size_t n);
    DramTier::LookupResult  LookupDram (const DramKey& key);
    bool                    EraseDram  (const DramKey& key);

    // ---- T3 (NVMe) ----
    bool PutNvme  (const DramKey& key, const uint8_t* data, std::size_t n, std::string* err);
    bool GetNvme  (const DramKey& key, std::vector<uint8_t>* out, std::string* err);
    bool EraseNvme(const DramKey& key);
    bool HasNvme  () const noexcept { return nvme_ != nullptr; }

    // ---- T4 (Cold) ----
    bool PutCold  (const DramKey& key, const uint8_t* data, std::size_t n, std::string* err);
    bool GetCold  (const DramKey& key, std::vector<uint8_t>* out, std::string* err);
    bool EraseCold(const DramKey& key, std::string* err);
    bool HasCold  () const noexcept { return cold_ != nullptr; }

    // ---- Unified fetch ----

    enum class FetchHit {
        kMiss = 0,
        kDram,
        kNvme,
        kCold,
    };

    struct FetchResult {
        FetchHit         hit = FetchHit::kMiss;
        std::vector<uint8_t> data;
    };

    // Look up `key` in DRAM, then NVMe, then Cold. On a hit from a colder
    // tier, promote per the Options. Returns the bytes the caller asked for.
    FetchResult Fetch(const DramKey& key, std::string* err);

    // ---- accessors for tests / ops ----
    PinnedTier& pinned() noexcept { return *pinned_; }
    DramTier&   dram  () noexcept { return *dram_;   }
    NvmeTier*   nvme  () noexcept { return nvme_.get(); }
    IColdTier*  cold  () noexcept { return cold_.get(); }

   private:
    TierManager() = default;

    Options                       opts_;
    std::unique_ptr<PinnedTier>   pinned_;
    std::unique_ptr<DramTier>     dram_;
    std::unique_ptr<NvmeTier>     nvme_;
    std::unique_ptr<IColdTier>    cold_;
};

}  // namespace kvcache::node::tier
