"""Phase KVZ-3 — SGLang adapter lossy KV-tensor compression mode.

store(compress=True) compresses the KV via the CacheGen-class codec; retrieve
sizes the fetch with kv_lookup_stored_bytes, decodes, and slices to the
matched prefix. End-to-end through the real C ABI.
"""

from __future__ import annotations

import array
import os
import shutil

import pytest

cffi = pytest.importorskip("cffi", reason="cffi is required for the C ABI tests")

from kvcache_sglang import SGLangKVBackend

CHUNK = 16
BYTES_PER_TOKEN = 64          # 16 fp32 elements per token
ELEMS = BYTES_PER_TOKEN // 4


def _have_library() -> bool:
    if os.environ.get("KVCACHE_LIB"):
        return os.path.isfile(os.environ["KVCACHE_LIB"])
    return shutil.which("cmake") is not None


def _smooth_kv_bytes(n_tokens: int) -> bytes:
    # Smooth across tokens → compresses well, low reconstruction error.
    vals = [1.0 + 0.01 * t + 0.1 * (e % 5)
            for t in range(n_tokens) for e in range(ELEMS)]
    return array.array("f", vals).tobytes()


@pytest.mark.skipif(not _have_library(), reason="libkvcache.so not available")
def test_compressed_store_retrieve_round_trip():
    tokens = list(range(2 * CHUNK))
    payload = _smooth_kv_bytes(len(tokens))
    with SGLangKVBackend(tenant_id="sg-zc", model_id="sg-zc-demo",
                         bytes_per_token=BYTES_PER_TOKEN, compress=True) as kv:
        kv.store(tokens, payload)
        got = kv.retrieve(tokens)
        assert got is not None
        assert len(got) == len(payload)
        # Lossy (int8) — reconstructed floats within <1% of the value range.
        orig = array.array("f"); orig.frombytes(payload)
        rec = array.array("f"); rec.frombytes(got)
        max_err = max(abs(a - b) for a, b in zip(orig, rec))
        assert max_err < 0.01


@pytest.mark.skipif(not _have_library(), reason="libkvcache.so not available")
def test_compressed_stored_smaller_than_raw():
    tokens = list(range(4 * CHUNK))
    payload = _smooth_kv_bytes(len(tokens))
    with SGLangKVBackend(tenant_id="sg-zc", model_id="sg-zc-size",
                         bytes_per_token=BYTES_PER_TOKEN, compress=True) as kv:
        kv.store(tokens, payload)
        hit = kv._cx.lookup(tokens)
        assert hit is not None
        stored = kv._cx.stored_bytes(hit.handle)
        kv._cx.release(hit.handle)
        assert stored < len(payload)  # the value proof: fewer bytes at rest


@pytest.mark.skipif(not _have_library(), reason="libkvcache.so not available")
def test_uncompressed_mode_unchanged():
    tokens = list(range(2 * CHUNK))
    payload = bytes((i * 7) & 0xFF for i in range(len(tokens) * BYTES_PER_TOKEN))
    with SGLangKVBackend(tenant_id="sg-zc", model_id="sg-zc-plain",
                         bytes_per_token=BYTES_PER_TOKEN) as kv:  # compress off
        kv.store(tokens, payload)
        assert kv.retrieve(tokens) == payload  # exact, lossless


def test_compress_requires_fp32_aligned_bytes_per_token():
    with pytest.raises(ValueError):
        SGLangKVBackend(tenant_id="t", model_id="m",
                        bytes_per_token=66, compress=True)  # 66 % 4 != 0
