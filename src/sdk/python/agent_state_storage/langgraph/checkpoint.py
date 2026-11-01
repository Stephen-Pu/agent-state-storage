"""LangGraph checkpointer backed by AgentStateStorage records.

Same storage model as LangGraph's InMemorySaver, persisted:
  checkpoint  pack("lg","cp",thread,ns,checkpoint_id) -> pack(type, checkpoint, type, metadata, parent_id)
  channel     pack("lg","b",thread,ns,channel,version) -> pack(type, value)
  write       pack("lg","w",thread,ns,checkpoint_id,task_id,idx) -> pack(task_id, channel, type, value, task_path)
Checkpoint ids sort by creation time, so the newest checkpoint is the last
key under its namespace. A checkpoint and its new channel values are written
in one atomic batch.
"""

from __future__ import annotations

import asyncio
import random
from collections.abc import AsyncIterator, Iterator, Sequence
from typing import Any

from langchain_core.runnables import RunnableConfig
from langgraph.checkpoint.base import (
    WRITES_IDX_MAP,
    BaseCheckpointSaver,
    ChannelVersions,
    Checkpoint,
    CheckpointMetadata,
    CheckpointTuple,
    get_checkpoint_id,
    get_checkpoint_metadata,
    writes_sort_key,
)

from .._common import Record
from ..client import Client
from ._keys import pack, unpack

_CP, _BLOB, _WRITE = "cp", "b", "w"
# Keep each WriteRecords request well under gRPC's 4 MiB message limit.
_BATCH_BYTES = 2 << 20


def _key(kind: str, *parts: str) -> bytes:
    return pack("lg", kind, *parts)


