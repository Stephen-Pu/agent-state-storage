"""The M1 quickstart example runs end to end, and its state carries over between runs."""

from __future__ import annotations

import os
import pathlib
import subprocess
import sys

import pytest

pytest.importorskip("agents")
pytest.importorskip("langgraph")

EXAMPLE = pathlib.Path(__file__).resolve().parents[4] / "examples" / "m1_quickstart.py"


def _run(address: str, tenant: str) -> str:
    out = subprocess.run(
        [sys.executable, str(EXAMPLE), "--target", address, "--tenant", tenant],
        capture_output=True, text=True, timeout=120, env={**os.environ, "OPENAI_AGENTS_DISABLE_TRACING": "1"},
    )
    assert out.returncode == 0, out.stderr
    return out.stdout


def test_quickstart_runs_and_resumes(fresh_node, tenant):
    first = _run(fresh_node.address, tenant)
    assert "LangGraph: count=1" in first
    assert "OpenAI Agents: 2 items" in first
    fresh_node.restart()
    second = _run(fresh_node.address, tenant)
    assert "LangGraph: count=2" in second
    assert "OpenAI Agents: 4 items" in second
