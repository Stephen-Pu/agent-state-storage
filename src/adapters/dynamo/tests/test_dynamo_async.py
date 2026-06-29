"""AsyncLoadDriver + AsyncDynamoKVBMConnector tests.

Mirrors the SGLang / AIBrix async tests — fake-connector lifecycle /
threading invariants (no libkvcache.so), one real C-ABI round-trip at the
bottom — reshaped to KVBM's start_onboard / take naming.
"""

from __future__ import annotations

import os
import shutil
import threading
import time
from dataclasses import dataclass

import pytest

from kvcache_dynamo import AsyncLoadDriver


# ----- fake connector --------------------------------------------------------

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
        self.lookup_calls = 0
        self.fetch_fail_next = False

    def store(self, key, blob: bytes) -> None:
        self.stored[tuple(key)] = blob

    def lookup(self, key):
        self.lookup_calls += 1
        toks = tuple(key)
        best = None
        for stored in self.stored:
            n = min(len(toks), len(stored))
            if toks[:n] == stored[:n] and n > 0:
                if best is None or n > best[0]:
                    best = (n, stored)
        if best is None:
            return None
        matched, _ = best
        self._next_handle += 1
        return _Hit(handle=self._next_handle, matched_tokens=matched)

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


# ----- driver lifecycle tests ------------------------------------------------

BPT = 4


def test_start_onboard_miss_schedules_nothing():
    fc = FakeConnector()
    d = AsyncLoadDriver(fc, bytes_per_token=BPT)
    assert d.start_onboard("req-1", [1, 2, 3]) == 0
    assert d.in_flight() == 0
    assert fc.fetch_calls == 0


def test_start_onboard_hit_then_pop_returns_bytes():
    fc = FakeConnector()
    fc.store([1, 2, 3, 4], b"\x00" * (4 * BPT))
    d = AsyncLoadDriver(fc, bytes_per_token=BPT)
    matched = d.start_onboard("req-1", [1, 2, 3, 4, 99])
    assert matched == 4
    assert d.in_flight() == 1
    assert d.matched_tokens("req-1") == 4
    out = d.pop("req-1")
    assert out is not None
    assert len(out) == 4 * BPT
    assert len(set(out)) == 1
    assert d.in_flight() == 0
    assert fc.released


def test_fetch_is_actually_async_start_returns_before_fetch_done():
    fc = FakeConnector()
    fc.store([7], b"x" * BPT)
    fc.fetch_gate.clear()
    d = AsyncLoadDriver(fc, bytes_per_token=BPT)
    t0 = time.monotonic()
    matched = d.start_onboard("r", [7])
    elapsed = time.monotonic() - t0
    assert matched == 1
    assert elapsed < 0.5, f"start_onboard blocked {elapsed:.3f}s"
    assert not d.finished_ids({"r"})
    fc.fetch_gate.set()
    out = d.pop("r")
    assert out is not None
    assert len(out) == BPT


def test_finished_ids_polls_correctly_and_is_idempotent():
    fc = FakeConnector()
    fc.store([1], b"a" * BPT)
    fc.store([2], b"b" * BPT)
    d = AsyncLoadDriver(fc, bytes_per_token=BPT)
    d.start_onboard("a", [1])
    d.start_onboard("b", [2])
    for _ in range(100):
        if d.finished_ids({"a", "b"}) == {"a", "b"}:
            break
        time.sleep(0.005)
    assert d.finished_ids({"a", "b"}) == {"a", "b"}
    assert d.finished_ids() == {"a", "b"}
    d.pop("a")
    assert d.finished_ids({"a", "b"}) == {"b"}


def test_cancel_releases_handle_and_drops_in_flight():
    fc = FakeConnector()
    fc.store([1], b"a" * BPT)
    d = AsyncLoadDriver(fc, bytes_per_token=BPT)
    d.start_onboard("r", [1])
    d.cancel("r")
    assert d.in_flight() == 0
    assert fc.released
    d.cancel("never-existed")  # no-op


def test_reonboard_replacing_in_flight_rid_releases_prior_handle():
    fc = FakeConnector()
    fc.store([1], b"a" * BPT)
    fc.store([2], b"b" * BPT)
    d = AsyncLoadDriver(fc, bytes_per_token=BPT)
    d.start_onboard("r", [1])
    first_handle = fc._next_handle
    d.start_onboard("r", [2])
    assert first_handle in fc.released


def test_worker_exception_surfaces_through_finished_ids():
    fc = FakeConnector()
    fc.store([1], b"a" * BPT)
    fc.fetch_fail_next = True
    d = AsyncLoadDriver(fc, bytes_per_token=BPT)
    d.start_onboard("r", [1])
    deadline = time.monotonic() + 1.0
    raised = None
    while time.monotonic() < deadline:
        try:
            d.finished_ids({"r"})
        except RuntimeError as e:
            raised = e
            break
        if d._state["r"].future.done():
            try:
                d.finished_ids({"r"})
            except RuntimeError as e:
                raised = e
            break
        time.sleep(0.005)
    assert raised is not None and "boom" in str(raised)


def test_close_blocks_and_releases_remaining_handles():
    fc = FakeConnector()
    fc.store([1], b"a" * BPT)
    d = AsyncLoadDriver(fc, bytes_per_token=BPT)
    d.start_onboard("r", [1])
    d.close(wait=True)
    assert fc.released


def test_constructor_validates_args():
    fc = FakeConnector()
    with pytest.raises(ValueError):
        AsyncLoadDriver(fc, bytes_per_token=0)
    with pytest.raises(ValueError):
        AsyncLoadDriver(fc, bytes_per_token=4, workers=0)


# ----- e2e against real C ABI ------------------------------------------------

BLOCK = 16
BYTES_PER_TOKEN = 64


def _have_library() -> bool:
    if os.environ.get("KVCACHE_LIB"):
        return os.path.isfile(os.environ["KVCACHE_LIB"])
    return shutil.which("cmake") is not None


cffi_avail = pytest.importorskip(
    "cffi", reason="cffi is required for the C ABI tests")


@pytest.mark.skipif(not _have_library(), reason="libkvcache.so not available")
def test_async_kvbm_start_onboard_take_round_trip_against_real_abi():
    from kvcache_dynamo import AsyncDynamoKVBMConnector
    key = list(range(2 * BLOCK))
    payload = bytes(((i * 23) & 0xFF
                      for i in range(len(key) * BYTES_PER_TOKEN)))
    with AsyncDynamoKVBMConnector(tenant_id="dynamo-async-tenant",
                                  model_id="dynamo-async-demo",
                                  bytes_per_token=BYTES_PER_TOKEN,
                                  workers=2) as kvbm:
        kvbm.offload(key, payload)
        matched = kvbm.start_onboard("req-async-1", key)
        assert matched == 2 * BLOCK
        out = kvbm.take("req-async-1")
        assert out == payload
        assert kvbm.in_flight() == 0
