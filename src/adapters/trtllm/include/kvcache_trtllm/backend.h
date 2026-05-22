// TRT-LLM-shaped KV cache backend over the Core ABI.
//
// TRT-LLM's KVCacheManager plugin surface is native C++ — there's no Python
// connector glue like SGLang / AIBrix. This header is what the TRT-LLM
// integration includes; the .cpp links against libkvcache (the public C
// ABI shared library), so the plugin doesn't drag in any kvstore-node
// internals.
//
// Method shape mirrors the Python adapters (lookup / store / retrieve /
// drop) so engineers reading any one adapter recognise the others.
//
// LLD reference: §6.1.4 (engine adapter strategy) + §6.1.2 (Core ABI).
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace kvcache::trtllm {

// SGLang & AIBrix use 16-token chunks; the TRT-LLM backend follows the
// same convention so LPM behavior is identical across adapters.
inline constexpr std::size_t kChunkTokens = 16;

// Backend lifecycle is RAII: ctor opens a kv_ctx_t, dtor closes it.
// Construction throws std::runtime_error on invalid configuration or on
// kv_ctx_open failure (status code is encoded in the .what() string).
class TrtLlmKVCacheBackend {
   public:
    // bytes_per_token controls how Retrieve sizes the destination buffer
    // from the matched-prefix token count. Must be > 0.
    TrtLlmKVCacheBackend(const std::string& tenant_id,
                          const std::string& model_id,
                          std::size_t bytes_per_token);

    ~TrtLlmKVCacheBackend();

    TrtLlmKVCacheBackend(const TrtLlmKVCacheBackend&)            = delete;
    TrtLlmKVCacheBackend& operator=(const TrtLlmKVCacheBackend&) = delete;
    TrtLlmKVCacheBackend(TrtLlmKVCacheBackend&&) noexcept;
    TrtLlmKVCacheBackend& operator=(TrtLlmKVCacheBackend&&) noexcept;

    // LPM probe. Returns the matched token count (always a multiple of
    // kChunkTokens) or 0 on miss. Does NOT pin a refcount — callers that
    // intend to immediately Retrieve should follow up with that call.
    std::size_t Lookup(std::span<const uint32_t> tokens);

    // Atomic commit: reserve a slot, copy bytes in, publish + seal in one
    // go. Throws on any underlying ABI error.
    void Store(std::span<const uint32_t> tokens,
                std::span<const std::byte> kv_bytes);

    // Pull cached bytes for the LPM-matched prefix. Returns std::nullopt
    // on miss; otherwise a buffer of size matched_tokens * bytes_per_token.
    std::optional<std::vector<std::byte>> Retrieve(
        std::span<const uint32_t> tokens);

    // No-op hint that `tokens` are cold. The MVP Core ABI has no Drop
    // verb — eviction is capacity / refcount driven. Returns false so
    // engine wiring needs no conditional.
    bool Drop(std::span<const uint32_t> tokens);

    // Active tenant / model for diagnostics.
    const std::string& tenant_id() const noexcept { return tenant_id_; }
    const std::string& model_id()  const noexcept { return model_id_; }

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::string tenant_id_;
    std::string model_id_;
    std::size_t bytes_per_token_;
};

}  // namespace kvcache::trtllm