class AgentStateSaver(BaseCheckpointSaver[str]):
    """A `BaseCheckpointSaver` that keeps checkpoints in an AgentStateStorage node.

        from agent_state_storage import Client
        from agent_state_storage.langgraph import AgentStateSaver

        graph = builder.compile(checkpointer=AgentStateSaver(Client("localhost:7000", "acme")))

    Async methods run the sync calls in a worker thread.
    """

    def __init__(self, client: Client, *, serde=None):
        super().__init__(serde=serde)
        self.client = client

    # ---- helpers ---------------------------------------------------------

    def _write_batched(self, puts: list[Record]) -> None:
        batch, size = [], 0
        for r in puts:
            if batch and size + len(r.key) + len(r.value) > _BATCH_BYTES:
                self.client.write_records(batch)
                batch, size = [], 0
            batch.append(r)
            size += len(r.key) + len(r.value)
        if batch:
            self.client.write_records(batch)

    def _load_blobs(self, thread_id: str, ns: str, versions: ChannelVersions) -> dict[str, Any]:
        keys = {_key(_BLOB, thread_id, ns, ch, str(ver)): ch for ch, ver in versions.items()}
        found = self.client.get_records(list(keys))
        values: dict[str, Any] = {}
        for key, raw in found.items():
            type_, data = unpack(raw)
            if type_ == b"empty":
                continue
            values[keys[key]] = self.serde.loads_typed((type_.decode(), data))
        return values

    def _pending_writes(self, thread_id: str, ns: str, checkpoint_id: str) -> list:
        rows = []
        for rec in self.client.scan_records(_key(_WRITE, thread_id, ns, checkpoint_id)):
            task_id, channel, type_, data, task_path = unpack(rec.value)
            idx = int(unpack(rec.key)[-1])
            rows.append((task_path.decode(), task_id.decode(), idx, channel.decode(), type_, data))
        rows.sort(key=lambda r: writes_sort_key(r[0], r[1], r[2]))
        return [(tid, ch, self.serde.loads_typed((t.decode(), d))) for _, tid, _, ch, t, d in rows]

    def _tuple(self, thread_id: str, ns: str, checkpoint_id: str, raw: bytes) -> CheckpointTuple:
        ctype, cdata, mtype, mdata, parent = unpack(raw)
        checkpoint: Checkpoint = self.serde.loads_typed((ctype.decode(), cdata))
        parent_id = parent.decode()
        return CheckpointTuple(
            config={"configurable": {"thread_id": thread_id, "checkpoint_ns": ns,
                                     "checkpoint_id": checkpoint_id}},
            checkpoint={
                **checkpoint,
                "channel_values": self._load_blobs(thread_id, ns, checkpoint["channel_versions"]),
            },
            metadata=self.serde.loads_typed((mtype.decode(), mdata)),
            parent_config=(
                {"configurable": {"thread_id": thread_id, "checkpoint_ns": ns,
                                  "checkpoint_id": parent_id}}
                if parent_id else None
            ),
            pending_writes=self._pending_writes(thread_id, ns, checkpoint_id),
        )

    # ---- BaseCheckpointSaver -------------------------------------------

    def get_tuple(self, config: RunnableConfig) -> CheckpointTuple | None:
        thread_id = config["configurable"]["thread_id"]
        ns = config["configurable"].get("checkpoint_ns", "")
        if checkpoint_id := get_checkpoint_id(config):
            raw = self.client.get_records([_key(_CP, thread_id, ns, checkpoint_id)])
            if not raw:
                return None
            tup = self._tuple(thread_id, ns, checkpoint_id, next(iter(raw.values())))
            return tup._replace(config=config)
        latest = next(iter(self.client.scan_records(pack("lg", _CP, thread_id, ns),
                                                    reverse=True, limit=1)), None)
        if latest is None:
            return None
        return self._tuple(thread_id, ns, unpack(latest.key)[-1].decode(), latest.value)

    def list(
        self,
        config: RunnableConfig | None,
        *,
        filter: dict[str, Any] | None = None,
        before: RunnableConfig | None = None,
        limit: int | None = None,
    ) -> Iterator[CheckpointTuple]:
        if config:
            thread_id = config["configurable"]["thread_id"]
            ns = config["configurable"].get("checkpoint_ns")
            prefix = pack("lg", _CP, thread_id) if ns is None else pack("lg", _CP, thread_id, ns)
        else:
            prefix = pack("lg", _CP)
        want_id = get_checkpoint_id(config) if config else None
        before_id = get_checkpoint_id(before) if before else None
        for rec in self.client.scan_records(prefix, reverse=True):
            _, _, thread_id, ns_b, cid_b = unpack(rec.key)
            cid = cid_b.decode()
            if want_id and cid != want_id:
                continue
            if before_id and cid >= before_id:
                continue
            if filter:
                _, _, mtype, mdata, _ = unpack(rec.value)
                metadata = self.serde.loads_typed((mtype.decode(), mdata))
                if not all(metadata.get(k) == v for k, v in filter.items()):
                    continue
            if limit is not None and limit <= 0:
                return
            if limit is not None:
                limit -= 1
            yield self._tuple(thread_id.decode(), ns_b.decode(), cid, rec.value)

    def put(
        self,
        config: RunnableConfig,
        checkpoint: Checkpoint,
        metadata: CheckpointMetadata,
        new_versions: ChannelVersions,
    ) -> RunnableConfig:
        thread_id = config["configurable"]["thread_id"]
        ns = config["configurable"].get("checkpoint_ns", "")
        c = checkpoint.copy()
        values: dict[str, Any] = c.pop("channel_values")  # type: ignore[misc]
        puts = []
        for ch, ver in new_versions.items():
            if ch in values:
                type_, data = self.serde.dumps_typed(values[ch])
            else:
                type_, data = "empty", b""
            puts.append(Record(_key(_BLOB, thread_id, ns, ch, str(ver)), pack(type_, data)))
        ctype, cdata = self.serde.dumps_typed(c)
        mtype, mdata = self.serde.dumps_typed(get_checkpoint_metadata(config, metadata))
        parent = config["configurable"].get("checkpoint_id") or ""
        puts.append(Record(_key(_CP, thread_id, ns, checkpoint["id"]),
                           pack(ctype, cdata, mtype, mdata, parent)))
        self.client.write_records(puts)
        return {"configurable": {"thread_id": thread_id, "checkpoint_ns": ns,
                                 "checkpoint_id": checkpoint["id"]}}

    def put_writes(
        self,
        config: RunnableConfig,
        writes: Sequence[tuple[str, Any]],
        task_id: str,
        task_path: str = "",
    ) -> None:
        thread_id = config["configurable"]["thread_id"]
        ns = config["configurable"].get("checkpoint_ns", "")
        checkpoint_id = config["configurable"]["checkpoint_id"]
        rows = []
        for idx, (channel, value) in enumerate(writes):
            widx = WRITES_IDX_MAP.get(channel, idx)
            type_, data = self.serde.dumps_typed(value)
            rows.append((widx, Record(_key(_WRITE, thread_id, ns, checkpoint_id, task_id, str(widx)),
                                      pack(task_id, channel, type_, data, task_path))))
        # Regular writes are kept from the first attempt; special ones
        # (errors, interrupts, ...) are overwritten, as in InMemorySaver.
        regular = [r.key for widx, r in rows if widx >= 0]
        existing = set(self.client.get_records(regular)) if regular else set()
        self._write_batched([r for widx, r in rows if widx < 0 or r.key not in existing])

    def delete_thread(self, thread_id: str) -> None:
        for kind in (_CP, _WRITE, _BLOB):
            self.client.delete_record_prefix(pack("lg", kind, thread_id))

    def copy_thread(self, source_thread_id: str, target_thread_id: str) -> None:
        puts = []
        for kind in (_CP, _WRITE, _BLOB):
            for rec in self.client.scan_records(pack("lg", kind, source_thread_id)):
                parts = unpack(rec.key)
                parts[2] = target_thread_id.encode()
                puts.append(Record(pack(*parts), rec.value))
        self._write_batched(puts)

    def get_next_version(self, current: str | None, channel: None) -> str:
        if current is None:
            current_v = 0
        elif isinstance(current, int):
            current_v = current
        else:
            current_v = int(current.split(".")[0])
        return f"{current_v + 1:032}.{random.random():016}"

    # ---- async twins -----------------------------------------------------

    async def aget_tuple(self, config: RunnableConfig) -> CheckpointTuple | None:
        return await asyncio.to_thread(self.get_tuple, config)

    async def alist(
        self,
        config: RunnableConfig | None,
        *,
        filter: dict[str, Any] | None = None,
        before: RunnableConfig | None = None,
        limit: int | None = None,
    ) -> AsyncIterator[CheckpointTuple]:
        items = await asyncio.to_thread(
            lambda: list(self.list(config, filter=filter, before=before, limit=limit))
        )
        for item in items:
            yield item

    async def aput(self, config: RunnableConfig, checkpoint: Checkpoint,
                   metadata: CheckpointMetadata, new_versions: ChannelVersions) -> RunnableConfig:
        return await asyncio.to_thread(self.put, config, checkpoint, metadata, new_versions)

    async def aput_writes(self, config: RunnableConfig, writes: Sequence[tuple[str, Any]],
                          task_id: str, task_path: str = "") -> None:
        await asyncio.to_thread(self.put_writes, config, writes, task_id, task_path)

    async def adelete_thread(self, thread_id: str) -> None:
        await asyncio.to_thread(self.delete_thread, thread_id)

    async def acopy_thread(self, source_thread_id: str, target_thread_id: str) -> None:
        await asyncio.to_thread(self.copy_thread, source_thread_id, target_thread_id)
