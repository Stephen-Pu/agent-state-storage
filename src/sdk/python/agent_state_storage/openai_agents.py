"""OpenAI Agents SDK integration.

`AgentStateSession` implements the SDK's `Session` protocol, so conversation
history survives a process restart and can be forked:

    from agents import Agent, Runner
    from agent_state_storage import AsyncClient
    from agent_state_storage.openai_agents import AgentStateSession

    client = AsyncClient("localhost:7000", tenant="acme")
    await Runner.run(agent, "hi", session=AgentStateSession("conv-1", client))

`AgentStateSnapshotClient` stores sandbox workspace snapshots. Bind it in the
sandbox dependencies and point a `RemoteSnapshotSpec` at the same key:

    Dependencies.with_values({"agent-state": AgentStateSnapshotClient(client)})
    RemoteSnapshotSpec(client_dependency_key="agent-state")

Requires `pip install "agent-state-storage[openai-agents]"`.
"""

from __future__ import annotations

import io
import json
import tempfile
from typing import Any

from agents.memory import SessionSettings
from agents.memory.session_settings import resolve_session_limit

from ._aio import AsyncClient

_SPOOL_IN_MEMORY = 16 << 20


class AgentStateSession:
    """Conversation history for one session id, stored as one item per message."""

    def __init__(self, session_id: str, client: AsyncClient, *,
                 session_settings: SessionSettings | None = None):
        self.session_id = session_id
        self.session_settings = session_settings or SessionSettings()
        self._client = client

    async def get_items(self, limit: int | None = None) -> list[Any]:
        """The latest `limit` items (session_settings.limit when None), oldest first."""
        limit = resolve_session_limit(limit, self.session_settings)
        raw = await self._client.get_items(self.session_id, limit=limit)
        return [json.loads(b) for b in raw]

    async def add_items(self, items: list[Any]) -> None:
        if not items:
            return
        await self._client.append_items(
            self.session_id, [json.dumps(item, separators=(",", ":")).encode() for item in items]
        )

    async def pop_item(self) -> Any | None:
        raw = await self._client.pop_item(self.session_id)
        return None if raw is None else json.loads(raw)

    async def clear_session(self) -> None:
        await self._client.clear_session(self.session_id)

    async def fork(self, new_session_id: str) -> "AgentStateSession":
        """Copy this conversation into `new_session_id` (which must be empty) and return it."""
        await self._client.fork_session(self.session_id, new_session_id)
        return AgentStateSession(new_session_id, self._client,
                                 session_settings=self.session_settings)


class AgentStateSnapshotClient:
    """The upload / download / exists client a sandbox `RemoteSnapshot` resolves."""

    def __init__(self, client: AsyncClient, *, prefix: str = "openai-agents/sandbox/"):
        self._client = client
        self._prefix = prefix

    def _name(self, snapshot_id: str) -> str:
        return self._prefix + snapshot_id

    async def upload(self, snapshot_id: str, data: io.IOBase) -> None:
        await self._client.put_blob(self._name(snapshot_id), data)

    async def download(self, snapshot_id: str) -> io.IOBase:
        chunks = await self._client.open_blob(self._name(snapshot_id))
        if chunks is None:
            raise FileNotFoundError(f"no snapshot {snapshot_id!r}")
        out = tempfile.SpooledTemporaryFile(max_size=_SPOOL_IN_MEMORY)
        async for chunk in chunks:
            out.write(chunk)
        out.seek(0)
        return out

    async def exists(self, snapshot_id: str) -> bool:
        return await self._client.blob_exists(self._name(snapshot_id))
