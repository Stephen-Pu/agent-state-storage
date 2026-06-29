"""End-to-end coverage of DynamoKVBMConnector against the loopback C ABI.

Drives the KVBM-shaped match / offload / onboard verbs through the real
Core ABI (Reserve → write → Publish → Seal → Lookup → Fetch → byte verify).

Requires libkvcache.so / .dylib reachable through ``$KVCACHE_LIB`` or the
default build/ search path. Skipped (not failed) if cffi isn't installed.
"""

from __future__ import annotations

import os
import shutil

import pytest

cffi = pytest.importorskip("cffi", reason="cffi is required for the C ABI tests")

from kvcache_dynamo import DynamoKVBMConnector

BLOCK = 16
BYTES_PER_TOKEN = 64


def _have_library() -> bool:
    if os.environ.get("KVCACHE_LIB"):
        return os.path.isfile(os.environ["KVCACHE_LIB"])
    return shutil.which("cmake") is not None


@pytest.mark.skipif(not _have_library(), reason="libkvcache.so not available")
def test_offload_then_onboard_round_trip():
    tokens = list(range(2 * BLOCK))  # 32 tokens = 2 blocks
    payload = bytes(((i * 11) & 0xFF
                      for i in range(len(tokens) * BYTES_PER_TOKEN)))
    with DynamoKVBMConnector(tenant_id="dynamo-tenant",
                             model_id="dynamo-demo",
                             bytes_per_token=BYTES_PER_TOKEN) as kvbm:
        kvbm.offload(tokens, payload)
        got = kvbm.onboard(tokens)
        assert got is not None
        assert got == payload


@pytest.mark.skipif(not _have_library(), reason="libkvcache.so not available")
def test_match_returns_block_aligned_count():
    tokens = list(range(2 * BLOCK))
    payload = bytes(len(tokens) * BYTES_PER_TOKEN)  # zeros
    with DynamoKVBMConnector(tenant_id="dynamo-tenant",
                             model_id="dynamo-match",
                             bytes_per_token=BYTES_PER_TOKEN) as kvbm:
        kvbm.offload(tokens, payload)
        # Unrelated 3-token tail — LPM drops the partial block.
        n = kvbm.match(tokens + [9001, 9002, 9003])
        assert n == 2 * BLOCK


@pytest.mark.skipif(not _have_library(), reason="libkvcache.so not available")
def test_onboard_miss_returns_none():
    with DynamoKVBMConnector(tenant_id="dynamo-tenant",
                             model_id="dynamo-miss",
                             bytes_per_token=BYTES_PER_TOKEN) as kvbm:
        assert kvbm.onboard(list(range(BLOCK))) is None
        assert kvbm.match(list(range(BLOCK))) == 0


@pytest.mark.skipif(not _have_library(), reason="libkvcache.so not available")
def test_onboard_truncates_to_matched_prefix():
    base = list(range(2 * BLOCK))
    payload = bytes(((i * 17) & 0xFF
                      for i in range(len(base) * BYTES_PER_TOKEN)))
    with DynamoKVBMConnector(tenant_id="dynamo-tenant",
                             model_id="dynamo-prefix",
                             bytes_per_token=BYTES_PER_TOKEN) as kvbm:
        kvbm.offload(base, payload)
        extended = base + list(range(2 * BLOCK, 3 * BLOCK))
        got = kvbm.onboard(extended)
        assert got is not None
        assert len(got) == len(payload)
        assert got == payload


def test_constructor_rejects_zero_bytes_per_token():
    with pytest.raises(ValueError):
        DynamoKVBMConnector(tenant_id="t", model_id="m", bytes_per_token=0)


def test_evict_returns_false():
    if not _have_library():
        pytest.skip("libkvcache.so not available")
    with DynamoKVBMConnector(tenant_id="dynamo-tenant", model_id="dynamo-evict",
                             bytes_per_token=BYTES_PER_TOKEN) as kvbm:
        assert kvbm.evict(list(range(BLOCK))) is False
