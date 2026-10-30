"""OpenAI Agents SDK integration: Session protocol and sandbox snapshot client."""

from __future__ import annotations

import io

import pytest

agents = pytest.importorskip("agents")

from agents import Agent, Runner, set_tracing_disabled  # noqa: E402
from agents.memory import Session  # noqa: E402
from agents.sandbox.session.dependencies import Dependencies  # noqa: E402
from agents.sandbox.snapshot import RemoteSnapshotSpec  # noqa: E402
from agents.testing import ScriptedModel, assistant_message  # noqa: E402

from agent_state_storage import AsyncClient  # noqa: E402
from agent_state_storage.openai_agents import (  # noqa: E402
    AgentStateSession,
    AgentStateSnapshotClient,
)

set_tracing_disabled(True)  # the scripted model needs no API key; skip trace export


def _texts(items) -> list[str]:
    out = []
    for item in items:
        content = item.get("content")
        if isinstance(content, str):
            out.append(content)
        elif isinstance(content, list):
            out.extend(part.get("text", "") for part in content if isinstance(part, dict))
    return out


async def test_session_protocol_conformance(aclient):
    assert isinstance(AgentStateSession("s", aclient), Session)


async def test_session_methods(aclient):
    s = AgentStateSession("s", aclient)
    await s.add_items([{"role": "user", "content": "a"}, {"role": "user", "content": "b"}])
    await s.add_items([{"role": "user", "content": "c"}])
    assert _texts(await s.get_items()) == ["a", "b", "c"]
    assert _texts(await s.get_items(limit=2)) == ["b", "c"]
    assert (await s.pop_item())["content"] == "c"
    await s.clear_session()
    assert await s.get_items() == []
    assert await s.pop_item() is None


async def test_session_settings_limit_applies(aclient):
    from agents.memory import SessionSettings

    s = AgentStateSession("s", aclient, session_settings=SessionSettings(limit=1))
    await s.add_items([{"role": "user", "content": "a"}, {"role": "user", "content": "b"}])
    assert _texts(await s.get_items()) == ["b"]


async def test_runner_persists_history(node, tenant):
    model = ScriptedModel([[assistant_message("hello 1")], [assistant_message("hello 2")]])
    agent = Agent(name="a", instructions="be brief", model=model)
    async with AsyncClient(node.address, tenant) as c:
        await Runner.run(agent, "first question", session=AgentStateSession("conv", c))
    # A new process would start here: new client, new session object, same id.
    async with AsyncClient(node.address, tenant) as c:
        session = AgentStateSession("conv", c)
        await Runner.run(agent, "second question", session=session)
        assert "first question" in str(model.last_call.input)
        assert "hello 1" in str(model.last_call.input)
        texts = _texts(await session.get_items())
    assert texts == ["first question", "hello 1", "second question", "hello 2"]


async def test_fork_continues_independently(aclient):
    main = AgentStateSession("main", aclient)
    await main.add_items([{"role": "user", "content": "shared"}])
    branch = await main.fork("branch")
    await branch.add_items([{"role": "user", "content": "only-branch"}])
    await main.add_items([{"role": "user", "content": "only-main"}])
    assert _texts(await main.get_items()) == ["shared", "only-main"]
    assert _texts(await branch.get_items()) == ["shared", "only-branch"]


async def test_snapshot_client_round_trip(aclient):
    client = AgentStateSnapshotClient(aclient)
    data = bytes(range(256)) * 9000  # ~2.3 MB, several chunks
    assert not await client.exists("snap-1")
    await client.upload("snap-1", io.BytesIO(data))
    assert await client.exists("snap-1")
    restored = await client.download("snap-1")
    assert restored.read() == data


async def test_remote_snapshot_spec_uses_the_client(aclient):
    deps = Dependencies.with_values({"agent-state": AgentStateSnapshotClient(aclient)})
    snapshot = RemoteSnapshotSpec(client_dependency_key="agent-state").build("ws-42")
    await snapshot.persist(io.BytesIO(b"tar bytes"), dependencies=deps)
    assert await snapshot.restorable(dependencies=deps)
    assert (await snapshot.restore(dependencies=deps)).read() == b"tar bytes"
