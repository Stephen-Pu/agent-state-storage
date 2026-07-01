//! Phase A7 — Rust binding integration tests against the live libkvcache.
//!
//! Exercises the full data-plane cycle (reserve → write → publish → seal →
//! lookup → fetch → wait → release) through the in-process loopback backend,
//! plus the stateless KV-tensor codec and metrics scrape. Distinct token
//! ranges per test keep them independent under cargo's default parallel
//! harness (Q-5 (tenant, model) ART isolation + disjoint tokens).

use kvcache::{kvtensor_decode, kvtensor_encode, metrics_scrape, Context, Locator};

const BPT: usize = 64; // bytes per token

fn payload(n_tokens: usize) -> Vec<u8> {
    (0..n_tokens * BPT).map(|i| ((i * 7) & 0xFF) as u8).collect()
}

#[test]
fn reserve_seal_lookup_fetch_round_trip() {
    let cx = Context::open("rust-tenant", "rust-rt").expect("open");
    let tokens: Vec<u32> = (90000..90032).collect(); // 32 tokens = 2 chunks
    let data = payload(tokens.len());

    let loc = Locator::for_tokens("rust-tenant", "rust-rt", &tokens);
    let mut rsv = cx.reserve(&loc, data.len()).expect("reserve");
    assert!(rsv.capacity() >= data.len());
    rsv.write(&data).expect("write slot");
    cx.publish(rsv.handle(), data.len() as u64).expect("publish");
    cx.seal(rsv.handle(), &tokens).expect("seal");

    let hit = cx.lookup(&tokens).expect("lookup").expect("hit");
    assert_eq!(hit.matched_tokens, 32);

    let mut dst = vec![0u8; hit.matched_tokens as usize * BPT];
    let c = cx.fetch(hit.handle, &mut dst).expect("fetch");
    cx.wait(c, 5000).expect("wait");
    assert_eq!(dst, data, "fetched bytes must match the sealed payload");

    // stored_bytes on the read handle reflects the sealed content size.
    assert_eq!(cx.stored_bytes(hit.handle).expect("stored_bytes"), data.len());
    cx.release(hit.handle).expect("release");
}

#[test]
fn lookup_miss_returns_none() {
    let cx = Context::open("rust-tenant", "rust-miss").expect("open");
    let tokens: Vec<u32> = (91000..91016).collect();
    assert!(cx.lookup(&tokens).expect("lookup").is_none());
}

#[test]
fn reservation_write_rejects_oversized() {
    let cx = Context::open("rust-tenant", "rust-oversize").expect("open");
    let tokens: Vec<u32> = (92000..92016).collect();
    let loc = Locator::for_tokens("rust-tenant", "rust-oversize", &tokens);
    let mut rsv = cx.reserve(&loc, 64).expect("reserve");
    let too_big = vec![0u8; rsv.capacity() + 1];
    assert!(rsv.write(&too_big).is_err());
}

#[test]
fn codec_round_trip_is_lossy_within_tolerance() {
    let n_tokens: u32 = 32;
    let elems: u32 = (BPT / 4) as u32; // fp32
    // Smooth along the token axis so DPCM + int8 quant reconstructs tightly.
    let data: Vec<f32> = (0..n_tokens)
        .flat_map(|t| (0..elems).map(move |e| 1.0 + 0.01 * t as f32 + 0.1 * (e % 5) as f32))
        .collect();

    let blob = kvtensor_encode(&data, n_tokens, elems, 8, true).expect("encode");
    assert!(
        blob.len() < data.len() * 4,
        "compressed {} should be < raw {}",
        blob.len(),
        data.len() * 4
    );

    let (rec, (dnt, dne)) = kvtensor_decode(&blob).expect("decode");
    assert_eq!((dnt, dne), (n_tokens, elems));
    assert_eq!(rec.len(), data.len());
    let max_err = data
        .iter()
        .zip(&rec)
        .map(|(a, b)| (a - b).abs())
        .fold(0.0f32, f32::max);
    assert!(max_err < 0.01, "max reconstruction error {max_err} too large");
}

#[test]
fn int4_is_smaller_and_lossier_than_int8() {
    let n_tokens: u32 = 64;
    let elems: u32 = (BPT / 4) as u32;
    let data: Vec<f32> = (0..n_tokens * elems).map(|i| (i as f32).sin()).collect();

    let b8 = kvtensor_encode(&data, n_tokens, elems, 8, true).expect("enc8");
    let b4 = kvtensor_encode(&data, n_tokens, elems, 4, true).expect("enc4");
    assert!(b4.len() <= b8.len(), "int4 blob {} should be <= int8 {}", b4.len(), b8.len());
}

#[test]
fn metrics_scrape_returns_prometheus_text() {
    // Touch the backend so at least the reserve gauges are seeded.
    let cx = Context::open("rust-tenant", "rust-metrics").expect("open");
    let tokens: Vec<u32> = (93000..93016).collect();
    let loc = Locator::for_tokens("rust-tenant", "rust-metrics", &tokens);
    let _ = cx.reserve(&loc, 64).expect("reserve");

    let text = metrics_scrape().expect("scrape");
    assert!(text.contains("kv_"), "expected Prometheus kv_* series, got: {text:.120}");
}
