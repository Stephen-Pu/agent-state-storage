"""M1 quickstart: keep an agent's state in AgentStateStorage. No API key needed.

    kvstore-node --grpc-port 7000 --state-dir ./agent-state
    pip install -e "src/sdk/python[openai-agents,langgraph]"
    python examples/m1_quickstart.py

Run it again (restart the node in between if you like): the LangGraph thread
and the OpenAI Agents conversation continue where they stopped.
"""

from __future__ import annotations

import argparse
import asyncio
import operator
from typing import Annotated, TypedDict

from agents import Agent, Runner, set_tracing_disabled
from agents.testing import ScriptedModel, assistant_message
from langgraph.graph import END, START, StateGraph

from agent_state_storage import AsyncClient, Client
from agent_state_storage.langgraph import AgentStateSaver, AgentStateStore
from agent_state_storage.openai_agents import AgentStateSession


class Counter(TypedDict):
    count: int
    log: Annotated[list[str], operator.add]


def langgraph_demo(client: Client) -> None:
    saver, store = AgentStateSaver(client), AgentStateStore(client)

    def inc(state: Counter) -> dict:
        n = state.get("count", 0) + 1
        return {"count": n, "log": [f"inc->{n}"]}

    builder = StateGraph(Counter)
    builder.add_node("inc", inc)
    builder.add_edge(START, "inc")
    builder.add_edge("inc", END)
    app = builder.compile(checkpointer=saver, store=store)

    thread = {"configurable": {"thread_id": "demo-thread"}}
    state = app.invoke({"log": []}, thread)
    print(f"LangGraph: count={state['count']} log={state['log']}")

    # Fork: copy the thread and continue the copy; the original is untouched.
    fork_id = f"demo-thread-fork-{state['count']}"
    saver.copy_thread("demo-thread", fork_id)
    forked = app.invoke({"log": ["on-fork"]}, {"configurable": {"thread_id": fork_id}})
    print(f"LangGraph fork {fork_id}: count={forked['count']} "
          f"(original stays {app.get_state(thread).values['count']})")

    # Long-term memory shared across threads.
    runs = store.get(("demo", "stats"), "runs")
    total = (runs.value["n"] if runs else 0) + 1
    store.put(("demo", "stats"), "runs", {"n": total})
    print(f"LangGraph store: this demo has run {total} time(s)")


async def openai_agents_demo(target: str, tenant: str) -> None:
    async with AsyncClient(target, tenant) as client:
        session = AgentStateSession("demo-conv", client)
        turn = len(await session.get_items()) // 2 + 1
        # A scripted model stands in for a real one, so no API key is needed.
        agent = Agent(name="demo", model=ScriptedModel([[assistant_message(f"reply {turn}")]]))
        await Runner.run(agent, f"message {turn}", session=session)
        items = await session.get_items()
        print(f"OpenAI Agents: {len(items)} items in demo-conv, "
              f"last reply {items[-1]['content'][0]['text']!r}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--target", default="localhost:7000")
    parser.add_argument("--tenant", default="demo")
    args = parser.parse_args()
    set_tracing_disabled(True)
    with Client(args.target, args.tenant) as client:
        langgraph_demo(client)
    asyncio.run(openai_agents_demo(args.target, args.tenant))


if __name__ == "__main__":
    main()
