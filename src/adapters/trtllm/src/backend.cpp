// TRT-LLM-shaped KV cache backend over the Core ABI — implementation.
//
// Translates the C++ surface in include/kvcache_trtllm/backend.h into
// kv_abi.h calls. Mirrors the Python adapters' Reserve → write → Publish
// → Seal sequence inside Store(), and the Lookup → Fetch → Wait → Release
// sequence inside Retrieve().
#include "kvcache_trtllm/backend.h"

#include "kvcache/kv_abi.h"
#include "kvcache/kv_errors.h"
#include "kvcache/kv_types.h"
#include "hashing.h"  // Phase W-2 — canonical kvcache::Fnv1a64

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>

namespace kvcache::trtllm {

namespace {

[[noreturn]] void ThrowAbi(const char* op, int status) {
    std::string msg = std::string(op) + ": ";
    msg += kv_status_str(status);
    msg += " (status=" + std::to_string(status) + ")";
    throw std::runtime_error(std::move(msg));
}

// model_id / tenant_id_hash use the canonical kvcache::Fnv1a64 (hashing.h)
// — the same hash the C ABI (kv_abi.cpp) and Python connector derive, so a
// locator built client-side here resolves to the same ctx / namespace. We
// rebuild a locator client-side because the reserve / seal path wants a
// pre-computed locator + range, not a token list.

// Lightweight prefix_hash for the Locator built in Store(): the C ABI side
// derives chunk paths from the token bytes via Blake3, but Store only
// needs a stable, content-addressed 16-byte identity, so we use the same
// FNV-1a primitive folded over the token bytes. Keeps the dep surface
// trivial (no blake3 link from the adapter); the underlying ART lookup
// still works because Lookup uses the engine-side token-derived path
// produced inside libkvcache.
void FillPrefixHash(std::span<const uint32_t> tokens, uint8_t out[16]) {
    uint64_t a = 0xcbf29ce484222325ULL;
    uint64_t b = 0x84222325cbf29ce4ULL;
    for (uint32_t t : tokens) {
        a ^= static_cast<uint64_t>(t);
        a *= 0x100000001b3ULL;
        b ^= static_cast<uint64_t>(t) * 0x9e3779b97f4a7c15ULL;
        b *= 0x100000001b3ULL;
    }
    for (int i = 0; i < 8; ++i) out[i]     = static_cast<uint8_t>(a >> (i * 8));
    for (int i = 0; i < 8; ++i) out[8 + i] = static_cast<uint8_t>(b >> (i * 8));
}

}  // namespace

struct TrtLlmKVCacheBackend::Impl {
    kv_ctx_t* ctx = nullptr;
};

TrtLlmKVCacheBackend::TrtLlmKVCacheBackend(const std::string& tenant_id,
                                             const std::string& model_id,
                                             std::size_t bytes_per_token)
    : impl_(std::make_unique<Impl>()),
      tenant_id_(tenant_id),
      model_id_(model_id),
      bytes_per_token_(bytes_per_token) {
    if (bytes_per_token == 0) {
        throw std::invalid_argument("bytes_per_token must be > 0");
    }
    kv_ctx_config_t cfg{};
    cfg.abi_version     = KVCACHE_ABI_VERSION;
    cfg.agent_endpoint  = nullptr;
    cfg.tenant_id       = tenant_id_.c_str();
    cfg.model_id        = model_id_.c_str();
    cfg.flags           = 0;

    int rc = kv_ctx_open(&cfg, &impl_->ctx);
    if (rc != KV_OK) ThrowAbi("kv_ctx_open", rc);
}

TrtLlmKVCacheBackend::~TrtLlmKVCacheBackend() {
    if (impl_ && impl_->ctx) {
        kv_ctx_close(impl_->ctx);
        impl_->ctx = nullptr;
    }
}

TrtLlmKVCacheBackend::TrtLlmKVCacheBackend(TrtLlmKVCacheBackend&& other) noexcept
    : impl_(std::move(other.impl_)),
      tenant_id_(std::move(other.tenant_id_)),
      model_id_(std::move(other.model_id_)),
      bytes_per_token_(other.bytes_per_token_) {}

TrtLlmKVCacheBackend& TrtLlmKVCacheBackend::operator=(TrtLlmKVCacheBackend&& other) noexcept {
    if (this != &other) {
        if (impl_ && impl_->ctx) {
            kv_ctx_close(impl_->ctx);
        }
        impl_            = std::move(other.impl_);
        tenant_id_       = std::move(other.tenant_id_);
        model_id_        = std::move(other.model_id_);
        bytes_per_token_ = other.bytes_per_token_;
    }
    return *this;
}

std::size_t TrtLlmKVCacheBackend::Lookup(std::span<const uint32_t> tokens) {
    if (tokens.empty()) return 0;
    kv_locator_t meta{};
    kv_handle_t handle = 0;
    uint32_t matched   = 0;
    int rc = kv_lookup(impl_->ctx, tokens.data(), tokens.size(),
                        &meta, &handle, &matched);
    if (rc == KV_E_NOT_FOUND) return 0;
    if (rc != KV_OK) ThrowAbi("kv_lookup", rc);
    // Release immediately — Lookup is a no-pin probe.
    kv_release(impl_->ctx, handle);
    return static_cast<std::size_t>(matched);
}

void TrtLlmKVCacheBackend::Store(std::span<const uint32_t> tokens,
                                   std::span<const std::byte> kv_bytes) {
    if (tokens.empty()) throw std::invalid_argument("tokens must be non-empty");
    if (kv_bytes.empty()) throw std::invalid_argument("kv_bytes must be non-empty");

    // Build the locator (LLD §2.1) — 16B tenant + 8B model + 16B prefix +
    // 16B range + 4B version + 4B flags.
    kv_locator_t loc{};
    // tenant_id: FNV-1a of the tenant string, broadcast across 16 bytes.
    uint64_t th = kvcache::Fnv1a64(tenant_id_);
    for (int i = 0; i < 8; ++i) loc.tenant_id[i]     = static_cast<uint8_t>(th >> (i * 8));
    for (int i = 0; i < 8; ++i) loc.tenant_id[8 + i] = static_cast<uint8_t>(th >> (i * 8));
    loc.model_id_hash = kvcache::Fnv1a64(model_id_);
    FillPrefixHash(tokens, loc.prefix_hash);
    loc.version = 1;
    loc.flags   = 0;

    kv_handle_t handle = 0;
    kv_buffer_desc_t slot{};
    int rc = kv_reserve(impl_->ctx, &loc, kv_bytes.size(), &handle, &slot);
    if (rc != KV_OK) ThrowAbi("kv_reserve", rc);
    if (slot.len < kv_bytes.size()) {
        kv_release(impl_->ctx, handle);
        throw std::runtime_error("reserved slot too small: " +
                                  std::to_string(slot.len) + " < " +
                                  std::to_string(kv_bytes.size()));
    }
    std::memcpy(slot.addr, kv_bytes.data(), kv_bytes.size());

    kv_buffer_desc_t empty{};  // publish takes a src descriptor; copy already done
    rc = kv_publish(impl_->ctx, handle, empty,
                     static_cast<uint64_t>(kv_bytes.size()));
    if (rc != KV_OK) ThrowAbi("kv_publish", rc);

    rc = kv_seal(impl_->ctx, handle, tokens.data(), tokens.size());
    if (rc != KV_OK) ThrowAbi("kv_seal", rc);
}

std::optional<std::vector<std::byte>>
TrtLlmKVCacheBackend::Retrieve(std::span<const uint32_t> tokens) {
    if (tokens.empty()) return std::nullopt;
    kv_locator_t meta{};
    kv_handle_t handle = 0;
    uint32_t matched   = 0;
    int rc = kv_lookup(impl_->ctx, tokens.data(), tokens.size(),
                        &meta, &handle, &matched);
    if (rc == KV_E_NOT_FOUND) return std::nullopt;
    if (rc != KV_OK) ThrowAbi("kv_lookup", rc);

    std::vector<std::byte> buf(static_cast<std::size_t>(matched) * bytes_per_token_);

    kv_buffer_desc_t dst{};
    dst.addr     = buf.data();
    dst.len      = buf.size();
    dst.mem_type = 0;  // KV_MEM_HOST
    dst.mr_key   = 0;

    kv_completion_t cid = 0;
    rc = kv_fetch(impl_->ctx, handle, nullptr, 0, dst, &cid);
    if (rc != KV_OK) {
        kv_release(impl_->ctx, handle);
        ThrowAbi("kv_fetch", rc);
    }
    rc = kv_wait(impl_->ctx, cid, 5000);
    if (rc != KV_OK) {
        kv_release(impl_->ctx, handle);
        ThrowAbi("kv_wait", rc);
    }
    kv_release(impl_->ctx, handle);
    return buf;
}

bool TrtLlmKVCacheBackend::Drop(std::span<const uint32_t> tokens) {
    (void)tokens;
    return false;
}

}  // namespace kvcache::trtllm
