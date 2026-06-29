"""lmdeploy adapter package. See LLD §6.1.4.

``LMDeployBlockCache`` is the TurboMind block-cache surface
(match / add / get / free) over the engine-agnostic ``KVCacheConnector``.
"""

from kvcache_core import (
    KVCacheConnector,
    KVCacheError,
    LookupResult,
    ReserveResult,
)

from .async_load import AsyncLoadDriver
from .backend import AsyncLMDeployBlockCache, LMDeployBlockCache

__all__ = [
    "AsyncLMDeployBlockCache",
    "AsyncLoadDriver",
    "KVCacheConnector",
    "KVCacheError",
    "LMDeployBlockCache",
    "LookupResult",
    "ReserveResult",
]
