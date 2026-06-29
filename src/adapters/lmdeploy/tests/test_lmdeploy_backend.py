"""End-to-end coverage of LMDeployBlockCache against the loopback C ABI."""

from __future__ import annotations

import os
import shutil

import pytest

cffi = pytest.importorskip("cffi", reason="cffi is required for the C ABI tests")

from kvcache_lmdeploy import LMDeployBlockCache

BLOCK = 16
BYTES_PER_TOKEN = 64


def _have_library() -> bool:
    if os.environ.get("KVCACHE_LIB"):
        return os.path.isfile(os.environ["KVCACHE_LIB"])
    return shutil.which("cmake") is not None


@pytest.mark.skipif(not _have_library(), reason="libkvcache.so not available")
def test_add_then_get_round_trip():
    tokens = list(range(2 * BLOCK))
    payload = bytes(((i * 11) & 0xFF
                      for i in range(len(tokens) * BYTES_PER_TOKEN)))
    with LMDeployBlockCache(tenant_id="lmdeploy-tenant",
                            model_id="lmdeploy-demo",
                            bytes_per_token=BYTES_PER_TOKEN) as bc:
        bc.add(tokens, payload)
        got = bc.get(tokens)
        assert got == payload


@pytest.mark.skipif(not _have_library(), reason="libkvcache.so not available")
def test_match_is_block_aligned():
    tokens = list(range(2 * BLOCK))
    with LMDeployBlockCache(tenant_id="lmdeploy-tenant",
                            model_id="lmdeploy-match",
                            bytes_per_token=BYTES_PER_TOKEN) as bc:
        bc.add(tokens, bytes(len(tokens) * BYTES_PER_TOKEN))
        assert bc.match(tokens + [7, 8, 9]) == 2 * BLOCK


@pytest.mark.skipif(not _have_library(), reason="libkvcache.so not available")
def test_get_miss_returns_none():
    with LMDeployBlockCache(tenant_id="lmdeploy-tenant",
                            model_id="lmdeploy-miss",
                            bytes_per_token=BYTES_PER_TOKEN) as bc:
        assert bc.get(list(range(BLOCK))) is None
        assert bc.match(list(range(BLOCK))) == 0


def test_constructor_rejects_zero_bytes_per_token():
    with pytest.raises(ValueError):
        LMDeployBlockCache(tenant_id="t", model_id="m", bytes_per_token=0)


def test_free_returns_false():
    if not _have_library():
        pytest.skip("libkvcache.so not available")
    with LMDeployBlockCache(tenant_id="lmdeploy-tenant", model_id="lmdeploy-free",
                            bytes_per_token=BYTES_PER_TOKEN) as bc:
        assert bc.free(list(range(BLOCK))) is False
