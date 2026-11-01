"""LangGraph long-term store backed by records."""

from __future__ import annotations

import pytest

pytest.importorskip("langgraph")

from agent_state_storage import Client  # noqa: E402
from agent_state_storage.langgraph import AgentStateStore  # noqa: E402


@pytest.fixture
def store(client):
    return AgentStateStore(client)


def test_put_get_delete(store):
    store.put(("users", "u1", "memories"), "m1", {"text": "likes tea", "n": 1})
    item = store.get(("users", "u1", "memories"), "m1")
    assert item.value == {"text": "likes tea", "n": 1}
    assert item.namespace == ("users", "u1", "memories")
    assert item.key == "m1"
    created = item.created_at
    store.put(("users", "u1", "memories"), "m1", {"text": "likes coffee", "n": 2})
    updated = store.get(("users", "u1", "memories"), "m1")
    assert updated.value["text"] == "likes coffee"
    assert updated.created_at == created
    assert updated.updated_at >= created
    store.delete(("users", "u1", "memories"), "m1")
    assert store.get(("users", "u1", "memories"), "m1") is None


def test_search_filters_and_paginates(store):
    for i in range(12):
        store.put(("docs", "team-a"), f"d{i:02d}", {"kind": "note" if i % 2 else "todo", "rank": i})
    store.put(("docs", "team-b"), "other", {"kind": "note", "rank": 99})
    notes = store.search(("docs", "team-a"), filter={"kind": "note"}, limit=100)
    assert [i.key for i in notes] == [f"d{i:02d}" for i in range(1, 12, 2)]
    high = store.search(("docs",), filter={"rank": {"$gte": 10}}, limit=100)
    assert sorted(i.key for i in high) == ["d10", "d11", "other"]
    page = store.search(("docs", "team-a"), limit=5, offset=10)
    assert [i.key for i in page] == ["d10", "d11"]
    assert len(store.search(("docs",), limit=3)) == 3


def test_list_namespaces_with_prefix_and_depth(store):
    for ns in [("a", "x", "1"), ("a", "x", "2"), ("a", "y"), ("b", "x")]:
        store.put(ns, "k", {"v": 1})
    assert store.list_namespaces() == [("a", "x", "1"), ("a", "x", "2"), ("a", "y"), ("b", "x")]
    assert store.list_namespaces(prefix=("a",)) == [("a", "x", "1"), ("a", "x", "2"), ("a", "y")]
    assert store.list_namespaces(suffix=("x",)) == [("b", "x")]
    assert store.list_namespaces(prefix=("a", "*", "2")) == [("a", "x", "2")]
    assert store.list_namespaces(max_depth=2) == [("a", "x"), ("a", "y"), ("b", "x")]
    assert store.list_namespaces(limit=2, offset=1) == [("a", "x", "2"), ("a", "y")]


def test_namespace_prefix_does_not_match_longer_label(store):
    store.put(("a",), "k", {"v": "short"})
    store.put(("ab",), "k", {"v": "long"})
    store.put(("a", "b"), "k", {"v": "child"})
    assert sorted(i.value["v"] for i in store.search(("a",))) == ["child", "short"]
    assert store.get(("a",), "k").value == {"v": "short"}


def test_query_without_index_returns_filtered_items_unscored(store):
    # Same as LangGraph's InMemoryStore without an index: the query is not
    # used for ranking (no vector search yet) and items carry no score.
    store.put(("m",), "1", {"text": "tea", "kind": "pref"})
    store.put(("m",), "2", {"text": "rain", "kind": "fact"})
    hits = store.search(("m",), query="what drink?", filter={"kind": "pref"})
    assert [(h.key, h.score) for h in hits] == [("1", None)]


async def test_async_batch(client):
    store = AgentStateStore(client)
    await store.aput(("as",), "k", {"v": 1})
    assert (await store.aget(("as",), "k")).value == {"v": 1}
    assert [i.key for i in await store.asearch(("as",))] == ["k"]
    assert await store.alist_namespaces() == [("as",)]


def test_store_survives_node_restart(fresh_node, tenant):
    with Client(fresh_node.address, tenant) as c:
        AgentStateStore(c).put(("u",), "k", {"remember": "this"})
    fresh_node.restart()
    with Client(fresh_node.address, tenant) as c:
        assert AgentStateStore(c).get(("u",), "k").value == {"remember": "this"}


def test_matches_in_memory_store(store):
    from langgraph.store.memory import InMemoryStore

    ref = InMemoryStore()
    puts = [
        (("org", "a", "notes"), "n1", {"tag": "x", "score": 3, "meta": {"lang": "en"}}),
        (("org", "a", "notes"), "n2", {"tag": "y", "score": 7, "meta": {"lang": "fr"}}),
        (("org", "b"), "n3", {"tag": "x", "score": 5, "meta": {"lang": "en"}}),
        (("other",), "n4", {"tag": "x", "score": 1, "list": [1, 2]}),
    ]
    for s in (store, ref):
        for ns, k, v in puts:
            s.put(ns, k, v)
        s.delete(("org", "b"), "missing")

    def keys(s, *args, **kw):
        return sorted(i.key for i in s.search(*args, **kw))

    for args, kw in [
        ((("org",),), {}),
        ((("org",),), {"filter": {"tag": "x"}}),
        (((),), {"filter": {"score": {"$gt": 2, "$lte": 7}}}),
        (((),), {"filter": {"meta": {"lang": "en"}}}),
        (((),), {"filter": {"list": [1, 2]}}),
        (((),), {"filter": {"tag": {"$ne": "x"}}}),
    ]:
        assert keys(store, *args, **kw) == keys(ref, *args, **kw), (args, kw)
    for kw in [{}, {"prefix": ("org",)}, {"suffix": ("notes",)}, {"max_depth": 1},
               {"prefix": ("org", "*")}]:
        assert store.list_namespaces(**kw) == ref.list_namespaces(**kw), kw
