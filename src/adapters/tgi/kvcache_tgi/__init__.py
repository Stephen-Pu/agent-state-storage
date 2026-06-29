"""tgi adapter package. See LLD §6.1.4.

``TGIPrefixCache`` is the TGI prefix-cache surface
(prefix_lookup / insert / load / evict) over the engine-agnostic
``KVCacheConnector``.
"""

from kvcache_core import (
    KVCacheConnector,
    KVCacheError,
    LookupResult,
    ReserveResult,
)

from .async_load import AsyncLoadDriver
from .backend import AsyncTGIPrefixCache, TGIPrefixCache

__all__ = [
    "AsyncLoadDriver",
    "AsyncTGIPrefixCache",
    "KVCacheConnector",
    "KVCacheError",
    "LookupResult",
    "ReserveResult",
    "TGIPrefixCache",
]
