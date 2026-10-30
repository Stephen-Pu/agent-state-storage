"""Pieces shared by the sync and async clients."""

from __future__ import annotations

import io
from dataclasses import dataclass
from typing import BinaryIO, Iterator, NamedTuple

import grpc

from ._proto import agentstate_pb2 as pb

BLOB_CHUNK = 1 << 20
# Server cap per ScanRecords page.
SCAN_PAGE = 1000
CHANNEL_OPTIONS = [
    # GetItems on a long session can exceed gRPC's 4 MiB default.
    ("grpc.max_receive_message_length", 64 << 20),
]


class Record(NamedTuple):
    key: bytes
    value: bytes


@dataclass(frozen=True)
class TlsConfig:
    """Client-certificate TLS. With mTLS the server takes the tenant from `cert`."""

    ca: bytes
    cert: bytes
    key: bytes
    server_name: str | None = None

    def credentials(self) -> grpc.ChannelCredentials:
        return grpc.ssl_channel_credentials(
            root_certificates=self.ca, private_key=self.key, certificate_chain=self.cert
        )

    def options(self) -> list[tuple[str, str]]:
        if self.server_name:
            return [("grpc.ssl_target_name_override", self.server_name)]
        return []


def blob_chunks(tenant: str, name: str, data: bytes | BinaryIO) -> Iterator[pb.PutBlobChunk]:
    stream = io.BytesIO(data) if isinstance(data, (bytes, bytearray, memoryview)) else data
    first = True
    while True:
        chunk = stream.read(BLOB_CHUNK)
        if not chunk and not first:
            return
        if first:
            yield pb.PutBlobChunk(tenant=tenant, name=name, data=chunk or b"")
            first = False
        else:
            yield pb.PutBlobChunk(data=chunk)
        if not chunk:
            return


def scan_request(tenant: str, prefix: bytes, start_after: bytes, end_before: bytes,
                 remaining: int | None, reverse: bool) -> pb.ScanRecordsRequest:
    page = SCAN_PAGE if remaining is None else min(SCAN_PAGE, remaining)
    return pb.ScanRecordsRequest(tenant=tenant, prefix=prefix, start_after=start_after,
                                 end_before=end_before, limit=page, reverse=reverse)
