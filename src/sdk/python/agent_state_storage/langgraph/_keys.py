"""Length-prefixed key and value framing for LangGraph state in records.

Each part is `u32 big-endian length | bytes`, so a part can hold any bytes
(including separators) without colliding with its neighbours, and a key
built from the first N parts is a prefix of every key extending them.
"""

from __future__ import annotations

import struct


def pack(*parts: str | bytes) -> bytes:
    out = bytearray()
    for p in parts:
        b = p.encode() if isinstance(p, str) else bytes(p)
        out += struct.pack(">I", len(b))
        out += b
    return bytes(out)


def unpack(data: bytes) -> list[bytes]:
    parts, off = [], 0
    while off < len(data):
        (n,) = struct.unpack_from(">I", data, off)
        off += 4
        parts.append(data[off:off + n])
        off += n
    return parts
