"""The Python client against a real kvstore-node."""

from __future__ import annotations

import io

import pytest

from agent_state_storage import AgentStateError, AsyncClient, Client, NotFound, Record


def test_items_round_trip(client):
    assert client.append_items("s", [b"a", b"b"]) == 3
    client.append_items("s", [b"c"])
    assert client.get_items("s") == [b"a", b"b", b"c"]
    assert client.get_items("s", limit=2) == [b"b", b"c"]
    assert client.pop_item("s") == b"c"
    assert client.fork_session("s", "s2") == 2
    assert client.clear_session("s") == 2
    assert client.get_items("s") == []
    assert client.pop_item("s") is None
    assert client.get_items("s2") == [b"a", b"b"]


def test_fork_into_non_empty_session_raises(client):
    client.append_items("a", [b"1"])
    client.append_items("b", [b"2"])
    with pytest.raises(AgentStateError):
        client.fork_session("a", "b")


def test_records_round_trip(client):
    client.write_records([Record(b"k/1", b"v1"), Record(b"k/2", b"v2"), Record(b"x", b"y")])
    assert client.get_records([b"k/1", b"missing"]) == {b"k/1": b"v1"}
    assert [r.key for r in client.scan_records(b"k/")] == [b"k/1", b"k/2"]
    assert [r.key for r in client.scan_records(b"k/", reverse=True, limit=1)] == [b"k/2"]
    client.write_records(deletes=[b"k/1"])
    assert [r.key for r in client.scan_records(b"k/")] == [b"k/2"]
    assert client.delete_record_prefix(b"k/") == 1
    assert [r.key for r in client.scan_records()] == [b"x"]


def test_scan_pages_past_server_limit(client):
    puts = [Record(f"p/{i:05d}".encode(), b"") for i in range(2500)]
    for i in range(0, len(puts), 500):
        client.write_records(puts[i:i + 500])
    keys = [r.key for r in client.scan_records(b"p/")]
    assert keys == [r.key for r in puts]
    rev = [r.key for r in client.scan_records(b"p/", reverse=True)]
    assert rev == list(reversed(keys))
    assert len(list(client.scan_records(b"p/", limit=1234))) == 1234


def test_blob_round_trip_streams(client):
    data = bytes(i % 251 for i in range(5 * 1024 * 1024 + 17))
    assert client.put_blob("ws/snap.tar", io.BytesIO(data)) == len(data)
    assert client.blob_exists("ws/snap.tar")
    assert client.get_blob("ws/snap.tar") == data
    assert b"".join(client.open_blob("ws/snap.tar")) == data
    assert client.delete_blob("ws/snap.tar")
    assert not client.blob_exists("ws/snap.tar")
    assert client.get_blob("ws/snap.tar") is None
    assert client.open_blob("ws/snap.tar") is None


def test_missing_tenant_raises(node):
    with Client(node.address) as c, pytest.raises(AgentStateError) as e:
        c.append_items("s", [b"x"])
    assert not isinstance(e.value, NotFound)


def test_tenants_are_isolated(node):
    with Client(node.address, "iso-a") as a, Client(node.address, "iso-b") as b:
        a.append_items("same", [b"secret"])
        a.write_records([Record(b"same", b"secret")])
        assert b.get_items("same") == []
        assert b.get_records([b"same"]) == {}


async def test_async_client_matches_sync(aclient, client):
    assert await aclient.append_items("s", [b"a"]) == 2
    await aclient.write_records([Record(b"r", b"1")])
    assert client.get_items("s") == [b"a"]
    assert await aclient.get_items("s") == [b"a"]
    assert [r async for r in aclient.scan_records()] == [Record(b"r", b"1")]
    assert await aclient.put_blob("b", b"xyz") == 3
    assert await aclient.get_blob("b") == b"xyz"
    assert await aclient.blob_exists("b")
    assert await aclient.pop_item("s") == b"a"
    assert await aclient.get_records([b"r"]) == {b"r": b"1"}


def test_state_survives_node_restart(fresh_node, tenant):
    with Client(fresh_node.address, tenant) as c:
        c.append_items("s", [b"turn-1", b"turn-2"])
        c.write_records([Record(b"cp/1", b"checkpoint")])
        c.put_blob("ws", b"workspace")
    fresh_node.restart()
    with Client(fresh_node.address, tenant) as c:
        assert c.get_items("s") == [b"turn-1", b"turn-2"]
        assert c.get_records([b"cp/1"]) == {b"cp/1": b"checkpoint"}
        assert c.get_blob("ws") == b"workspace"
        assert c.append_items("s", [b"turn-3"]) == 4


MB = 1024 * 1024


def test_items_larger_than_one_message_page_through(client):
    items = [bytes([i]) * (3 * MB + 900 * 1024) for i in range(20)]  # ~78 MB in total
    for i in range(0, 20, 4):
        client.append_items("big", items[i:i + 4])  # ~15.6 MB per request
    assert client.get_items("big") == items
    assert client.get_items("big", limit=3) == items[-3:]


def test_large_records_page_through_scans(client):
    value = b"v" * (3 * MB + 900 * 1024)
    for i in range(20):
        client.write_records([Record(f"big/{i:02d}".encode(), value)])
    keys = [r.key for r in client.scan_records(b"big/")]
    assert keys == [f"big/{i:02d}".encode() for i in range(20)]
    assert [r.key for r in client.scan_records(b"big/", reverse=True, limit=2)] == keys[:-3:-1]


def test_request_with_two_large_values(client):
    two = [b"a" * (2500 * 1024), b"b" * (2500 * 1024)]
    client.append_items("two", two)
    client.write_records([Record(b"r1", two[0]), Record(b"r2", two[1])])
    assert client.get_items("two") == two
    assert client.get_records([b"r1", b"r2"]) == {b"r1": two[0], b"r2": two[1]}


def test_plaintext_listener_warns_at_startup(node):
    assert "WARNING" in node.log() and "plaintext" in node.log()
