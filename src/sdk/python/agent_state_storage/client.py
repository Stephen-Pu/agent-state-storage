"""Synchronous client for the agentstate.v1 service."""

from __future__ import annotations

from typing import BinaryIO, Iterator, Sequence

import grpc

from ._common import CHANNEL_OPTIONS, Record, TlsConfig, blob_chunks, scan_request
from ._errors import NotFound, translate
from ._proto import agentstate_pb2 as pb
from ._proto import agentstate_pb2_grpc as pb_grpc


class Client:
    """Talks to one kvstore-node.

    `tenant` is required without TLS. With mTLS the server takes the tenant
    from the client certificate; leave `tenant` empty or set it to the same
    value.
    """

    def __init__(self, target: str, tenant: str = "", *, tls: TlsConfig | None = None,
                 timeout: float = 30.0):
        self.tenant = tenant
        self.timeout = timeout
        if tls is None:
            self._channel = grpc.insecure_channel(target, options=CHANNEL_OPTIONS)
        else:
            self._channel = grpc.secure_channel(target, tls.credentials(),
                                                options=CHANNEL_OPTIONS + tls.options())
        self._stub = pb_grpc.AgentStateStub(self._channel)

    def __enter__(self) -> "Client":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def close(self) -> None:
        self._channel.close()

    def _call(self, method, request):
        try:
            return method(request, timeout=self.timeout)
        except grpc.RpcError as e:
            raise translate(e) from None

    # ---- Items ---------------------------------------------------------

    def append_items(self, session: str, items: Sequence[bytes]) -> int:
        """Append items; returns the sequence number the next item will get."""
        req = pb.AppendItemsRequest(tenant=self.tenant, session=session, items=list(items))
        return self._call(self._stub.AppendItems, req).next_seq

    def get_items(self, session: str, limit: int | None = None) -> list[bytes]:
        """The latest `limit` items (all when None), oldest first."""
        req = pb.GetItemsRequest(tenant=self.tenant, session=session, limit=limit or 0)
        return list(self._call(self._stub.GetItems, req).items)

    def pop_item(self, session: str) -> bytes | None:
        resp = self._call(self._stub.PopItem, pb.PopItemRequest(tenant=self.tenant, session=session))
        return resp.item if resp.found else None

    def clear_session(self, session: str) -> int:
        req = pb.ClearSessionRequest(tenant=self.tenant, session=session)
        return self._call(self._stub.ClearSession, req).removed

    def fork_session(self, source: str, target: str) -> int:
        """Copy every item of `source` into the empty session `target`."""
        req = pb.ForkSessionRequest(tenant=self.tenant, source=source, target=target)
        return self._call(self._stub.ForkSession, req).copied

    # ---- Records -------------------------------------------------------

    def write_records(self, puts: Sequence[Record] = (), deletes: Sequence[bytes] = ()) -> None:
        """Apply puts and deletes atomically."""
        req = pb.WriteRecordsRequest(
            tenant=self.tenant,
            puts=[pb.Record(key=k, value=v) for k, v in puts],
            deletes=list(deletes),
        )
        self._call(self._stub.WriteRecords, req)

    def get_records(self, keys: Sequence[bytes]) -> dict[bytes, bytes]:
        resp = self._call(self._stub.GetRecords, pb.GetRecordsRequest(tenant=self.tenant, keys=list(keys)))
        return {r.key: r.value for r in resp.found}

    def scan_records(self, prefix: bytes = b"", *, start_after: bytes = b"", end_before: bytes = b"",
                     limit: int | None = None, reverse: bool = False) -> Iterator[Record]:
        """Records under `prefix` strictly between the bounds, paging through the server cap."""
        remaining = limit
        while remaining is None or remaining > 0:
            req = scan_request(self.tenant, prefix, start_after, end_before, remaining, reverse)
            resp = self._call(self._stub.ScanRecords, req)
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

    def delete_record_prefix(self, prefix: bytes) -> int:
        req = pb.DeleteRecordPrefixRequest(tenant=self.tenant, prefix=prefix)
        return self._call(self._stub.DeleteRecordPrefix, req).removed

    # ---- Blobs ---------------------------------------------------------

    def put_blob(self, name: str, data: bytes | BinaryIO) -> int:
        """Store `data` (bytes or a binary file object) under `name`; returns its size."""
        try:
            return self._stub.PutBlob(blob_chunks(self.tenant, name, data)).size
        except grpc.RpcError as e:
            raise translate(e) from None

    def open_blob(self, name: str) -> Iterator[bytes] | None:
        """The blob as an iterator of chunks, or None if it does not exist."""
        stream = self._stub.GetBlob(pb.GetBlobRequest(tenant=self.tenant, name=name))
        try:
            first = next(stream, None)
        except grpc.RpcError as e:
            err = translate(e)
            if isinstance(err, NotFound):
                return None
            raise err from None

        def chunks() -> Iterator[bytes]:
            if first is None:
                return
            yield first.data
            try:
                for c in stream:
                    yield c.data
            except grpc.RpcError as e:
                raise translate(e) from None

        return chunks()

    def get_blob(self, name: str) -> bytes | None:
        chunks = self.open_blob(name)
        return None if chunks is None else b"".join(chunks)

    def blob_exists(self, name: str) -> bool:
        return self._call(self._stub.StatBlob, pb.StatBlobRequest(tenant=self.tenant, name=name)).exists

    def delete_blob(self, name: str) -> bool:
        return self._call(self._stub.DeleteBlob, pb.DeleteBlobRequest(tenant=self.tenant, name=name)).existed
