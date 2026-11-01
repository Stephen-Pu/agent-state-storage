"""Length-prefixed key and value framing for LangGraph state in records.

Each part is `u32 big-endian length | bytes`, so a part can hold any bytes
(including separators) without colliding with its neighbours, and a key
built from the first N parts is a prefix of every key extending them.
"""

from __future__ import annotations

import struct


def pack(*parts: object) -> bytes:
    """Frame each part; bytes stay as they are, anything else is str()-encoded.

    LangGraph allows non-string thread ids (an int 7 is the thread "7", as in
    its own savers); bytes(7) would be seven NUL bytes instead.
    """
    out = bytearray()
    for p in parts:
        b = bytes(p) if isinstance(p, (bytes, bytearray, memoryview)) else str(p).encode()
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
