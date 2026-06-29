"""dynamo adapter package. See LLD §6.1.4.

``KVCacheConnector`` and friends are re-exported from :mod:`kvcache_core`
for parity with the other adapters; ``DynamoKVBMConnector`` is the
KVBM-shaped surface (match / offload / onboard / evict) on top.
"""

from kvcache_core import (
    KVCacheConnector,
    KVCacheError,
    LookupResult,
    ReserveResult,
)

from .async_load import AsyncLoadDriver
from .backend import AsyncDynamoKVBMConnector, DynamoKVBMConnector

__all__ = [
    "AsyncDynamoKVBMConnector",
    "AsyncLoadDriver",
    "DynamoKVBMConnector",
    "KVCacheConnector",
    "KVCacheError",
    "LookupResult",
    "ReserveResult",
]
