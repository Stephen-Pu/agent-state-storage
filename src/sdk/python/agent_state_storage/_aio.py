"""asyncio client for the agentstate.v1 service; same methods as Client, as coroutines."""

from __future__ import annotations

from typing import AsyncIterator, BinaryIO, Sequence

import grpc
import grpc.aio

from ._common import CHANNEL_OPTIONS, Record, TlsConfig, blob_chunks, scan_request
from ._errors import NotFound, translate
from ._proto import agentstate_pb2 as pb
from ._proto import agentstate_pb2_grpc as pb_grpc


class AsyncClient:
    def __init__(self, target: str, tenant: str = "", *, tls: TlsConfig | None = None,
                 timeout: float = 30.0):
        self.tenant = tenant
        self.timeout = timeout
        if tls is None:
            self._channel = grpc.aio.insecure_channel(target, options=CHANNEL_OPTIONS)
        else:
            self._channel = grpc.aio.secure_channel(target, tls.credentials(),
                                                    options=CHANNEL_OPTIONS + tls.options())
        self._stub = pb_grpc.AgentStateStub(self._channel)

    async def __aenter__(self) -> "AsyncClient":
        return self

    async def __aexit__(self, *exc) -> None:
        await self.close()

    async def close(self) -> None:
        await self._channel.close()

    async def _call(self, method, request):
        try:
            return await method(request, timeout=self.timeout)
        except grpc.RpcError as e:
            raise translate(e) from None

    # ---- Items ---------------------------------------------------------

    async def append_items(self, session: str, items: Sequence[bytes]) -> int:
        req = pb.AppendItemsRequest(tenant=self.tenant, session=session, items=list(items))
        return (await self._call(self._stub.AppendItems, req)).next_seq

    async def get_items(self, session: str, limit: int | None = None) -> list[bytes]:
        req = pb.GetItemsRequest(tenant=self.tenant, session=session, limit=limit or 0)
        return list((await self._call(self._stub.GetItems, req)).items)

    async def pop_item(self, session: str) -> bytes | None:
        resp = await self._call(self._stub.PopItem, pb.PopItemRequest(tenant=self.tenant, session=session))
        return resp.item if resp.found else None

    async def clear_session(self, session: str) -> int:
        req = pb.ClearSessionRequest(tenant=self.tenant, session=session)
        return (await self._call(self._stub.ClearSession, req)).removed

    async def fork_session(self, source: str, target: str) -> int:
        req = pb.ForkSessionRequest(tenant=self.tenant, source=source, target=target)
        return (await self._call(self._stub.ForkSession, req)).copied

    # ---- Records -------------------------------------------------------

    async def write_records(self, puts: Sequence[Record] = (), deletes: Sequence[bytes] = ()) -> None:
        req = pb.WriteRecordsRequest(
            tenant=self.tenant,
            puts=[pb.Record(key=k, value=v) for k, v in puts],
            deletes=list(deletes),
        )
        await self._call(self._stub.WriteRecords, req)

    async def get_records(self, keys: Sequence[bytes]) -> dict[bytes, bytes]:
        resp = await self._call(self._stub.GetRecords, pb.GetRecordsRequest(tenant=self.tenant, keys=list(keys)))
        return {r.key: r.value for r in resp.found}

    async def scan_records(self, prefix: bytes = b"", *, start_after: bytes = b"",
                           end_before: bytes = b"", limit: int | None = None,
                           reverse: bool = False) -> AsyncIterator[Record]:
        remaining = limit
        while remaining is None or remaining > 0:
            req = scan_request(self.tenant, prefix, start_after, end_before, remaining, reverse)
            resp = await self._call(self._stub.ScanRecords, req)
            for r in resp.records:
                yield Record(r.key, r.value)
            if remaining is not None:
                remaining -= len(resp.records)
            if not resp.has_more or not resp.records:
                return
            if reverse:
                end_before = resp.records[-1].key
            else:
                start_after = resp.records[-1].key

    async def delete_record_prefix(self, prefix: bytes) -> int:
        req = pb.DeleteRecordPrefixRequest(tenant=self.tenant, prefix=prefix)
        return (await self._call(self._stub.DeleteRecordPrefix, req)).removed

    # ---- Blobs ---------------------------------------------------------

    async def put_blob(self, name: str, data: bytes | BinaryIO) -> int:
        try:
            return (await self._stub.PutBlob(blob_chunks(self.tenant, name, data))).size
        except grpc.RpcError as e:
            raise translate(e) from None

    async def open_blob(self, name: str) -> AsyncIterator[bytes] | None:
        call = self._stub.GetBlob(pb.GetBlobRequest(tenant=self.tenant, name=name))
        try:
            first = await call.read()
        except grpc.RpcError as e:
            err = translate(e)
            if isinstance(err, NotFound):
                return None
            raise err from None

        async def chunks() -> AsyncIterator[bytes]:
            if first is grpc.aio.EOF:
                return
            yield first.data
            try:
                while (c := await call.read()) is not grpc.aio.EOF:
                    yield c.data
            except grpc.RpcError as e:
                raise translate(e) from None

        return chunks()

    async def get_blob(self, name: str) -> bytes | None:
        chunks = await self.open_blob(name)
        if chunks is None:
            return None
        return b"".join([c async for c in chunks])

    async def blob_exists(self, name: str) -> bool:
        return (await self._call(self._stub.StatBlob, pb.StatBlobRequest(tenant=self.tenant, name=name))).exists

    async def delete_blob(self, name: str) -> bool:
        return (await self._call(self._stub.DeleteBlob, pb.DeleteBlobRequest(tenant=self.tenant, name=name))).existed
