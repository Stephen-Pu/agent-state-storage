# kvcache operator runbook (Phase G-5)

Operator-facing playbook for the alerts shipped in the kvcache Helm
chart (`templates/prometheusrule.yaml`). For each alert: what it
means, what to check, common causes, and remediation. Every alert
also fires a metrics snapshot — start there before drilling into
logs.

> **Scope**: the four G-5 alerts (`KVCachePinnedTierSaturated`,
> `KVCacheReserveNomemSpike`, `KVCacheArtRetireBacklog`,
> `KVCacheArtWritesStalled`) keyed to the 7 gauges actually
> published by the in-tree kvstore-node today. The five
> pre-existing LLD §6.2 alerts (`KVCacheHitRateDrop`,
> `KVCacheColdTierLatencyHigh`, `KVCacheNodeUnhealthy`,
> `KVCacheQuotaBreach`, `KVCacheEvictionStorm`) reference metrics
> still pending node-side wiring — out of scope for this runbook.

## Metrics cheat sheet

What each gauge means, where it's published from, and the typical
healthy range.

| Metric                                       | Type    | Source                          | Healthy range                           |
|----------------------------------------------|---------|---------------------------------|------------------------------------------|
| `kv_pinned_tier_slots_total`                 | gauge   | `headless_node.cpp` (G-3)       | constant after Init (e.g. 64)            |
| `kv_pinned_tier_slots_in_use`                | gauge   | `headless_node.cpp` (G-3)       | `0 ≤ x ≤ slots_total`                    |
| `kv_pinned_tier_slots_utilization_ratio`     | gauge   | `headless_node.cpp` (G-3)       | usually < 0.7 in steady state            |
| `kv_reserves_total`                          | counter | `kv_reserve` ABI                | monotonic; rate matches engine save rate |
| `kv_reserve_nomem_total`                     | counter | `kv_reserve` NOMEM path         | should stay flat                         |
| `kv_reserve_invalid_total`                   | counter | `kv_reserve` arg-validation     | should stay flat                         |
| `kv_art_leaf_count`                          | gauge   | `headless_node.cpp` (D-3)       | grows with workload; eviction trims it   |
| `kv_art_pending_retires`                     | gauge   | `headless_node.cpp` (D-3)       | spikes during writes; should drain to ~0 |
| `kv_art_global_epoch`                        | gauge   | `headless_node.cpp` (D-3)       | monotonic; rate matches write rate       |

All gauges refresh on every `kv_metrics_scrape` call (the ART
gauges via `HeadlessNode::RefreshArtGauges`); Prometheus scraping
the `/metrics` endpoint sees current values.

---

## KVCachePinnedTierSaturated — severity **warning**

```yaml
expr: kv_pinned_tier_slots_utilization_ratio > 0.9
for: 5m
```

### What it means

The pinned-tier slot pool is the bounded resource backing
`kv_reserve` — every in-flight Reserve holds one slot until the
matching `kv_seal` (or `kv_release` on a cancellation) returns it.
Sustained > 90% means the engine is producing Reserves faster than
it's completing Seals; the headroom for traffic bursts is gone.

### Investigate

1. Confirm it's sustained, not a momentary burst:
   ```bash
   kubectl exec -n {ns} {node-pod} -- curl -s localhost:8080/metrics \
     | grep kv_pinned_tier_slots
   ```
   You should see `slots_in_use` very close to `slots_total`.
2. Check the engine's save / publish rate vs the seal rate. The
   gap is how many slots are stuck in-flight at any moment.
3. Look at `kv_reserves_total` rate over the last 30 minutes —
   sudden uptick = traffic spike; flat-high = engine misbehaving.

### Common causes

- **Pool undersized for the workload**: default is 1 GiB / 16 MiB
  slots → 64 slots. A LLama-3-70B engine doing 30 concurrent
  prefills can sit here.
- **Slow downstream tier**: DRAM tier eviction blocking Seal
  completion, so slots stay held longer than expected.
- **Engine bug**: Reserves issued without matching Seal /
  Release. Look for stuck handles in node logs.

### Remediate

- Resize the pinned pool in the AlluxioCluster CR
  (`spec.node.tier.pinned.pool_bytes` or `slot_bytes`).
- If the workload is bursty, increase the pool but also confirm
  the engine respects the C ABI's `KV_E_NOMEM` and retries with
  the `retry-after-ms: 50` trailing metadata (Phase G-4).
- If saturation is sudden + sustained AND `kv_art_leaf_count` is
  spiking, a runaway engine is writing without read traffic to
  drain — stop the misbehaving client.

---

## KVCacheReserveNomemSpike — severity **critical**

