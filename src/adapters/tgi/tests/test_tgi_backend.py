"""End-to-end coverage of TGIPrefixCache against the loopback C ABI."""

from __future__ import annotations

import os
import shutil

import pytest

cffi = pytest.importorskip("cffi", reason="cffi is required for the C ABI tests")

from kvcache_tgi import TGIPrefixCache

BLOCK = 16
BYTES_PER_TOKEN = 64


def _have_library() -> bool:
    if os.environ.get("KVCACHE_LIB"):
        return os.path.isfile(os.environ["KVCACHE_LIB"])
    return shutil.which("cmake") is not None


@pytest.mark.skipif(not _have_library(), reason="libkvcache.so not available")
def test_insert_then_load_round_trip():
    ids = list(range(2 * BLOCK))
    payload = bytes(((i * 11) & 0xFF
                      for i in range(len(ids) * BYTES_PER_TOKEN)))
    with TGIPrefixCache(tenant_id="tgi-tenant", model_id="tgi-demo",
                        bytes_per_token=BYTES_PER_TOKEN) as pc:
        pc.insert(ids, payload)
        assert pc.load(ids) == payload


@pytest.mark.skipif(not _have_library(), reason="libkvcache.so not available")
def test_prefix_lookup_block_aligned():
    ids = list(range(2 * BLOCK))
    with TGIPrefixCache(tenant_id="tgi-tenant", model_id="tgi-lpm",
                        bytes_per_token=BYTES_PER_TOKEN) as pc:
        pc.insert(ids, bytes(len(ids) * BYTES_PER_TOKEN))
        assert pc.prefix_lookup(ids + [7, 8, 9]) == 2 * BLOCK


@pytest.mark.skipif(not _have_library(), reason="libkvcache.so not available")
def test_load_miss_returns_none():
    with TGIPrefixCache(tenant_id="tgi-tenant", model_id="tgi-miss",
                        bytes_per_token=BYTES_PER_TOKEN) as pc:
        assert pc.load(list(range(BLOCK))) is None
        assert pc.prefix_lookup(list(range(BLOCK))) == 0


def test_constructor_rejects_zero_bytes_per_token():
    with pytest.raises(ValueError):
        TGIPrefixCache(tenant_id="t", model_id="m", bytes_per_token=0)


def test_evict_returns_false():
    if not _have_library():
        pytest.skip("libkvcache.so not available")
    with TGIPrefixCache(tenant_id="tgi-tenant", model_id="tgi-evict",
                        bytes_per_token=BYTES_PER_TOKEN) as pc:
        assert pc.evict(list(range(BLOCK))) is False
