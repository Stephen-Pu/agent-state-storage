"""AgentStateStorage Python SDK: the agentstate.v1 client and framework integrations.

    from agent_state_storage import Client

    with Client("localhost:7000", tenant="acme") as c:
        c.append_items("sess-1", [b'{"role": "user", "content": "hi"}'])

Integrations live in submodules: `agent_state_storage.openai_agents` and
`agent_state_storage.langgraph`.
"""

from ._aio import AsyncClient
from ._common import Record, TlsConfig
from ._errors import AgentStateError, NotFound, PermissionDenied
from .client import Client

__all__ = [
    "AgentStateError",
    "AsyncClient",
    "Client",
    "NotFound",
    "PermissionDenied",
    "Record",
    "TlsConfig",
]
__version__ = "0.1.0"
