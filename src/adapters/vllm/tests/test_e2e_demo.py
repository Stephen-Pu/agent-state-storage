"""End-to-end demo: two-request prefix reuse.

Flow:

  1. Build a KVCacheConnector.
  2. Request A: 32 tokens (2 full chunks) — Reserve → write → Publish → Seal.
  3. Request B: same 32 tokens + 3 extra → Lookup must report 32 matched
     tokens (i.e. 2 chunks); the tail < 16 tokens is discarded by LPM (LLD §3.2).
  4. Fetch the matched bytes into a caller buffer and confirm content equals
     what request A wrote.

Requires libkvcache.so to be built and discoverable via $KVCACHE_LIB or the
default build/ search path. The test is skipped (not failed) if cffi is not
installed in the environment.
"""

from __future__ import annotations

import os
import shutil

import pytest

cffi = pytest.importorskip("cffi", reason="cffi is required for the C ABI tests")

from kvcache_vllm import KVCacheConnector

CHUNK = 16
BYTES_PER_TOKEN = 64  # arbitrary KV payload size for the demo


def _have_library() -> bool:
    if os.environ.get("KVCACHE_LIB"):
        return os.path.isfile(os.environ["KVCACHE_LIB"])
    # Quick sanity: don't bother running if no build directory exists.
    return shutil.which("cmake") is not None  # heuristic; real check is in _ffi.load


@pytest.mark.skipif(not _have_library(), reason="libkvcache.so not available")
def test_prefix_reuse_across_two_requests():
    with KVCacheConnector(tenant_id="demo-tenant", model_id="demo-model") as c:
        # ---------- Request A ---------------------------------------------
        tokens_a = list(range(2 * CHUNK))  # 32 tokens = 2 chunks
        payload_a = bytes(((i * 7) & 0xFF for i in range(len(tokens_a) * BYTES_PER_TOKEN)))

        locator_a = c.make_locator(tokens_a)
        rsv = c.reserve(locator_a, len(payload_a))
        assert rsv.slot_bytes >= len(payload_a)

        c.write_into_slot(rsv.slot_addr, payload_a)
        c.publish(rsv.handle, watermark=len(payload_a))
        c.seal(rsv.handle, tokens_a)

        # ---------- Request B (shares prefix with A) ----------------------
        tokens_b = tokens_a + [9001, 9002, 9003]  # +3 token tail, dropped by LPM
        hit = c.lookup(tokens_b)
        assert hit is not None, "expected an LPM hit on the shared prefix"
        assert hit.matched_tokens == 2 * CHUNK, (
            "LPM should return exactly 2 chunks (32 tokens); "
            f"got {hit.matched_tokens}"
        )

        # ---------- Fetch the cached bytes --------------------------------
        out = bytearray(len(payload_a))
        cid = c.fetch(hit.handle, out)
        c.wait(cid)
        assert bytes(out) == payload_a, "fetched bytes should match the original write"

        c.release(hit.handle)


@pytest.mark.skipif(not _have_library(), reason="libkvcache.so not available")
def test_lookup_miss_returns_none():
    with KVCacheConnector(tenant_id="demo-tenant", model_id="demo-model-2") as c:
        tokens = list(range(CHUNK))  # never seen by this fresh model id
        assert c.lookup(tokens) is None
