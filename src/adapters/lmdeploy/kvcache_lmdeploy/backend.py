"""LMDeploy-shaped KV cache backend over the Core ABI.

LMDeploy's TurboMind engine manages KV as **paged blocks** with a prefix
cache (`BlockManager` + block trie). This adapter treats kvcache as the
external L2 tier those blocks spill to / are restored from, exposing a
block-cache vocabulary — `match` a prefix, `add` finished blocks, `get`
matched blocks back, `free` a hint — over the engine-agnostic
:class:`KVCacheConnector`.

LLD reference: §6.1.4 (engine adapter strategy).
"""

from __future__ import annotations

from typing import Optional, Sequence, Set

from kvcache_core import KVCacheConnector, compress_retrieve, compress_store

from .async_load import AsyncLoadDriver


class LMDeployBlockCache:
    """TurboMind-shaped external block cache.

    Lifecycle::

        with LMDeployBlockCache(tenant_id="t1", model_id="internlm2-20b",
                                 bytes_per_token=64) as bc:
            n = bc.match(tokens)          # 0 on miss, else block-aligned
            bc.add(tokens, kv_bytes)      # spill finished blocks to L2
            blocks = bc.get(tokens)       # None on miss
    """

    # TurboMind's default block holds 16 tokens — same chunking as our ART.
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

    def match(self, tokens: Sequence[int]) -> int:
        """Block-aligned matched-prefix token count, or 0 on miss."""
        result = self._cx.lookup(tokens)
        if result is None:
            return 0
        try:
            self._cx.release(result.handle)
        except Exception:
            pass
        return int(result.matched_tokens)

    def add(self, tokens: Sequence[int], kv_bytes: bytes) -> None:
        """Spill the finished blocks for ``tokens`` to the external tier."""
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
        """Restore the cached blocks for the matched prefix. None on miss."""
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

    def free(self, tokens: Sequence[int]) -> bool:
        """Hint that ``tokens``' blocks are cold. No explicit Drop verb in
        the MVP ABI (eviction is capacity + refcount driven); always False."""
        del tokens
        return False

    def close(self) -> None:
        if not self._closed:
            self._cx.close()
            self._closed = True

    def __enter__(self) -> "LMDeployBlockCache":
        return self

    def __exit__(self, *_: object) -> None:
        self.close()


class AsyncLMDeployBlockCache:
    """TurboMind-shaped block cache with an async restore path.

    Adds ``prefetch`` / ``finished_ids`` / ``collect`` / ``cancel`` so the
    scheduler can overlap block restore with prior compute. ``match`` /
    ``add`` / ``free`` stay sync; ``get`` stays sync as a fallback.
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

    def match(self, tokens: Sequence[int]) -> int:
        result = self._cx.lookup(tokens)
        if result is None:
            return 0
        try:
            self._cx.release(result.handle)
        except Exception:
            pass
        return int(result.matched_tokens)

    def add(self, tokens: Sequence[int], kv_bytes: bytes) -> None:
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

    def free(self, tokens: Sequence[int]) -> bool:
        del tokens
        return False

    def prefetch(self, request_id: str, tokens: Sequence[int]) -> int:
        """Sync match + async block restore. Returns matched_tokens; 0 =
        miss (nothing scheduled)."""
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

    def __enter__(self) -> "AsyncLMDeployBlockCache":
        return self

    def __exit__(self, *_: object) -> None:
        self.close()
