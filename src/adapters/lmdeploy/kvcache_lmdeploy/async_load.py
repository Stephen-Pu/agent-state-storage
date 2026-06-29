"""Async load driver for the LMDeploy adapter.

Identical in shape to the SGLang / AIBrix / Dynamo drivers — a worker-pool
wrapper around ``KVCacheConnector.fetch`` — reshaped to LMDeploy's
vocabulary. Per-adapter copy by convention (the shared slice is tiny and
the adapters keep independent release cadences); extraction into
kvcache_core is mechanical if the duplication ever costs maintenance.

The driver's async kick-off verb is ``prefetch(rid, key)`` — begin loading
the matched-prefix blocks so a later ``collect`` returns instantly.

Lifecycle: prefetch → finished_ids → pop (block + release) / cancel; close
shuts the pool down and releases remaining handles.
"""
from __future__ import annotations

import concurrent.futures
from dataclasses import dataclass
from typing import Dict, Iterable, Optional, Protocol, Sequence, Set


class _ConnectorLike(Protocol):
    def lookup(self, key: Sequence[int]):  # -> Optional[LookupResult]
        ...

    def fetch(self, handle: int, dst: bytearray) -> int:
        ...

    def wait(self, cid: int) -> None:
        ...

    def release(self, handle: int) -> None:
        ...


@dataclass
class _Entry:
    future: "concurrent.futures.Future[None]"
    handle: int
    staging: bytearray
    matched_tokens: int
    finished: bool = False


class AsyncLoadDriver:
    """Worker-pool wrapper around ``KVCacheConnector.fetch``. Holds the
    connector by reference; does NOT own its lifecycle (the backend does)."""

    def __init__(self, connector: _ConnectorLike, *,
                 bytes_per_token: int, workers: int = 4) -> None:
        if bytes_per_token <= 0:
            raise ValueError("bytes_per_token must be positive")
        if workers <= 0:
            raise ValueError("workers must be positive")
        self._cx = connector
        self._bytes_per_token = bytes_per_token
        self._executor = concurrent.futures.ThreadPoolExecutor(
            max_workers=workers,
            thread_name_prefix="kvcache-lmdeploy-load")
        self._state: Dict[str, _Entry] = {}

    def prefetch(self, request_id: str, key: Sequence[int]) -> int:
        """Sync lookup; on hit dispatch an async Fetch and return
        matched_tokens. On miss returns 0 and schedules nothing. Re-issuing
        the same rid releases the prior in-flight handle first."""
        if request_id in self._state:
            self.cancel(request_id)
        hit = self._cx.lookup(key)
        if hit is None:
            return 0
        matched = int(hit.matched_tokens)
        if matched == 0:
            try:
                self._cx.release(hit.handle)
            except Exception:
                pass
            return 0
        staging = bytearray(matched * self._bytes_per_token)
        future = self._executor.submit(self._fetch_one, hit.handle, staging)
        self._state[request_id] = _Entry(
            future=future, handle=hit.handle, staging=staging,
            matched_tokens=matched)
        return matched

    def _fetch_one(self, handle: int, dst: bytearray) -> None:
        cid = self._cx.fetch(handle, dst)
        self._cx.wait(cid)

    def finished_ids(
        self, candidates: Optional[Iterable[str]] = None
    ) -> Set[str]:
        out: Set[str] = set()
        rids = (list(self._state.keys())
                if candidates is None else list(candidates))
        for rid in rids:
            state = self._state.get(rid)
            if state is None:
                continue
            if state.finished:
                out.add(rid)
                continue
            if state.future.done():
                state.future.result()  # re-raises worker exception
                state.finished = True
                out.add(rid)
        return out

    def pop(self, request_id: str) -> Optional[bytes]:
        state = self._state.pop(request_id, None)
        if state is None:
            return None
        try:
            state.future.result()
        finally:
            try:
                self._cx.release(state.handle)
            except Exception:
                pass
        return bytes(state.staging)

    def cancel(self, request_id: str) -> None:
        state = self._state.pop(request_id, None)
        if state is None:
            return
        if not state.future.done():
            try:
                state.future.result()
            except Exception:
                pass
        try:
            self._cx.release(state.handle)
        except Exception:
            pass

    def in_flight(self) -> int:
        return len(self._state)

    def has(self, request_id: str) -> bool:
        return request_id in self._state

    def matched_tokens(self, request_id: str) -> Optional[int]:
        state = self._state.get(request_id)
        return None if state is None else state.matched_tokens

    def close(self, wait: bool = True) -> None:
        for rid in list(self._state.keys()):
            self.cancel(rid)
        self._executor.shutdown(wait=wait)
