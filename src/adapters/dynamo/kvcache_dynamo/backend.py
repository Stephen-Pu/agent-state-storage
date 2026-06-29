"""NVIDIA Dynamo-shaped KV cache backend over the Core ABI.

Dynamo's **KVBM** (KV Block Manager) is the component that moves finished
KV blocks down a memory hierarchy (G1 device → G2 host → G3 disk/remote)
and pulls matched-prefix blocks back up on a cache hit. Its connector
surface is block-granular and tier-aware: *match* a prefix, *offload*
finished blocks to the next tier down, *onboard* matched blocks back up.

This adapter maps that vocabulary onto the engine-agnostic
:class:`KVCacheConnector` (``connector.py``), treating kvcache as the
external L2/L3 tier KVBM offloads to. Dynamo "already uses NIXL", so the
server-pull data path lines up naturally — KVBM's onboard becomes our
Lookup → Fetch, and its offload becomes Reserve → write → Publish → Seal.

Distinct from the vLLM adapter: that one implements vLLM's *per-layer* V1
KVConnector (save_kv_layer / start_load_kv). KVBM sits a level up — it's
the block-manager tier-mover, not the per-layer hook — so the surface
here is block/prefix-granular, not layer-granular.

LLD reference: §6.1.4 (engine adapter strategy).
"""

from __future__ import annotations

from typing import Optional, Sequence, Set

from kvcache_core import KVCacheConnector

from .async_load import AsyncLoadDriver


class DynamoKVBMConnector:
    """KVBM-shaped L2/L3 tier backend with Dynamo's expected verbs.

    Lifecycle::

        with DynamoKVBMConnector(tenant_id="t1", model_id="llama-3-70b",
                                  bytes_per_token=64) as kvbm:
            matched = kvbm.match(tokens)          # 0 on miss
            kvbm.offload(tokens, kv_bytes)         # push finished blocks down
            blocks = kvbm.onboard(tokens)          # None on miss
    """

    # Dynamo's KVBM blocks default to 16 tokens — same chunking as our ART.
    BLOCK_TOKENS = 16

    def __init__(self, tenant_id: str, model_id: str,
                 bytes_per_token: int) -> None:
        if bytes_per_token <= 0:
            raise ValueError("bytes_per_token must be positive")
        self._cx = KVCacheConnector(tenant_id=tenant_id, model_id=model_id)
        self._bytes_per_token = bytes_per_token
        self._closed = False

    # ----- KVBM-style verbs ------------------------------------------------

    def match(self, tokens: Sequence[int]) -> int:
        """Longest-prefix-match block count (in tokens), or 0 on miss.

        KVBM calls this to decide how many leading blocks of a request it
        can onboard from a lower tier instead of recomputing. The count is
        always a multiple of :pyattr:`BLOCK_TOKENS` — partial blocks are
        dropped by LPM.
        """
        result = self._cx.lookup(tokens)
        if result is None:
            return 0
        # match() is a read-only probe; release the handle it minted.
        try:
            self._cx.release(result.handle)
        except Exception:
            pass
        return int(result.matched_tokens)

    def offload(self, tokens: Sequence[int], kv_bytes: bytes) -> None:
        """Push the finished blocks for ``tokens`` down to the kvcache tier.

        KVBM calls this when a sequence's blocks are evicted from G1/G2 but
        worth keeping for prefix reuse. The bytes are opaque;
        ``bytes_per_token`` controls how :py:meth:`onboard` slices the
        matched prefix back out.
        """
        if not tokens:
            raise ValueError("tokens must be non-empty")
        if not kv_bytes:
            raise ValueError("kv_bytes must be non-empty")
        locator = self._cx.make_locator(tokens)
        rsv = self._cx.reserve(locator, len(kv_bytes))
        if rsv.slot_bytes < len(kv_bytes):
            raise RuntimeError(
                f"reserved slot too small: {rsv.slot_bytes} < {len(kv_bytes)}")
        self._cx.write_into_slot(rsv.slot_addr, kv_bytes)
        self._cx.publish(rsv.handle, watermark=len(kv_bytes))
        self._cx.seal(rsv.handle, tokens)

    def onboard(self, tokens: Sequence[int]) -> Optional[bytes]:
        """Pull the cached blocks for the LPM-matched prefix back up.

        Returns ``None`` on miss. On hit, the buffer covers
        ``matched_tokens * bytes_per_token`` bytes — the matched prefix,
        which may be shorter than the requested ``tokens``.
        """
        hit = self._cx.lookup(tokens)
        if hit is None:
            return None
        n_bytes = int(hit.matched_tokens) * self._bytes_per_token
        buf = bytearray(n_bytes)
        cid = self._cx.fetch(hit.handle, buf)
        self._cx.wait(cid)
        self._cx.release(hit.handle)
        return bytes(buf)

    def evict(self, tokens: Sequence[int]) -> bool:
        """Best-effort hint that ``tokens``' blocks are no longer hot.

        The MVP Core ABI has no explicit Drop verb — eviction is driven by
        tier capacity + refcount. We surface KVBM's expected signature so
        engine wiring needs no conditional; the return is always ``False``
        ("no immediate action").
        """
        del tokens
        return False

    # ----- lifecycle -------------------------------------------------------

    def close(self) -> None:
        if not self._closed:
            self._cx.close()
            self._closed = True

    def __enter__(self) -> "DynamoKVBMConnector":
        return self

    def __exit__(self, *_: object) -> None:
        self.close()


