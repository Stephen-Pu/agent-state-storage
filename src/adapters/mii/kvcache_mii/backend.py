"""DeepSpeed-MII (FastGen)-shaped KV cache backend over the Core ABI.

DeepSpeed-MII's FastGen engine manages KV in a **blocked/ragged** cache
(Dynamic SplitFuse). This adapter exposes a cache-store vocabulary —
`query` a prefix, `put` finished KV, `get` it back, `release` a hint —
over the engine-agnostic :class:`KVCacheConnector`, treating kvcache as the
external KV-cache tier MII offloads to.

LLD reference: §6.1.4 (engine adapter strategy).
"""

from __future__ import annotations

from typing import Optional, Sequence, Set

from kvcache_core import KVCacheConnector, compress_retrieve, compress_store

from .async_load import AsyncLoadDriver


class MIIKVCache:
    """DeepSpeed-MII-shaped external KV cache.

    Lifecycle::

        with MIIKVCache(tenant_id="t1", model_id="mixtral-8x7b",
                         bytes_per_token=64) as kv:
            n = kv.query(tokens)          # 0 on miss
            kv.put(tokens, kv_bytes)      # offload finished KV
            blob = kv.get(tokens)         # None on miss
    """

    # MII's FastGen blocks default to 16 tokens — same as our ART chunk.
    BLOCK_TOKENS = 16

    def __init__(self, tenant_id: str, model_id: str,
                 bytes_per_token: int, *, compress: bool = False,
                 compress_bits: int = 8) -> None:
        if bytes_per_token <= 0:
            raise ValueError("bytes_per_token must be positive")
        # B7 — optional lossy KV-tensor compression (shared kvcache_core
        # helper, same codec as the vLLM/SGLang/AIBrix adapters). fp32
        # [tokens][elems], so bytes_per_token must be a whole number of floats.
        if compress and bytes_per_token % 4 != 0:
            raise ValueError("compress requires bytes_per_token to be a "
                             "multiple of 4 (fp32 elements)")
        self._cx = KVCacheConnector(tenant_id=tenant_id, model_id=model_id)
        self._bytes_per_token = bytes_per_token
        self._compress = compress
        self._compress_bits = compress_bits
        self._closed = False

    def query(self, tokens: Sequence[int]) -> int:
        """Matched-prefix token count (block-aligned), or 0 on miss."""
        result = self._cx.lookup(tokens)
        if result is None:
            return 0
        try:
            self._cx.release(result.handle)
        except Exception:
            pass
        return int(result.matched_tokens)

    def put(self, tokens: Sequence[int], kv_bytes: bytes) -> None:
        """Offload the finished KV for ``tokens`` to the external tier."""
        if not tokens:
            raise ValueError("tokens must be non-empty")
        if not kv_bytes:
            raise ValueError("kv_bytes must be non-empty")
        if self._compress:
            compress_store(self._cx, tokens, kv_bytes, self._bytes_per_token,
                           bits=self._compress_bits)
            return
        locator = self._cx.make_locator(tokens)
        rsv = self._cx.reserve(locator, len(kv_bytes))
        if rsv.slot_bytes < len(kv_bytes):
            raise RuntimeError(
                f"reserved slot too small: {rsv.slot_bytes} < {len(kv_bytes)}")
        self._cx.write_into_slot(rsv.slot_addr, kv_bytes)
        self._cx.publish(rsv.handle, watermark=len(kv_bytes))
        self._cx.seal(rsv.handle, tokens)

    def get(self, tokens: Sequence[int]) -> Optional[bytes]:
        """Pull the cached KV for the matched prefix. None on miss."""
        if self._compress:
            return compress_retrieve(self._cx, tokens, self._bytes_per_token)
        hit = self._cx.lookup(tokens)
        if hit is None:
            return None
        n_bytes = int(hit.matched_tokens) * self._bytes_per_token
        buf = bytearray(n_bytes)
        cid = self._cx.fetch(hit.handle, buf)
        self._cx.wait(cid)
        self._cx.release(hit.handle)
        return bytes(buf)

    def release(self, tokens: Sequence[int]) -> bool:
        """Hint that ``tokens`` are cold. No explicit Drop verb in the MVP
        ABI (capacity + refcount drive eviction); always False."""
        del tokens
        return False

    def close(self) -> None:
        if not self._closed:
            self._cx.close()
            self._closed = True

    def __enter__(self) -> "MIIKVCache":
        return self

    def __exit__(self, *_: object) -> None:
        self.close()


class AsyncMIIKVCache:
    """DeepSpeed-MII-shaped KV cache with an async get path.

    Adds ``schedule_load`` / ``finished_ids`` / ``collect`` / ``cancel`` so
    the FastGen scheduler can overlap KV load with compute. ``query`` /
    ``put`` / ``release`` stay sync; ``get`` stays sync as a fallback.
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

    def query(self, tokens: Sequence[int]) -> int:
        result = self._cx.lookup(tokens)
        if result is None:
            return 0
        try:
            self._cx.release(result.handle)
        except Exception:
            pass
        return int(result.matched_tokens)

    def put(self, tokens: Sequence[int], kv_bytes: bytes) -> None:
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

    def get(self, tokens: Sequence[int]) -> Optional[bytes]:
        hit = self._cx.lookup(tokens)
        if hit is None:
            return None
        n_bytes = int(hit.matched_tokens) * self._bytes_per_token
        buf = bytearray(n_bytes)
        cid = self._cx.fetch(hit.handle, buf)
        self._cx.wait(cid)
        self._cx.release(hit.handle)
        return bytes(buf)

    def release(self, tokens: Sequence[int]) -> bool:
        del tokens
        return False

    def schedule_load(self, request_id: str, tokens: Sequence[int]) -> int:
        """Sync query + async load. Returns matched_tokens; 0 = miss
        (nothing scheduled)."""
        return self._driver.prefetch(request_id, tokens)

    def finished_ids(self, candidates=None) -> Set[str]:
        return self._driver.finished_ids(candidates)

    def collect(self, request_id: str) -> Optional[bytes]:
        return self._driver.pop(request_id)

    def cancel(self, request_id: str) -> None:
        self._driver.cancel(request_id)

    def in_flight(self) -> int:
        return self._driver.in_flight()

    def close(self) -> None:
        if not self._closed:
            self._driver.close(wait=True)
            self._cx.close()
            self._closed = True

    def __enter__(self) -> "AsyncMIIKVCache":
        return self

    def __exit__(self, *_: object) -> None:
        self.close()
