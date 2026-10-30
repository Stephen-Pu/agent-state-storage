"""LangGraph long-term store (BaseStore) backed by AgentStateStorage records.

Key: pack("lg","st") + pack(*namespace) + pack("") + pack(key). Namespace
labels are never empty (LangGraph rejects them), so the empty part marks
where the namespace ends, and a namespace prefix only matches whole labels.
Value: JSON {"value", "created_at", "updated_at"}.

No vector index yet: like InMemoryStore without an index, a search `query`
does not rank results and items carry no score. Filtering, pagination and
namespace listing run on the client over a prefix scan.
"""

from __future__ import annotations

import asyncio
import json
from collections.abc import Iterable
from datetime import datetime, timezone
from typing import Any

from langgraph.store.base import (
    BaseStore,
    GetOp,
    Item,
    ListNamespacesOp,
    MatchCondition,
    Op,
    PutOp,
    Result,
    SearchItem,
    SearchOp,
)

from .._common import Record
from ..client import Client
from ._keys import pack, unpack

_ROOT = pack("lg", "st")
_NS_END = pack("")


def _key(namespace: tuple[str, ...], key: str) -> bytes:
    return _ROOT + pack(*namespace) + _NS_END + pack(key)


def _split(raw_key: bytes) -> tuple[tuple[str, ...], str]:
    parts = unpack(raw_key[len(_ROOT):])
    end = parts.index(b"")
    return tuple(p.decode() for p in parts[:end]), parts[end + 1].decode()


def _item(namespace: tuple[str, ...], key: str, raw: bytes, cls=Item):
    doc = json.loads(raw)
    return cls(
        value=doc["value"],
        key=key,
        namespace=namespace,
        created_at=datetime.fromisoformat(doc["created_at"]),
        updated_at=datetime.fromisoformat(doc["updated_at"]),
    )


# Filter semantics follow langgraph.store.memory (JSONB-like comparison).
def _compare(item_value: Any, filter_value: Any) -> bool:
    if isinstance(filter_value, dict):
        if any(k.startswith("$") for k in filter_value):
            return all(_operator(item_value, k, v) for k, v in filter_value.items())
        if not isinstance(item_value, dict):
            return False
        return all(_compare(item_value.get(k), v) for k, v in filter_value.items())
    if isinstance(filter_value, (list, tuple)):
        return (isinstance(item_value, (list, tuple)) and len(item_value) == len(filter_value)
                and all(_compare(a, b) for a, b in zip(item_value, filter_value)))
    return item_value == filter_value


def _operator(value: Any, op: str, operand: Any) -> bool:
    if op == "$eq":
        return value == operand
    if op == "$ne":
        return value != operand
    if value is None:
        return False
    if op == "$gt":
        return float(value) > float(operand)
    if op == "$gte":
        return float(value) >= float(operand)
    if op == "$lt":
        return float(value) < float(operand)
    if op == "$lte":
        return float(value) <= float(operand)
    raise ValueError(f"Unsupported operator: {op}")


def _matches(cond: MatchCondition, namespace: tuple[str, ...]) -> bool:
    path = tuple(cond.path)
    if len(namespace) < len(path):
        return False
    pairs = (zip(namespace, path) if cond.match_type == "prefix"
             else zip(reversed(namespace), reversed(path)))
    if cond.match_type not in ("prefix", "suffix"):
        raise ValueError(f"Unsupported match type: {cond.match_type}")
    return all(p == "*" or n == p for n, p in pairs)


class AgentStateStore(BaseStore):
    """A LangGraph `BaseStore` that keeps long-term memory in an AgentStateStorage node.

        store = AgentStateStore(Client("localhost:7000", "acme"))
        graph = builder.compile(checkpointer=saver, store=store)

    Async methods run the sync calls in a worker thread.
    """

    def __init__(self, client: Client):
        self.client = client

    def batch(self, ops: Iterable[Op]) -> list[Result]:
        ops = list(ops)
        results: list[Result] = [None] * len(ops)
        puts: dict[tuple[tuple[str, ...], str], PutOp] = {}
        for i, op in enumerate(ops):
            if isinstance(op, GetOp):
                raw = self.client.get_records([_key(op.namespace, op.key)])
                results[i] = (_item(op.namespace, op.key, next(iter(raw.values())))
                              if raw else None)
            elif isinstance(op, SearchOp):
                results[i] = self._search(op)
            elif isinstance(op, ListNamespacesOp):
                results[i] = self._list_namespaces(op)
            elif isinstance(op, PutOp):
                puts[(op.namespace, op.key)] = op  # the last put to a key wins
            else:
                raise ValueError(f"Unknown operation type: {type(op)}")
        if puts:
            self._apply_puts(puts)
        return results

    async def abatch(self, ops: Iterable[Op]) -> list[Result]:
        return await asyncio.to_thread(self.batch, list(ops))

    def _apply_puts(self, puts: dict[tuple[tuple[str, ...], str], PutOp]) -> None:
        keys = {(ns, k): _key(ns, k) for ns, k in puts}
        existing = self.client.get_records([keys[nk] for nk, op in puts.items()
                                            if op.value is not None])
        now = datetime.now(timezone.utc).isoformat()
        writes, deletes = [], []
        for nk, op in puts.items():
            if op.value is None:
                deletes.append(keys[nk])
                continue
            created = json.loads(existing[keys[nk]])["created_at"] if keys[nk] in existing else now
            doc = {"value": dict(op.value), "created_at": created, "updated_at": now}
            writes.append(Record(keys[nk], json.dumps(doc).encode()))
        self.client.write_records(writes, deletes)

    def _search(self, op: SearchOp) -> list[SearchItem]:
        prefix = _ROOT + pack(*op.namespace_prefix)
        hits: list[SearchItem] = []
        skipped = 0
        for rec in self.client.scan_records(prefix):
            namespace, key = _split(rec.key)
            item = _item(namespace, key, rec.value, SearchItem)
            if op.filter and not all(_compare(item.value.get(k), v) for k, v in op.filter.items()):
                continue
            if skipped < op.offset:
                skipped += 1
                continue
            hits.append(item)
            if len(hits) >= op.limit:
                break
        return hits

    def _list_namespaces(self, op: ListNamespacesOp) -> list[tuple[str, ...]]:
        namespaces = set()
        for rec in self.client.scan_records(_ROOT):
            namespace, _ = _split(rec.key)
            if all(_matches(c, namespace) for c in op.match_conditions or ()):
                namespaces.add(namespace if op.max_depth is None else namespace[:op.max_depth])
        return sorted(namespaces)[op.offset:op.offset + op.limit]
