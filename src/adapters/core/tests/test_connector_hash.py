"""Phase W-2 — Python FNV-1a-64 golden vectors.

These exact values are ALSO asserted by the C++ side
(src/tests/unit/common/hashing_test.cpp). The two test files are the
cross-language lock on model_id_hash: if either implementation drifts, its
golden test fails. Do not change a value here without changing it there.
"""

from kvcache_core.connector import fnv1a64


def test_golden_vectors():
    assert fnv1a64(b"") == 0xCBF29CE484222325            # offset basis
    assert fnv1a64(b"a") == 0xAF63DC4C8601EC8C            # canonical FNV ref
    assert fnv1a64(b"llama-3-8b") == 0x2154ED5DC3AA6E7B
    assert fnv1a64(b"mistral-7b-instruct") == 0x4CDEEEA8EC431068


def test_sixteen_zero_bytes():
    assert fnv1a64(bytes(16)) == 0x88201FB960FF6465
