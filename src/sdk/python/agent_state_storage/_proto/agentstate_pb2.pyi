from google.protobuf.internal import containers as _containers
from google.protobuf import descriptor as _descriptor
from google.protobuf import message as _message
from typing import ClassVar as _ClassVar, Iterable as _Iterable, Mapping as _Mapping, Optional as _Optional, Union as _Union

DESCRIPTOR: _descriptor.FileDescriptor

class AppendItemsRequest(_message.Message):
    __slots__ = ("tenant", "session", "items")
    TENANT_FIELD_NUMBER: _ClassVar[int]
    SESSION_FIELD_NUMBER: _ClassVar[int]
    ITEMS_FIELD_NUMBER: _ClassVar[int]
    tenant: str
    session: str
    items: _containers.RepeatedScalarFieldContainer[bytes]
    def __init__(self, tenant: _Optional[str] = ..., session: _Optional[str] = ..., items: _Optional[_Iterable[bytes]] = ...) -> None: ...

class AppendItemsResponse(_message.Message):
    __slots__ = ("next_seq",)
    NEXT_SEQ_FIELD_NUMBER: _ClassVar[int]
    next_seq: int
    def __init__(self, next_seq: _Optional[int] = ...) -> None: ...

class GetItemsRequest(_message.Message):
    __slots__ = ("tenant", "session", "limit")
    TENANT_FIELD_NUMBER: _ClassVar[int]
    SESSION_FIELD_NUMBER: _ClassVar[int]
    LIMIT_FIELD_NUMBER: _ClassVar[int]
    tenant: str
    session: str
    limit: int
    def __init__(self, tenant: _Optional[str] = ..., session: _Optional[str] = ..., limit: _Optional[int] = ...) -> None: ...

class GetItemsResponse(_message.Message):
    __slots__ = ("items",)
    ITEMS_FIELD_NUMBER: _ClassVar[int]
    items: _containers.RepeatedScalarFieldContainer[bytes]
    def __init__(self, items: _Optional[_Iterable[bytes]] = ...) -> None: ...

class PopItemRequest(_message.Message):
    __slots__ = ("tenant", "session")
    TENANT_FIELD_NUMBER: _ClassVar[int]
    SESSION_FIELD_NUMBER: _ClassVar[int]
    tenant: str
    session: str
    def __init__(self, tenant: _Optional[str] = ..., session: _Optional[str] = ...) -> None: ...

class PopItemResponse(_message.Message):
    __slots__ = ("found", "item")
    FOUND_FIELD_NUMBER: _ClassVar[int]
    ITEM_FIELD_NUMBER: _ClassVar[int]
    found: bool
    item: bytes
    def __init__(self, found: bool = ..., item: _Optional[bytes] = ...) -> None: ...

class ClearSessionRequest(_message.Message):
    __slots__ = ("tenant", "session")
    TENANT_FIELD_NUMBER: _ClassVar[int]
    SESSION_FIELD_NUMBER: _ClassVar[int]
    tenant: str
    session: str
    def __init__(self, tenant: _Optional[str] = ..., session: _Optional[str] = ...) -> None: ...

class ClearSessionResponse(_message.Message):
    __slots__ = ("removed",)
    REMOVED_FIELD_NUMBER: _ClassVar[int]
    removed: int
    def __init__(self, removed: _Optional[int] = ...) -> None: ...

class ForkSessionRequest(_message.Message):
    __slots__ = ("tenant", "source", "target")
    TENANT_FIELD_NUMBER: _ClassVar[int]
    SOURCE_FIELD_NUMBER: _ClassVar[int]
    TARGET_FIELD_NUMBER: _ClassVar[int]
    tenant: str
    source: str
    target: str
    def __init__(self, tenant: _Optional[str] = ..., source: _Optional[str] = ..., target: _Optional[str] = ...) -> None: ...

class ForkSessionResponse(_message.Message):
    __slots__ = ("copied",)
    COPIED_FIELD_NUMBER: _ClassVar[int]
    copied: int
    def __init__(self, copied: _Optional[int] = ...) -> None: ...

class Record(_message.Message):
    __slots__ = ("key", "value")
    KEY_FIELD_NUMBER: _ClassVar[int]
    VALUE_FIELD_NUMBER: _ClassVar[int]
    key: bytes
    value: bytes
    def __init__(self, key: _Optional[bytes] = ..., value: _Optional[bytes] = ...) -> None: ...

class WriteRecordsRequest(_message.Message):
    __slots__ = ("tenant", "puts", "deletes")
    TENANT_FIELD_NUMBER: _ClassVar[int]
    PUTS_FIELD_NUMBER: _ClassVar[int]
    DELETES_FIELD_NUMBER: _ClassVar[int]
    tenant: str
    puts: _containers.RepeatedCompositeFieldContainer[Record]
    deletes: _containers.RepeatedScalarFieldContainer[bytes]
    def __init__(self, tenant: _Optional[str] = ..., puts: _Optional[_Iterable[_Union[Record, _Mapping]]] = ..., deletes: _Optional[_Iterable[bytes]] = ...) -> None: ...

class WriteRecordsResponse(_message.Message):
    __slots__ = ()
    def __init__(self) -> None: ...