```yaml
expr: rate(kv_reserve_nomem_total[2m]) > 0
for: 0m
```

### What it means

At least one `kv_reserve` failed with `KV_E_NOMEM` in the last 2
minutes. Engines see this as a save-side error; their fallback is
to recompute the prefix instead of caching it, which defeats the
point of the KV cache.

### Investigate

1. Pull the current snapshot:
   ```bash
   kubectl exec -n {ns} {node-pod} -- curl -s localhost:8080/metrics \
     | grep -E "kv_reserve_(nomem|invalid)_total|kv_pinned_tier_slots"
   ```
2. If `slots_in_use == slots_total`, the pool is saturated — this
   alert is the consequence of the warning above; resolve that
   first.
3. If `slots_in_use << slots_total` yet NOMEM is firing, the
   Reserve is being rejected by a different path — check node
   logs for `KV_E_NOMEM` traces. There's a defensive NOMEM in
   `HeadlessNode::Reserve` for over-large requests
   (`slot->bytes < bytes`).

### Common causes

- Pool saturation (see KVCachePinnedTierSaturated remediation).
- Caller asking for a Reserve larger than `slot_bytes`. Engine
  config mis-sized — `bytes_per_token × max_tokens` must fit one
  slot.

### Remediate

- Page-worthy: resize the pool or fix the caller. The engine is
  bleeding throughput right now.
- File a feedback ticket if the pool is correctly sized and the
  alert still fires — that's a real engine bug.

---

## KVCacheArtRetireBacklog — severity **warning**

```yaml
expr: kv_art_pending_retires > 100000
for: 10m
```

### What it means

The EBR (Epoch-Based Reclamation) retire list is growing faster
than `EpochManager::Reclaim()` drains it. Each entry holds an
ART node; sustained backlog grows heap and slows the
RefreshArtGauges hot path (it takes the retired_mu_ mutex).

### Investigate

1. Check whether the issue is "writers are very fast" vs
   "reclaim is broken":
   ```
   rate(kv_art_global_epoch[5m]) > 200 ?     # heavy write rate
   ```
   If yes, the backlog is symptom of high churn — that's
   workload-driven, not a bug. The 100k threshold is conservative.
2. If `global_epoch` rate is normal but `pending_retires` keeps
   climbing, Reclaim() isn't being called. Check whether the
   node has a long-running reader holding an old epoch witness
   (this stalls reclamation by design).

### Common causes

- Heavy write workload (D-3 churn bench saw 29.5k retires per
  1.5s — adjust threshold if your workload sustains this).
- Long-lived `ReaderGuard` somewhere — every Lookup creates one
  but should drop it before the next forward step. A leak there
  blocks `MinActiveEpoch` from advancing.

### Remediate

- Confirm the workload is real (it usually is). Raise the
  threshold via Helm values if needed.
- If a reader leak is suspected: `kubectl exec` into the node
  and check `kv_art_pending_retires` over time. If it grows
  monotonically while writes are paused, that confirms a reader
  is holding an epoch; restart the node as a stopgap and file
  feedback.

---

## KVCacheArtWritesStalled — severity **info**

```yaml
expr: |
  rate(kv_art_global_epoch[10m]) == 0
  and kv_art_leaf_count > 0
for: 30m
```

### What it means

The ART has leaves (so the node has done work in the past) but
`global_epoch` hasn't advanced in 10 minutes — no Inserts and
no Removes. After 30 minutes of this, fire informational alert
because it usually means the connector isn't being driven.

### Investigate

1. Are the engine pods actually running and using this node?
   ```bash
   kubectl get pods -n {engine-ns} -l app=vllm
   ```
2. Check engine logs for any errors connecting to the kvcache
   connector. If the engine tripped silent-fallback to local
   KV, it'll stop calling the connector entirely.
3. If multiple kvstore-node pods exist, traffic may have
   migrated to another shard — this one is genuinely idle.

### Common causes

- Engine restarted without the kvcache connector wired.
- Test / canary traffic stopped.
- Multi-shard cluster with skewed routing.

### Remediate

- This is informational — no automatic remediation. If the
  workload SHOULD be running and isn't, drill into the engine
  side.
- If you expect the node to be idle (e.g. development
  environment), silence this alert via Alertmanager.

---

## Tuning the thresholds

All four alerts are exposed in `values.yaml` via the
`observability.prometheus.rules` block. Operators can:

- Disable the whole `kvcache` alert group: `rules.enabled: false`.
- Override per-alert via PrometheusRule patching (kustomize / Helm
  post-render).

The defaults are conservative starting points. After 1–2 weeks of
production data, retune `KVCacheArtRetireBacklog` (workload-dependent)
and the `for:` durations (cluster scrape interval-dependent).
