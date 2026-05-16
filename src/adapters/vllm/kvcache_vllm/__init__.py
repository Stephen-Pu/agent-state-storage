"""vLLM adapter package. See README.md and LLD §6.1.4."""

from .connector import KVCacheConnector, KVCacheError, LookupResult, ReserveResult

__all__ = ["KVCacheConnector", "KVCacheError", "LookupResult", "ReserveResult"]