class AsyncDynamoKVBMConnector:
    """KVBM-shaped backend with an async onboard path.

    Same verb set as :class:`DynamoKVBMConnector` plus four methods —
    ``start_onboard`` / ``finished_ids`` / ``take`` / ``cancel`` — that let
    the Dynamo scheduler overlap KV-onboard with prior compute. ``match``,
    ``offload`` and ``evict`` stay sync (cheap / off the critical path);
    ``onboard`` stays sync as a fallback for callers not driving the async
    lifecycle.

    Typical KVBM scheduler use::

        with AsyncDynamoKVBMConnector(tenant_id="t1", model_id="m",
                                       bytes_per_token=64) as kvbm:
            matched = kvbm.start_onboard("req-7", tokens)  # sync match,
                                                            # async fetch
            if matched == 0:
                ...  # recompute the whole prompt
            else:
                ...  # begin model setup; the onboard runs in parallel
                if "req-7" in kvbm.finished_ids({"req-7"}):
                    blocks = kvbm.take("req-7")
                else:
                    blocks = kvbm.take("req-7")  # block-and-collect
    """

    BLOCK_TOKENS = 16

    def __init__(self, tenant_id: str, model_id: str,
                 bytes_per_token: int, *, workers: int = 4) -> None:
        if bytes_per_token <= 0:
            raise ValueError("bytes_per_token must be positive")
        self._cx = KVCacheConnector(tenant_id=tenant_id, model_id=model_id)
        self._bytes_per_token = bytes_per_token
        self._driver = AsyncLoadDriver(
            self._cx, bytes_per_token=bytes_per_token, workers=workers)
        self._closed = False

    # ----- sync verbs (delegate to connector directly) ---------------------

    def match(self, tokens: Sequence[int]) -> int:
        result = self._cx.lookup(tokens)
        if result is None:
            return 0
        try:
            self._cx.release(result.handle)
        except Exception:
            pass
        return int(result.matched_tokens)

    def offload(self, tokens: Sequence[int], kv_bytes: bytes) -> None:
        if not tokens:
            raise ValueError("tokens must be non-empty")
        if not kv_bytes:
            raise ValueError("kv_bytes must be non-empty")
        locator = self._cx.make_locator(tokens)
        rsv = self._cx.reserve(locator, len(kv_bytes))
        if rsv.slot_bytes < len(kv_bytes):
            raise RuntimeError(
                f"reserved slot too small: {rsv.slot_bytes} < {len(kv_bytes)}")
        self._cx.write_into_slot(rsv.slot_addr, kv_bytes)
        self._cx.publish(rsv.handle, watermark=len(kv_bytes))
        self._cx.seal(rsv.handle, tokens)

    def onboard(self, tokens: Sequence[int]) -> Optional[bytes]:
        hit = self._cx.lookup(tokens)
        if hit is None:
            return None
        n_bytes = int(hit.matched_tokens) * self._bytes_per_token
        buf = bytearray(n_bytes)
        cid = self._cx.fetch(hit.handle, buf)
        self._cx.wait(cid)
        self._cx.release(hit.handle)
        return bytes(buf)

    def evict(self, tokens: Sequence[int]) -> bool:
        del tokens
        return False

    # ----- async onboard path ----------------------------------------------

    def start_onboard(self, request_id: str, tokens: Sequence[int]) -> int:
        """Sync match + async fetch. Returns matched_tokens; 0 = miss (no
        work scheduled, caller falls through to recompute)."""
        return self._driver.start_onboard(request_id, tokens)

    def finished_ids(self, candidates=None) -> Set[str]:
        return self._driver.finished_ids(candidates)

    def take(self, request_id: str) -> Optional[bytes]:
        return self._driver.pop(request_id)

    def cancel(self, request_id: str) -> None:
        self._driver.cancel(request_id)

    def in_flight(self) -> int:
        return self._driver.in_flight()

    # ----- lifecycle -------------------------------------------------------

    def close(self) -> None:
        if not self._closed:
            self._driver.close(wait=True)
            self._cx.close()
            self._closed = True

    def __enter__(self) -> "AsyncDynamoKVBMConnector":
        return self

    def __exit__(self, *_: object) -> None:
        self.close()
