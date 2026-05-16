// libkvcache.so — implementation of the public C ABI.
// LLD §6.1.2.
//
// Backend: the in-process HeadlessNode (see headless_node.h). The Step-7+
// cross-process backend (shmem ring to kvagent) will replace this TU while
// keeping the public ABI identical.
#include "kvcache/kv_abi.h"

#include <cstring>
#include <string>

#include "headless_node.h"
#include "kvcache/kv_errors.h"

extern "C" {

#define KV_API __attribute__((visibility("default")))

struct kv_ctx_s {
    std::string  tenant_id;
    std::string  model_id;
    uint64_t     model_id_hash = 0;
    kvcache::abi::HeadlessNode* node = nullptr;
};

KV_API int kv_ctx_open(const kv_ctx_config_t* cfg, kv_ctx_t** out_ctx) {
    if (!cfg || !out_ctx) return KV_E_INVAL;
    if (cfg->abi_version != KVCACHE_ABI_VERSION) return KV_E_VERSION_MISMATCH;

    kvcache::abi::HeadlessNode::Options opts{};
    // Sensible defaults for headless / demo bring-up. Production callers
    // override via environment variables (TODO(stephen): expose options on
    // kv_ctx_config_t).
    opts.tier.pinned.pool_bytes = 32ull << 20;   // 32 MiB
    opts.tier.pinned.slot_bytes =  4ull << 20;   //  4 MiB per slot
    opts.tier.pinned.use_mlock  = false;
    opts.tier.dram.capacity_bytes    = 64ull << 20;
    opts.tier.dram.a1out_max_entries = 1024;
    opts.tier.enable_nvme = false;
    opts.tier.enable_cold = false;
    opts.nixl_backend = "loopback";

    std::string err;
    auto* node = kvcache::abi::HeadlessNode::GetOrCreate(opts, &err);
    if (!node) return KV_E_INTERNAL;

    auto c = new kv_ctx_s();
    if (cfg->tenant_id) c->tenant_id = cfg->tenant_id;
    if (cfg->model_id)  c->model_id  = cfg->model_id;
    // Hash the model_id string into a 64-bit canonical hash.
    uint64_t h = 0xcbf29ce484222325ULL;
    for (char ch : c->model_id) {
        h ^= static_cast<uint8_t>(ch);
        h *= 0x100000001b3ULL;
    }
    c->model_id_hash = h;
    c->node = node;
    *out_ctx = c;
    return KV_OK;
}

KV_API int kv_ctx_close(kv_ctx_t* ctx) {
    delete ctx;
    return KV_OK;
}

KV_API int kv_lookup(kv_ctx_t* ctx, const uint32_t* tokens, size_t n,
                    kv_locator_t* meta, kv_handle_t* handle,
                    uint32_t* matched_tokens) {
    if (!ctx || !ctx->node) return KV_E_INVAL;
    return ctx->node->Lookup(ctx->tenant_id.c_str(), ctx->model_id_hash,
                              tokens, n, meta, handle, matched_tokens);
}

KV_API int kv_reserve(kv_ctx_t* ctx, const kv_locator_t* locator, size_t bytes,
                     kv_handle_t* handle, kv_buffer_desc_t* slot) {
    if (!ctx || !ctx->node) return KV_E_INVAL;
    return ctx->node->Reserve(locator, bytes, handle, slot);
}

KV_API int kv_publish(kv_ctx_t* ctx, kv_handle_t handle, kv_buffer_desc_t src,
                     uint64_t watermark) {
    if (!ctx || !ctx->node) return KV_E_INVAL;
    return ctx->node->Publish(handle, src, watermark);
}

KV_API int kv_fetch(kv_ctx_t* ctx, kv_handle_t handle,
                   const kv_range_t* ranges, size_t n,
                   kv_buffer_desc_t dst, kv_completion_t* completion) {
    if (!ctx || !ctx->node) return KV_E_INVAL;
    return ctx->node->Fetch(handle, ranges, n, dst, completion);
}

KV_API int kv_wait(kv_ctx_t* ctx, kv_completion_t cid, uint32_t timeout_ms) {
    if (!ctx || !ctx->node) return KV_E_INVAL;
    return ctx->node->Wait(cid, timeout_ms);
}

KV_API int kv_seal(kv_ctx_t* ctx, kv_handle_t handle,
                  const uint32_t* tokens, size_t n_tokens) {
    if (!ctx || !ctx->node) return KV_E_INVAL;
    return ctx->node->Seal(handle, tokens, n_tokens);
}

KV_API int kv_release(kv_ctx_t* ctx, kv_handle_t handle) {
    if (!ctx || !ctx->node) return KV_E_INVAL;
    return ctx->node->Release(handle);
}

KV_API int kv_subscribe_events(kv_ctx_t*, kv_event_callback_t, void*) {
    // TODO(stephen): wire to HeadlessNode::Events()->Subscribe + poll loop.
    return KV_E_INTERNAL;
}

}  // extern "C"
