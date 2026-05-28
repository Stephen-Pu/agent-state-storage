"""Phase P-3 — per-layer save accumulator.

vLLM v1 invokes ``save_kv_layer(layer_name, kv_layer, attn_metadata)``
once per attention layer during the worker forward pass. The P-1 inner
connector saves at chunk granularity (16 tokens × all layers + heads
batched into one Reserve→Publish→Seal commit), so the bridge has to
buffer each layer's bytes until all layers have arrived, then
concatenate them in a deterministic order before handing them off.

This module is the buffer. It is intentionally **vllm-import-free** so
unit tests can exercise the fan-in logic on the default dev rig
without pulling vLLM in as a dependency.

LLD reference: §6.1.4 (per-layer save).
"""
from __future__ import annotations

from typing import Dict, List, Optional, Sequence, Tuple


class LayerAccumulator:
    """Per-request, per-layer save buffer with deterministic commit order.

    Each request is independent. A request's layers are concatenated in
    the order set by :meth:`register_layer_order`; layers seen but not
    in that order are appended sorted by name (defensive fallback so a
    mis-registered worker still produces a stable wire image).

    Threading model: callers serialize accumulate / drain per request.
    The bridge invokes this from the worker's single-threaded forward
    loop, so no internal locking is needed.
    """

    def __init__(self) -> None:
        self._layer_order: List[str] = []
        # request_id → {layer_name → bytes}
        self._buf: Dict[str, Dict[str, bytes]] = {}
        # request_id → token_ids (set by :meth:`bind_request`)
        self._tokens: Dict[str, List[int]] = {}

    # ---- registration --------------------------------------------------

    def register_layer_order(self, layer_names: Sequence[str]) -> None:
        """Record the canonical layer order. Concatenation at drain time
        follows this order; unknown layers are appended sorted by name.
        Idempotent — a re-register replaces the previous order."""
        self._layer_order = list(layer_names)

    def bind_request(self, request_id: str,
                      token_ids: Sequence[int]) -> None:
        """Record the token sequence the upcoming commit will Save under.
        The token list is required — ``drain_request`` returns ``None``
        if a request has buffered layers but no bound tokens, so an
        un-bound request can't corrupt the cache with a bogus key.
        """
        self._tokens[request_id] = list(token_ids)

    # ---- per-layer fan-in ----------------------------------------------

    def accumulate(self, request_id: str, layer_name: str,
                    layer_bytes: bytes) -> None:
        """Append one layer's bytes for ``request_id``. Empty payloads
        are dropped (no point sealing zero bytes). A repeat for the
        same (rid, layer) overwrites — vLLM occasionally re-fires a
        layer when chunked prefill straddles a step boundary, and the
        later view is the authoritative one.
        """
        if not layer_bytes:
            return
        self._buf.setdefault(request_id, {})[layer_name] = bytes(layer_bytes)

    # ---- inspection ----------------------------------------------------

    def has_pending(self, request_id: Optional[str] = None) -> bool:
        if request_id is None:
            return bool(self._buf)
        return request_id in self._buf

    def pending_layer_count(self, request_id: str) -> int:
        return len(self._buf.get(request_id, {}))

    # ---- commit --------------------------------------------------------

    def drain_request(
        self, request_id: str
    ) -> Optional[Tuple[List[int], bytes]]:
        """Pop the request's layers + token list and return
        ``(tokens, concatenated_bytes)``. Returns ``None`` if the
        request has no buffered layers OR no bound tokens — the
        caller (bridge) should skip the Save in that case.
        """
        layers = self._buf.pop(request_id, None)
        tokens = self._tokens.pop(request_id, None)
        if not layers or not tokens:
            return None
        order = self._effective_order(layers.keys())
        concat = b"".join(layers[ln] for ln in order)
        return tokens, concat

    def drain_all(self) -> Dict[str, Tuple[List[int], bytes]]:
        """Drain every request that has both buffered layers AND bound
        tokens. Requests missing either side stay in the buffer (they
        get a second chance on the next ``wait_for_save`` fire) —
        callers that want to GC orphans should call :meth:`clear`.
        """
        out: Dict[str, Tuple[List[int], bytes]] = {}
        for rid in list(self._buf.keys()):
            r = self.drain_request(rid)
            if r is not None:
                out[rid] = r
        return out

    def clear(self) -> None:
        """Drop all buffered state. Used on connector close or when an
        engine reset invalidates in-flight saves."""
        self._buf.clear()
        self._tokens.clear()

    # ---- helpers -------------------------------------------------------

    def _effective_order(self, present_layers) -> List[str]:
        present = set(present_layers)
        order = [ln for ln in self._layer_order if ln in present]
        leftover = sorted(present - set(self._layer_order))
        return order + leftover
