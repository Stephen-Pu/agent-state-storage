"""AsyncLoadDriver + AsyncLMDeployBlockCache tests (fake connector for
lifecycle/threading; one real C-ABI round-trip at the bottom)."""

from __future__ import annotations

import os
import shutil
import threading
import time
from dataclasses import dataclass

import pytest

from kvcache_lmdeploy import AsyncLoadDriver

BPT = 4


@dataclass
class _Hit:
    handle: int
    matched_tokens: int


class FakeConnector:
    def __init__(self) -> None:
        self.stored: dict[tuple[int, ...], bytes] = {}
        self.released: list[int] = []
        self._next_handle = 1000
        self.fetch_gate = threading.Event()
        self.fetch_gate.set()
        self.fetch_calls = 0
        self.fetch_fail_next = False

    def store(self, key, blob: bytes) -> None:
        self.stored[tuple(key)] = blob

    def lookup(self, key):
        toks = tuple(key)
        best = None
        for stored in self.stored:
            n = min(len(toks), len(stored))
            if toks[:n] == stored[:n] and n > 0 and (best is None or n > best[0]):
                best = (n, stored)
        if best is None:
            return None
        self._next_handle += 1
        return _Hit(handle=self._next_handle, matched_tokens=best[0])

    def fetch(self, handle: int, dst: bytearray) -> int:
        self.fetch_calls += 1
        self.fetch_gate.wait(timeout=5.0)
        if self.fetch_fail_next:
            self.fetch_fail_next = False
            raise RuntimeError("fetch boom")
        for i in range(len(dst)):
            dst[i] = handle & 0xFF
        return handle

    def wait(self, cid: int) -> None:
        del cid

    def release(self, handle: int) -> None:
        self.released.append(handle)


def test_prefetch_miss_schedules_nothing():
    d = AsyncLoadDriver(FakeConnector(), bytes_per_token=BPT)
    assert d.prefetch("r", [1, 2, 3]) == 0
    assert d.in_flight() == 0


def test_prefetch_hit_then_pop():
    fc = FakeConnector()
    fc.store([1, 2, 3, 4], b"\x00" * (4 * BPT))
    d = AsyncLoadDriver(fc, bytes_per_token=BPT)
    assert d.prefetch("r", [1, 2, 3, 4, 99]) == 4
    out = d.pop("r")
    assert out is not None and len(out) == 4 * BPT
    assert d.in_flight() == 0 and fc.released


def test_prefetch_is_async():
    fc = FakeConnector()
    fc.store([7], b"x" * BPT)
    fc.fetch_gate.clear()
    d = AsyncLoadDriver(fc, bytes_per_token=BPT)
    t0 = time.monotonic()
    assert d.prefetch("r", [7]) == 1
    assert time.monotonic() - t0 < 0.5
    assert not d.finished_ids({"r"})
    fc.fetch_gate.set()
    assert d.pop("r") is not None


def test_finished_ids_idempotent():
    fc = FakeConnector()
    fc.store([1], b"a" * BPT)
    d = AsyncLoadDriver(fc, bytes_per_token=BPT)
    d.prefetch("a", [1])
    for _ in range(100):
        if d.finished_ids({"a"}) == {"a"}:
            break
        time.sleep(0.005)
    assert d.finished_ids({"a"}) == {"a"}
    d.pop("a")
    assert d.finished_ids({"a"}) == set()


def test_cancel_releases_handle():
    fc = FakeConnector()
    fc.store([1], b"a" * BPT)
    d = AsyncLoadDriver(fc, bytes_per_token=BPT)
    d.prefetch("r", [1])
    d.cancel("r")
    assert d.in_flight() == 0 and fc.released
    d.cancel("nope")


def test_worker_exception_surfaces():
    fc = FakeConnector()
    fc.store([1], b"a" * BPT)
    fc.fetch_fail_next = True
    d = AsyncLoadDriver(fc, bytes_per_token=BPT)
    d.prefetch("r", [1])
    deadline = time.monotonic() + 1.0
    raised = None
    while time.monotonic() < deadline:
        try:
            if d._state["r"].future.done():
                d.finished_ids({"r"})
        except RuntimeError as e:
            raised = e
            break
        time.sleep(0.005)
    assert raised is not None and "boom" in str(raised)


def test_close_releases():
    fc = FakeConnector()
    fc.store([1], b"a" * BPT)
    d = AsyncLoadDriver(fc, bytes_per_token=BPT)
    d.prefetch("r", [1])
    d.close(wait=True)
    assert fc.released


def test_constructor_validates_args():
    with pytest.raises(ValueError):
        AsyncLoadDriver(FakeConnector(), bytes_per_token=0)
    with pytest.raises(ValueError):
        AsyncLoadDriver(FakeConnector(), bytes_per_token=4, workers=0)


# ----- e2e against real C ABI -----

BLOCK = 16
BYTES_PER_TOKEN = 64


def _have_library() -> bool:
    if os.environ.get("KVCACHE_LIB"):
        return os.path.isfile(os.environ["KVCACHE_LIB"])
    return shutil.which("cmake") is not None


pytest.importorskip("cffi", reason="cffi is required for the C ABI tests")


@pytest.mark.skipif(not _have_library(), reason="libkvcache.so not available")
def test_async_prefetch_collect_round_trip():
    from kvcache_lmdeploy import AsyncLMDeployBlockCache
    key = list(range(2 * BLOCK))
    payload = bytes(((i * 23) & 0xFF
                      for i in range(len(key) * BYTES_PER_TOKEN)))
    with AsyncLMDeployBlockCache(tenant_id="lmdeploy-async",
                                 model_id="lmdeploy-async-demo",
                                 bytes_per_token=BYTES_PER_TOKEN,
                                 workers=2) as bc:
        bc.add(key, payload)
        assert bc.prefetch("r1", key) == 2 * BLOCK
        assert bc.collect("r1") == payload
        assert bc.in_flight() == 0
