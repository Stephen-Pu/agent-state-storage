"""aibrix adapter package. See README.md and LLD §6.1.4.

``KVCacheConnector`` and friends are re-exported from :mod:`kvcache_core`
for parity with the other adapters; ``AIBrixKVConnector`` is the
AIBrix-shaped surface (get / put / delete / exists) on top.
"""

from kvcache_core import (
    KVCacheConnector,
    KVCacheError,
    LookupResult,
    ReserveResult,
)

from .async_load import AsyncLoadDriver
from .backend import AIBrixKVConnector, AsyncAIBrixKVConnector

__all__ = [
    "AIBrixKVConnector",
    "AsyncAIBrixKVConnector",
    "AsyncLoadDriver",
    "KVCacheConnector",
    "KVCacheError",
    "LookupResult",
    "ReserveResult",
]
