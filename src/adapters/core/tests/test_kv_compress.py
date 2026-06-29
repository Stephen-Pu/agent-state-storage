"""Phase KVZ-2 — KV-tensor codec via the Python C-ABI binding.

Drives connector.compress_kv / decompress_kv against the real libkvcache,
mirroring the C++ ABI test: lossy round-trip within an error bound, int4 is
smaller + lossier than int8, and arg validation.
"""

from __future__ import annotations

import os
import shutil

import pytest

pytest.importorskip("cffi", reason="cffi is required for the C ABI tests")

from kvcache_core import KVCacheConnector

T, E = 32, 64


def _have_library() -> bool:
    if os.environ.get("KVCACHE_LIB"):
        return os.path.isfile(os.environ["KVCACHE_LIB"])
    return shutil.which("cmake") is not None


def _smooth_kv():
    return [1.0 + 0.01 * t + 0.1 * (e % 5) for t in range(T) for e in range(E)]


@pytest.mark.skipif(not _have_library(), reason="libkvcache.so not available")
def test_compress_decompress_round_trip():
    vals = _smooth_kv()
    with KVCacheConnector(tenant_id="kvz-tenant", model_id="kvz-demo") as cx:
        blob = cx.compress_kv(vals, T, E, bits=8, delta=True)
        assert 0 < len(blob) < len(vals) * 4  # smaller than raw fp32
        dec, shape = cx.decompress_kv(blob)
        assert shape == (T, E)
        assert len(dec) == T * E
        max_err = max(abs(a - b) for a, b in zip(vals, dec))
        assert max_err < 0.01  # int8 over smooth KV: <1% of the value range


@pytest.mark.skipif(not _have_library(), reason="libkvcache.so not available")
def test_int4_smaller_and_lossier_than_int8():
    vals = _smooth_kv()
    with KVCacheConnector(tenant_id="kvz-tenant", model_id="kvz-rd") as cx:
        b8 = cx.compress_kv(vals, T, E, bits=8)
        b4 = cx.compress_kv(vals, T, E, bits=4)
        assert len(b4) < len(b8)
        d8, _ = cx.decompress_kv(b8)
        d4, _ = cx.decompress_kv(b4)
        e8 = max(abs(a - b) for a, b in zip(vals, d8))
        e4 = max(abs(a - b) for a, b in zip(vals, d4))
        assert e8 < e4  # rate-distortion: more bits → less error


@pytest.mark.skipif(not _have_library(), reason="libkvcache.so not available")
def test_length_mismatch_raises():
    with KVCacheConnector(tenant_id="kvz-tenant", model_id="kvz-bad") as cx:
        with pytest.raises(ValueError):
            cx.compress_kv([1.0, 2.0, 3.0], T, E)  # len != T*E
