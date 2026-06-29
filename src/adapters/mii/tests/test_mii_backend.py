"""End-to-end coverage of MIIKVCache against the loopback C ABI."""

from __future__ import annotations

import os
import shutil

import pytest

cffi = pytest.importorskip("cffi", reason="cffi is required for the C ABI tests")

from kvcache_mii import MIIKVCache

BLOCK = 16
BYTES_PER_TOKEN = 64


def _have_library() -> bool:
    if os.environ.get("KVCACHE_LIB"):
        return os.path.isfile(os.environ["KVCACHE_LIB"])
    return shutil.which("cmake") is not None


@pytest.mark.skipif(not _have_library(), reason="libkvcache.so not available")
def test_put_then_get_round_trip():
    tokens = list(range(2 * BLOCK))
    payload = bytes(((i * 11) & 0xFF
                      for i in range(len(tokens) * BYTES_PER_TOKEN)))
    with MIIKVCache(tenant_id="mii-tenant", model_id="mii-demo",
                    bytes_per_token=BYTES_PER_TOKEN) as kv:
        kv.put(tokens, payload)
        assert kv.get(tokens) == payload


@pytest.mark.skipif(not _have_library(), reason="libkvcache.so not available")
def test_query_block_aligned():
    tokens = list(range(2 * BLOCK))
    with MIIKVCache(tenant_id="mii-tenant", model_id="mii-lpm",
                    bytes_per_token=BYTES_PER_TOKEN) as kv:
        kv.put(tokens, bytes(len(tokens) * BYTES_PER_TOKEN))
        assert kv.query(tokens + [7, 8, 9]) == 2 * BLOCK


@pytest.mark.skipif(not _have_library(), reason="libkvcache.so not available")
def test_get_miss_returns_none():
    with MIIKVCache(tenant_id="mii-tenant", model_id="mii-miss",
                    bytes_per_token=BYTES_PER_TOKEN) as kv:
        assert kv.get(list(range(BLOCK))) is None
        assert kv.query(list(range(BLOCK))) == 0


def test_constructor_rejects_zero_bytes_per_token():
    with pytest.raises(ValueError):
        MIIKVCache(tenant_id="t", model_id="m", bytes_per_token=0)


def test_release_returns_false():
    if not _have_library():
        pytest.skip("libkvcache.so not available")
    with MIIKVCache(tenant_id="mii-tenant", model_id="mii-release",
                    bytes_per_token=BYTES_PER_TOKEN) as kv:
        assert kv.release(list(range(BLOCK))) is False