class GetRecordsRequest(_message.Message):
    __slots__ = ("tenant", "keys")
    TENANT_FIELD_NUMBER: _ClassVar[int]
    KEYS_FIELD_NUMBER: _ClassVar[int]
    tenant: str
    keys: _containers.RepeatedScalarFieldContainer[bytes]
    def __init__(self, tenant: _Optional[str] = ..., keys: _Optional[_Iterable[bytes]] = ...) -> None: ...

class GetRecordsResponse(_message.Message):
    __slots__ = ("found",)
    FOUND_FIELD_NUMBER: _ClassVar[int]
    found: _containers.RepeatedCompositeFieldContainer[Record]
    def __init__(self, found: _Optional[_Iterable[_Union[Record, _Mapping]]] = ...) -> None: ...

class ScanRecordsRequest(_message.Message):
    __slots__ = ("tenant", "prefix", "start_after", "end_before", "limit", "reverse")
    TENANT_FIELD_NUMBER: _ClassVar[int]
    PREFIX_FIELD_NUMBER: _ClassVar[int]
    START_AFTER_FIELD_NUMBER: _ClassVar[int]
    END_BEFORE_FIELD_NUMBER: _ClassVar[int]
    LIMIT_FIELD_NUMBER: _ClassVar[int]
    REVERSE_FIELD_NUMBER: _ClassVar[int]
    tenant: str
    prefix: bytes
    start_after: bytes
    end_before: bytes
    limit: int
    reverse: bool
    def __init__(self, tenant: _Optional[str] = ..., prefix: _Optional[bytes] = ..., start_after: _Optional[bytes] = ..., end_before: _Optional[bytes] = ..., limit: _Optional[int] = ..., reverse: bool = ...) -> None: ...

class ScanRecordsResponse(_message.Message):
    __slots__ = ("records", "has_more")
    RECORDS_FIELD_NUMBER: _ClassVar[int]
    HAS_MORE_FIELD_NUMBER: _ClassVar[int]
    records: _containers.RepeatedCompositeFieldContainer[Record]
    has_more: bool
    def __init__(self, records: _Optional[_Iterable[_Union[Record, _Mapping]]] = ..., has_more: bool = ...) -> None: ...

class DeleteRecordPrefixRequest(_message.Message):
    __slots__ = ("tenant", "prefix")
    TENANT_FIELD_NUMBER: _ClassVar[int]
    PREFIX_FIELD_NUMBER: _ClassVar[int]
    tenant: str
    prefix: bytes
    def __init__(self, tenant: _Optional[str] = ..., prefix: _Optional[bytes] = ...) -> None: ...

class DeleteRecordPrefixResponse(_message.Message):
    __slots__ = ("removed",)
    REMOVED_FIELD_NUMBER: _ClassVar[int]
    removed: int
    def __init__(self, removed: _Optional[int] = ...) -> None: ...

class PutBlobChunk(_message.Message):
    __slots__ = ("tenant", "name", "data")
    TENANT_FIELD_NUMBER: _ClassVar[int]
    NAME_FIELD_NUMBER: _ClassVar[int]
    DATA_FIELD_NUMBER: _ClassVar[int]
    tenant: str
    name: str
    data: bytes
    def __init__(self, tenant: _Optional[str] = ..., name: _Optional[str] = ..., data: _Optional[bytes] = ...) -> None: ...

class PutBlobResponse(_message.Message):
    __slots__ = ("size",)
    SIZE_FIELD_NUMBER: _ClassVar[int]
    size: int
    def __init__(self, size: _Optional[int] = ...) -> None: ...

class GetBlobRequest(_message.Message):
    __slots__ = ("tenant", "name")
    TENANT_FIELD_NUMBER: _ClassVar[int]
    NAME_FIELD_NUMBER: _ClassVar[int]
    tenant: str
    name: str
    def __init__(self, tenant: _Optional[str] = ..., name: _Optional[str] = ...) -> None: ...

class BlobChunk(_message.Message):
    __slots__ = ("data",)
    DATA_FIELD_NUMBER: _ClassVar[int]
    data: bytes
    def __init__(self, data: _Optional[bytes] = ...) -> None: ...

class StatBlobRequest(_message.Message):
    __slots__ = ("tenant", "name")
    TENANT_FIELD_NUMBER: _ClassVar[int]
    NAME_FIELD_NUMBER: _ClassVar[int]
    tenant: str
    name: str
    def __init__(self, tenant: _Optional[str] = ..., name: _Optional[str] = ...) -> None: ...

class StatBlobResponse(_message.Message):
    __slots__ = ("exists", "size")
    EXISTS_FIELD_NUMBER: _ClassVar[int]
    SIZE_FIELD_NUMBER: _ClassVar[int]
    exists: bool
    size: int
    def __init__(self, exists: bool = ..., size: _Optional[int] = ...) -> None: ...

class DeleteBlobRequest(_message.Message):
    __slots__ = ("tenant", "name")
    TENANT_FIELD_NUMBER: _ClassVar[int]
    NAME_FIELD_NUMBER: _ClassVar[int]
    tenant: str
    name: str
    def __init__(self, tenant: _Optional[str] = ..., name: _Optional[str] = ...) -> None: ...

class DeleteBlobResponse(_message.Message):
    __slots__ = ("existed",)
    EXISTED_FIELD_NUMBER: _ClassVar[int]
    existed: bool
    def __init__(self, existed: bool = ...) -> None: ...
