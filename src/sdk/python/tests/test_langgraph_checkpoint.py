"""LangGraph checkpointer: the official conformance suite plus real graphs."""

from __future__ import annotations

import operator
import uuid
from typing import Annotated, TypedDict

import pytest

pytest.importorskip("langgraph")
conformance = pytest.importorskip("langgraph.checkpoint.conformance")

from langgraph.graph import END, START, StateGraph  # noqa: E402

from agent_state_storage import Client  # noqa: E402
from agent_state_storage.langgraph import AgentStateSaver  # noqa: E402


async def test_conformance_base_and_copy_thread(node):
    @conformance.checkpointer_test(name="AgentStateSaver")
    async def saver_factory():
        # A fresh tenant per suite gives every capability an empty store.
        with Client(node.address, f"conf-{uuid.uuid4().hex[:8]}") as c:
            yield AgentStateSaver(c)

    report = await conformance.validate(saver_factory)
    failures = {
        name: [t for t in (r.failures or [])]
        for name, r in report.results.items()
        if r.detected and r.passed is not True
    }
    assert report.passed_all(), failures
    detected = {str(getattr(n, "value", n)) for n, r in report.results.items() if r.detected}
    assert {"put", "put_writes", "get_tuple", "list", "delete_thread", "copy_thread"} <= detected


class Counter(TypedDict):
    count: int
    log: Annotated[list[str], operator.add]


def _graph(saver):
    g = StateGraph(Counter)
    g.add_node("inc", lambda s: {"count": s["count"] + 1, "log": [f"inc->{s['count'] + 1}"]})
    g.add_edge(START, "inc")
    g.add_edge("inc", END)
    return g.compile(checkpointer=saver)


def test_graph_resumes_after_node_restart(fresh_node, tenant):
    cfg = {"configurable": {"thread_id": "t1"}}
    with Client(fresh_node.address, tenant) as c:
        app = _graph(AgentStateSaver(c))
        app.invoke({"count": 0, "log": []}, cfg)
        app.invoke({"log": ["again"]}, cfg)
    fresh_node.restart()
    with Client(fresh_node.address, tenant) as c:
        app = _graph(AgentStateSaver(c))
        state = app.get_state(cfg)
        assert state.values["count"] == 2
        out = app.invoke({"log": ["third"]}, cfg)
        assert out["count"] == 3
        assert out["log"] == ["inc->1", "again", "inc->2", "third", "inc->3"]


def _time_travel(saver) -> dict:
    app = _graph(saver)
    cfg = {"configurable": {"thread_id": "tt"}}
    app.invoke({"count": 0, "log": []}, cfg)
    app.invoke({"log": []}, cfg)
    after_first = next(s for s in app.get_state_history(cfg)
                       if s.values.get("count") == 1 and not s.next)
    # Branch from the state after the first run, as LangGraph time travel does.
    branch_cfg = app.update_state(after_first.config, {"log": ["branched"]})
    app.invoke({"log": ["on-branch"]}, branch_cfg)
    return {
        "latest": app.get_state(cfg).values,
        "history": [s.values for s in app.get_state_history(cfg)],
        "parents": [s.parent_config is not None for s in app.get_state_history(cfg)],
    }


def test_time_travel_fork_matches_in_memory_saver(client):
    from langgraph.checkpoint.memory import InMemorySaver

    ours = _time_travel(AgentStateSaver(client))
    reference = _time_travel(InMemorySaver())
    assert ours == reference
    assert ours["latest"]["log"] == ["inc->1", "branched", "on-branch", "inc->2"]
    # The original second run is still in the history next to the branch.
    assert ["inc->1", "inc->2"] in [h.get("log") for h in ours["history"]]


def test_thread_ids_with_separators_do_not_collide(client):
    saver = AgentStateSaver(client)
    app = _graph(saver)
    ids = ["a\x00b", "a", "a/b", "a\x00", "é"]
    for i, tid in enumerate(ids):
        app.invoke({"count": i * 10, "log": []}, {"configurable": {"thread_id": tid}})
    for i, tid in enumerate(ids):
        assert app.get_state({"configurable": {"thread_id": tid}}).values["count"] == i * 10 + 1
    saver.delete_thread("a")
    assert app.get_state({"configurable": {"thread_id": "a"}}).values == {}
    assert app.get_state({"configurable": {"thread_id": "a\x00b"}}).values["count"] == 1


async def test_async_graph(aclient, client):
    app = _graph(AgentStateSaver(client))
    cfg = {"configurable": {"thread_id": "async"}}
    out = await app.ainvoke({"count": 5, "log": []}, cfg)
    assert out["count"] == 6
    assert (await app.aget_state(cfg)).values["count"] == 6


def test_integer_thread_ids(client):
    from langgraph.checkpoint.base import empty_checkpoint

    saver = AgentStateSaver(client)
    cp = empty_checkpoint()
    saver.put({"configurable": {"thread_id": 7, "checkpoint_ns": ""}}, cp, {}, {})
    # LangGraph's own savers store str(thread_id), so 7 and "7" name one thread.
    for tid in (7, "7"):
        got = saver.get_tuple({"configurable": {"thread_id": tid}})
        assert got is not None and got.checkpoint["id"] == cp["id"]
    assert {t.config["configurable"]["thread_id"] for t in saver.list(None)} == {"7"}


def test_checkpoint_with_large_channels(client):
    class Big(TypedDict):
        a: str
        b: str

    g = StateGraph(Big)
    g.add_node("fill", lambda s: {"a": "x" * (2500 * 1024), "b": "y" * (2500 * 1024)})
    g.add_edge(START, "fill")
    g.add_edge("fill", END)
    app = g.compile(checkpointer=AgentStateSaver(client))
    cfg = {"configurable": {"thread_id": "big"}}
    app.invoke({"a": "", "b": ""}, cfg)
    assert len(app.get_state(cfg).values["a"]) == 2500 * 1024
