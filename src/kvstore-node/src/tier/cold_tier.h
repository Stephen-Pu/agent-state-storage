// LLD §3.3 T4 — Cold tier via a pluggable multi-cloud UFS.
//
// The cold tier is the only tier that crosses cloud boundaries. We outsource
// the multi-cloud problem to a pluggable UFS — see "Vendor-Neutral /
// UFS-as-Enabler" framing in HLD §2.3.
//
// MVP integration strategy:
//   * Abstract interface (IColdTier) — Put / Get / Delete / Exists.
//   * Concrete `FilesystemColdTier` — backed by a directory path. This is
//     intentionally generic: the same implementation works for
//       (a) local disk staging (tests, dev)
//       (b) a multi-cloud UFS mounted via FUSE (production default)
//       (c) any POSIX-mounted UFS
//   * Native REST / gRPC UFS client = Phase-2 (TODO(stephen)) once it
//     proves to be needed beyond what the FUSE mount delivers.
//
// Layout under the root directory:
//   {root}/{first_2_hex(key)}/{rest_of_hex(key)}.kv
// The 2-hex shard byte keeps any single directory under ~65k files, which is
// well within ext4 / xfs single-directory comfort.
//
// Compression / encryption are out of scope here; they will live in a
// pluggable middleware layer wrapped around the IColdTier interface
// (TODO(stephen): zstd + SSE-S3 once defined by ⑩/MVP item).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "tier/dram_tier.h"  // DramKey reused as content key

namespace kvcache::node::tier {

class IColdTier {
   public:
    virtual ~IColdTier() = default;

    virtual std::string Name() const = 0;

    virtual bool Put   (const DramKey&, const uint8_t* data, std::size_t n,
                        std::string* err) = 0;
    virtual bool Get   (const DramKey&, std::vector<uint8_t>* out,
                        std::string* err) = 0;
    virtual bool Delete(const DramKey&, std::string* err) = 0;
    virtual bool Exists(const DramKey&) const = 0;
};

// Filesystem-backed cold tier. Works with any POSIX path — local disk,
// FUSE-mounted UFS, or another UFS mount.
class FilesystemColdTier final : public IColdTier {
   public:
    struct Options {
        std::string root;                         // required
        bool        create_if_missing = true;
        bool        fsync_on_put      = true;
    };

    static std::unique_ptr<FilesystemColdTier> Create(const Options& opts,
                                                       std::string* err);
    ~FilesystemColdTier() override = default;

    std::string Name() const override { return "filesystem"; }

    bool Put   (const DramKey&, const uint8_t* data, std::size_t n, std::string* err) override;
    bool Get   (const DramKey&, std::vector<uint8_t>* out, std::string* err) override;
    bool Delete(const DramKey&, std::string* err) override;
    bool Exists(const DramKey&) const override;

   private:
    FilesystemColdTier() = default;

    // Resolve a key to its on-disk path. The shard directory is created lazily
    // on first Put for that shard.
    std::string PathFor(const DramKey& key, bool ensure_shard_exists) const;

    Options opts_;
};

// Factory for selecting a backend by name.
//   "fs" / "fuse-mount" -> FilesystemColdTier (POSIX path / FUSE mount)
//   "native-rest"       -> RestColdTier       (direct HTTP object store; B3)
struct ColdTierOptions {
    std::string                 type = "fs";
    FilesystemColdTier::Options fs;
    // REST backend options. A self-contained POD mirror of
    // RestColdTier::Options — kept here (rather than including
    // rest_cold_tier.h) so this header stays free of the transport seam.
    // Only set/read when type == "native-rest".
    struct Rest {
        std::string base_url;
        std::string key_prefix = "kvcache/";
        std::string bearer_token;
        std::string ca_pem_path;
        std::string client_cert_pem_path;
        std::string client_key_pem_path;
        long        timeout_ms = 30000;
    } rest;
    // Compression middleware (Phase B3.1). When codec != "none" the selected
    // backend is wrapped in a CompressingColdTier. "none"|"identity"|"zstd"
    // ("zstd" requires a KVCACHE_ENABLE_ZSTD build).
    struct Compression {
        std::string codec = "none";
        int         level = 3;  // zstd level (ignored by identity)
    } compression;
};
std::unique_ptr<IColdTier> CreateColdTier(const ColdTierOptions& opts, std::string* err);

}  // namespace kvcache::node::tier
