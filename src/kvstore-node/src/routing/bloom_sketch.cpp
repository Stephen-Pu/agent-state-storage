// LLD §4.2 — Bloom-sketch implementation.
#include "routing/bloom_sketch.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "blake3.h"

namespace kvcache::node::routing {

namespace {

constexpr double kLn2  = 0.6931471805599453;
constexpr double kLn2Sq = kLn2 * kLn2;

// Round up to a multiple of 64 so we can store bits in uint64_t words.
uint32_t RoundUpTo64(uint32_t v) {
    return (v + 63u) & ~63u;
}

// Two-hash double-hashing scheme: h_i = h1 + i * h2 (Kirsch & Mitzenmacher
// 2006). Cheap and shown to be statistically equivalent to k independent
// hashes for typical params.
struct HashPair { uint64_t h1; uint64_t h2; };

HashPair PairFor(std::span<const uint8_t> key) {
    const uint64_t a = kvcache::hash::Blake3_64(key);
    // Salt the second hash with a constant; the facade does not yet support
    // streaming with seeds, so we hash a salt+key buffer. Replace once
    // real BLAKE3 is vendored.
    std::vector<uint8_t> salted;
    salted.reserve(key.size() + 4);
    salted.insert(salted.end(), {0xBE, 0xEF, 0xCA, 0xFE});
    salted.insert(salted.end(), key.begin(), key.end());
    const uint64_t b = kvcache::hash::Blake3_64({salted.data(), salted.size()});
    return {a, b};
}

}  // namespace

BloomParams BloomParams::ForCapacity(uint64_t expected_n, double target_fpr) {
    if (expected_n == 0) expected_n = 1;
    if (target_fpr <= 0.0 || target_fpr >= 1.0) target_fpr = 0.01;
    const double n = static_cast<double>(expected_n);
    const double m = -n * std::log(target_fpr) / kLn2Sq;
    const double k = (m / n) * kLn2;
    BloomParams p{};
    p.m_bits   = RoundUpTo64(static_cast<uint32_t>(std::ceil(m)));
    p.k_hashes = std::max<uint32_t>(1, static_cast<uint32_t>(std::round(k)));
    return p;
}

// ---------------------------------------------------------------------------
// LocalBloom
// ---------------------------------------------------------------------------

LocalBloom::LocalBloom(BloomParams params)
    : params_(params), words_(params.m_bits / 64, 0) {}

void LocalBloom::SetBit(uint64_t idx) {
    words_[idx / 64] |= (1ULL << (idx & 63));
}
bool LocalBloom::TestBit(uint64_t idx) const {
    return (words_[idx / 64] & (1ULL << (idx & 63))) != 0;
}

void LocalBloom::Add(std::span<const uint8_t> key) {
    auto hp = PairFor(key);
    std::lock_guard lk(mu_);
    for (uint32_t i = 0; i < params_.k_hashes; ++i) {
        const uint64_t bit = (hp.h1 + static_cast<uint64_t>(i) * hp.h2) % params_.m_bits;
        SetBit(bit);
    }
    inserts_.fetch_add(1, std::memory_order_relaxed);
}

bool LocalBloom::MaybeContains(std::span<const uint8_t> key) const {
    auto hp = PairFor(key);
    std::lock_guard lk(mu_);
    for (uint32_t i = 0; i < params_.k_hashes; ++i) {
        const uint64_t bit = (hp.h1 + static_cast<uint64_t>(i) * hp.h2) % params_.m_bits;
        if (!TestBit(bit)) return false;
    }
    return true;
}

std::vector<uint8_t> LocalBloom::Snapshot() const {
    std::lock_guard lk(mu_);
    std::vector<uint8_t> out(words_.size() * sizeof(uint64_t));
    std::memcpy(out.data(), words_.data(), out.size());
    return out;
}

void LocalBloom::Reset() {
    std::lock_guard lk(mu_);
    std::fill(words_.begin(), words_.end(), 0);
    inserts_.store(0, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// AggregatedBloom
// ---------------------------------------------------------------------------

AggregatedBloom::AggregatedBloom() = default;
AggregatedBloom::AggregatedBloom(BloomParams params)
    : params_(params), words_(params.m_bits / 64, 0) {}

void AggregatedBloom::Set(BloomParams params, std::vector<uint8_t> raw) {
    std::lock_guard lk(mu_);
    params_ = params;
    words_.assign(params.m_bits / 64, 0);
    const std::size_t expected = words_.size() * sizeof(uint64_t);
    if (raw.size() < expected) return;  // ignore truncated input
    std::memcpy(words_.data(), raw.data(), expected);
}

bool AggregatedBloom::MaybeContains(std::span<const uint8_t> key) const {
    auto hp = PairFor(key);
    std::lock_guard lk(mu_);
    if (params_.m_bits == 0 || words_.empty()) return false;
    for (uint32_t i = 0; i < params_.k_hashes; ++i) {
        const uint64_t bit = (hp.h1 + static_cast<uint64_t>(i) * hp.h2) % params_.m_bits;
        if ((words_[bit / 64] & (1ULL << (bit & 63))) == 0) return false;
    }
    return true;
}

BloomParams AggregatedBloom::Params() const {
    std::lock_guard lk(mu_);
    return params_;
}

}  // namespace kvcache::node::routing
