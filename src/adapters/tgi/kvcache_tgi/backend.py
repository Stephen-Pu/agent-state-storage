"""TGI (HuggingFace Text Generation Inference)-shaped KV cache backend.

TGI v3 added a radix-style **prefix cache** to its router/server. This
adapter exposes that vocabulary — `prefix_lookup` a prompt, `insert`
finished KV, `load` the matched prefix back, `evict` a hint — over the
engine-agnostic :class:`KVCacheConnector`, treating kvcache as the external
prefix-cache tier.

LLD reference: §6.1.4 (engine adapter strategy).
"""

from __future__ import annotations

from typing import Optional, Sequence, Set

from kvcache_core import KVCacheConnector, compress_retrieve, compress_store

from .async_load import AsyncLoadDriver


class TGIPrefixCache:
    """TGI-shaped external prefix cache.

    Lifecycle::

        with TGIPrefixCache(tenant_id="t1", model_id="llama-3-8b",
                             bytes_per_token=64) as pc:
            n = pc.prefix_lookup(input_ids)     # 0 on miss
            pc.insert(input_ids, kv_bytes)      # commit finished KV
            kv = pc.load(input_ids)             # None on miss
    """

    # TGI's prefix cache blocks default to 16 tokens — same as our ART chunk.
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

    def prefix_lookup(self, input_ids: Sequence[int]) -> int:
        """Matched-prefix token count (block-aligned), or 0 on miss. TGI
        uses this to skip prefilling the cached prefix."""
        result = self._cx.lookup(input_ids)
        if result is None:
            return 0
        try:
            self._cx.release(result.handle)
        except Exception:
            pass
        return int(result.matched_tokens)

    def insert(self, input_ids: Sequence[int], kv_bytes: bytes) -> None:
        """Commit ``kv_bytes`` as the cached KV for ``input_ids``."""
        if not input_ids:
            raise ValueError("input_ids must be non-empty")
        if not kv_bytes:
            raise ValueError("kv_bytes must be non-empty")
        if self._compress:
            compress_store(self._cx, input_ids, kv_bytes, self._bytes_per_token,
                           bits=self._compress_bits)
            return
        locator = self._cx.make_locator(input_ids)
        rsv = self._cx.reserve(locator, len(kv_bytes))
        if rsv.slot_bytes < len(kv_bytes):
            raise RuntimeError(
                f"reserved slot too small: {rsv.slot_bytes} < {len(kv_bytes)}")
        self._cx.write_into_slot(rsv.slot_addr, kv_bytes)
        self._cx.publish(rsv.handle, watermark=len(kv_bytes))
        self._cx.seal(rsv.handle, input_ids)

    def load(self, input_ids: Sequence[int]) -> Optional[bytes]:
        """Load the cached KV for the matched prefix. None on miss."""
        if self._compress:
            return compress_retrieve(self._cx, input_ids, self._bytes_per_token)
        hit = self._cx.lookup(input_ids)
        if hit is None:
            return None
        n_bytes = int(hit.matched_tokens) * self._bytes_per_token
        buf = bytearray(n_bytes)
        cid = self._cx.fetch(hit.handle, buf)
        self._cx.wait(cid)
        self._cx.release(hit.handle)
        return bytes(buf)

    def evict(self, input_ids: Sequence[int]) -> bool:
        """Hint that ``input_ids`` are cold. No explicit Drop verb in the
        MVP ABI (capacity + refcount drive eviction); always False."""
        del input_ids
        return False

    def close(self) -> None:
        if not self._closed:
            self._cx.close()
            self._closed = True

    def __enter__(self) -> "TGIPrefixCache":
        return self

    def __exit__(self, *_: object) -> None:
        self.close()


class AsyncTGIPrefixCache:
    """TGI-shaped prefix cache with an async load path.

    Adds ``prefill`` / ``finished_ids`` / ``take`` / ``cancel`` so the
    router can overlap KV load with scheduling. ``prefix_lookup`` /
    ``insert`` / ``evict`` stay sync; ``load`` stays sync as a fallback.
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

    def prefix_lookup(self, input_ids: Sequence[int]) -> int:
        result = self._cx.lookup(input_ids)
        if result is None:
            return 0
        try:
            self._cx.release(result.handle)
        except Exception:
            pass
        return int(result.matched_tokens)

    def insert(self, input_ids: Sequence[int], kv_bytes: bytes) -> None:
        if not input_ids:
            raise ValueError("input_ids must be non-empty")
        if not kv_bytes:
            raise ValueError("kv_bytes must be non-empty")
        locator = self._cx.make_locator(input_ids)
        rsv = self._cx.reserve(locator, len(kv_bytes))
        if rsv.slot_bytes < len(kv_bytes):
            raise RuntimeError(
                f"reserved slot too small: {rsv.slot_bytes} < {len(kv_bytes)}")
        self._cx.write_into_slot(rsv.slot_addr, kv_bytes)
        self._cx.publish(rsv.handle, watermark=len(kv_bytes))
        self._cx.seal(rsv.handle, input_ids)

    def load(self, input_ids: Sequence[int]) -> Optional[bytes]:
        hit = self._cx.lookup(input_ids)
        if hit is None:
            return None
        n_bytes = int(hit.matched_tokens) * self._bytes_per_token
        buf = bytearray(n_bytes)
        cid = self._cx.fetch(hit.handle, buf)
        self._cx.wait(cid)
        self._cx.release(hit.handle)
        return bytes(buf)

    def evict(self, input_ids: Sequence[int]) -> bool:
        del input_ids
        return False

    def prefill(self, request_id: str, input_ids: Sequence[int]) -> int:
        """Sync prefix_lookup + async load. Returns matched_tokens; 0 =
        miss (nothing scheduled)."""
        return self._driver.prefetch(request_id, input_ids)

    def finished_ids(self, candidates=None) -> Set[str]:
        return self._driver.finished_ids(candidates)

    def take(self, request_id: str) -> Optional[bytes]:
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

    def __enter__(self) -> "AsyncTGIPrefixCache":
        return self

    def __exit__(self, *_: object) -> None:
        self.close()
