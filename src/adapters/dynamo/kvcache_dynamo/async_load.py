"""Async onboard driver for the Dynamo KVBM adapter.

Mirrors the SGLang / AIBrix async drivers — same connector-protocol
shape, same lifecycle invariants — reshaped to Dynamo KVBM's naming.
The class lives in this package (not in kvcache_core) by intent: the
adapters keep standalone release cadences and the shared slice is tiny;
if the duplication ever becomes a maintenance cost the extraction into
kvcache_core is mechanical.

KVBM's async onboard verb is ``start_onboard(tokens)`` — begin pulling
the matched-prefix blocks up from the kvcache tier so a later ``take``
returns instantly. The driver method (``start_onboard(rid, tokens)``) is
the generic kick-off renamed to track the KVBM vocabulary.

Lifecycle (per request):

  1. ``start_onboard(request_id, tokens)`` — sync match (so we know the
     matched_tokens count the caller reports to its prompt-truncation
     logic), async Fetch onto a worker thread. Returns matched_tokens;
     0 = nothing scheduled (caller recomputes).
  2. ``finished_ids(candidates)`` — poll which onboards have resolved.
     Idempotent. Surfaces worker exceptions so a failed Fetch becomes a
     visible engine error.
  3. ``pop(request_id)`` — block on the future, release the inner
     handle, return the staged bytes (exposed as ``take`` on the backend).
  4. ``cancel(request_id)`` — drop in-flight state; blocks on a running
     future first so the worker isn't still touching the connector when
     we release the handle.
  5. ``close(wait)`` — shut down the worker pool; release remaining
     handles regardless of ``wait``.
"""
from __future__ import annotations

import concurrent.futures
from dataclasses import dataclass
from typing import Dict, Iterable, Optional, Protocol, Sequence, Set


class _ConnectorLike(Protocol):
    """Minimum slice of KVCacheConnector the driver depends on."""

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
    """Worker-pool wrapper around ``KVCacheConnector.fetch`` shaped for
    KVBM's onboard lifecycle. The connector is held by reference; the
    driver does NOT own its lifecycle (the backend owns it).
    """

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
            thread_name_prefix="kvcache-dynamo-load")
        self._state: Dict[str, _Entry] = {}

    # ---- per-request lifecycle --------------------------------------------

    def start_onboard(self, request_id: str, key: Sequence[int]) -> int:
        """Sync match; on hit dispatch an async Fetch and return the
        matched_tokens count. On miss returns 0 and schedules nothing.

        Re-onboarding the same rid releases the prior in-flight handle
        before installing the new one, so a re-issue after cancel (or a
        same-rid back-to-back) never leaks a handle.
        """
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
        """Return the subset of ``candidates`` (or all in-flight rids when
        ``None``) whose Fetch future has resolved. Worker exceptions
        surface here. Idempotent until ``pop`` / ``cancel`` removes the rid.
        """
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
        """Block on the request's future, release the inner handle, return
        the staged bytes. Removes the entry. ``None`` if never onboarded.
        """
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
        """Drop in-flight state without returning bytes. Blocks on a
        running future first so the worker isn't still touching the inner
        connector when we release the handle.
        """
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

    # ---- inspection -------------------------------------------------------

    def in_flight(self) -> int:
        return len(self._state)

    def has(self, request_id: str) -> bool:
        return request_id in self._state

    def matched_tokens(self, request_id: str) -> Optional[int]:
        state = self._state.get(request_id)
        return None if state is None else state.matched_tokens

    # ---- shutdown ---------------------------------------------------------

    def close(self, wait: bool = True) -> None:
        """Shut down the worker pool. ``wait=True`` blocks until all
        in-flight Fetches complete. All remaining handles are released
        regardless of ``wait``.
        """
        rids = list(self._state.keys())
        for rid in rids:
            self.cancel(rid)
        self._executor.shutdown(wait=wait)
