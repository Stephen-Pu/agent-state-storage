"""Phase P-3.2 — async load driver.

Worker-thread orchestration for pre-fetching cached KV blobs on a cache
hit. The vLLM bridge owns one of these and forwards calls. Kept
**vllm-import-free** so the executor + future-tracking logic gets real
unit coverage on the default dev rig (no vLLM dependency).

Lifecycle (mirrors the bridge's lifecycle calls):

  1. ``kick_off(rid, staging)`` — bridge calls this from
     ``get_num_new_matched_tokens`` immediately after a hit. A worker
     thread starts driving ``inner.start_load_kv`` +
     ``inner.wait_for_layer_load`` into the supplied staging buffer.

  2. ``finished_ids(candidates)`` — bridge calls this from
     ``get_finished``. Returns the subset of ``candidates`` whose
     fetch future has resolved. Idempotent — a rid already reported
     stays reported until popped.

  3. ``pop_staging(rid)`` — bridge calls this from ``start_load_kv``.
     Blocks on the future (defensive — engine may have skipped
     ``get_finished``) and returns the staged bytes for the splitter.

  4. ``cancel(rid)`` — bridge calls this from ``request_finished``.
     Drops in-flight state; blocks on a running future first so we
     don't free the inner handle while a worker still references it.

  5. ``close(wait)`` — bridge calls this on shutdown.
"""
from __future__ import annotations

import concurrent.futures
from dataclasses import dataclass
from typing import Dict, Iterable, Optional, Set


@dataclass
class _Entry:
    future: "concurrent.futures.Future[None]"
    staging: bytearray
    finished: bool = False


class AsyncLoadDriver:
    """Owns a thread pool that pre-fetches saved blobs on cache hits.

    ``inner`` is any object exposing ``start_load_kv(rid, dst) -> int``
    (returns a completion id) and ``wait_for_layer_load(rid, cid)``.
    The in-tree :class:`VllmKVConnector` satisfies the protocol; tests
    can also pass a small mock.
    """

    def __init__(self, inner, *, workers: int = 4) -> None:
        if workers <= 0:
            raise ValueError("workers must be positive")
        self._inner = inner
        self._executor = concurrent.futures.ThreadPoolExecutor(
            max_workers=workers,
            thread_name_prefix="kvcache-load")
        self._state: Dict[str, _Entry] = {}

    # ---- per-request lifecycle ----------------------------------------

    def kick_off(self, request_id: str, staging: bytearray) -> None:
        """Schedule a Fetch into ``staging`` on a worker thread.
        Overwrites any existing in-flight state for the same rid —
        the caller has already decided this is a fresh kick-off
        (e.g. a re-prefill after cancellation).
        """
        future = self._executor.submit(
            self._fetch, request_id, staging)
        self._state[request_id] = _Entry(future=future, staging=staging)

    def _fetch(self, request_id: str, staging: bytearray) -> None:
        cid = self._inner.start_load_kv(request_id, staging)
        self._inner.wait_for_layer_load(request_id, cid)

    def finished_ids(
        self, candidates: Optional[Iterable[str]] = None
    ) -> Set[str]:
        """Return the subset of ``candidates`` whose fetch future has
        resolved. If ``candidates`` is ``None``, scans every in-flight
        request. Surfaces worker-thread exceptions synchronously so a
        failed Fetch turns into a visible engine error instead of an
        infinite poll.
        """
        out: Set[str] = set()
        if candidates is None:
            rids = list(self._state.keys())
        else:
            rids = list(candidates)
        for rid in rids:
            state = self._state.get(rid)
            if state is None:
                continue
            if state.finished:
                out.add(rid)
                continue
            if state.future.done():
                state.future.result()
                state.finished = True
                out.add(rid)
        return out

    def pop_staging(self, request_id: str) -> Optional[bytes]:
        """Block on the request's future and return the staged bytes.
        Removes the entry. Returns ``None`` if the rid was never
        kicked off — caller should fall through to the sync path.
        """
        state = self._state.pop(request_id, None)
        if state is None:
            return None
        if not state.future.done():
            state.future.result()
        return bytes(state.staging)

    def cancel(self, request_id: str) -> None:
        """Drop in-flight state for ``request_id``. Blocks on a
        running future first so the worker thread isn't still
        touching the inner connector when the caller releases the
        underlying handle.
        """
        state = self._state.pop(request_id, None)
        if state is None:
            return
        if not state.future.done():
            try:
                state.future.result()
            except Exception:
                # Cancel is best-effort cleanup — swallow worker
                # exceptions so they don't take down the engine's
                # cleanup path.
                pass

    # ---- inspection ---------------------------------------------------

    def in_flight(self) -> int:
        return len(self._state)

    def has(self, request_id: str) -> bool:
        return request_id in self._state

    # ---- shutdown -----------------------------------------------------

    def close(self, wait: bool = True) -> None:
        """Shut down the worker pool. ``wait=True`` blocks until all
        in-flight fetches complete; ``wait=False`` returns
        immediately (workers finish in the background)."""
        self._executor.shutdown(wait=wait)
        self._state.clear()
