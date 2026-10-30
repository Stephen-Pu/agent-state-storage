"""Fixtures: a real kvstore-node process serving agentstate.v1.

The node binary comes from $KVSTORE_NODE_BIN, else the first build tree that
has one (build/, then build-*/). Tests that need it are skipped without it.
"""

from __future__ import annotations

import os
import pathlib
import re
import subprocess
import tempfile
import time
import uuid

import pytest

REPO = pathlib.Path(__file__).resolve().parents[4]


def _node_binary() -> pathlib.Path | None:
    env = os.environ.get("KVSTORE_NODE_BIN")
    if env:
        return pathlib.Path(env)
    for build in [REPO / "build", *sorted(REPO.glob("build-*"))]:
        candidate = build / "kvstore-node" / "kvstore-node"
        if candidate.is_file():
            return candidate
    return None


class Node:
    """One kvstore-node process with its own --state-dir."""

    def __init__(self, binary: pathlib.Path, state_dir: pathlib.Path):
        self.binary = binary
        self.state_dir = state_dir
        self.address = ""
        self._proc: subprocess.Popen | None = None
        self._log = state_dir.parent / f"node-{uuid.uuid4().hex[:8]}.log"

    def start(self) -> None:
        log = open(self._log, "w")
        self._proc = subprocess.Popen(
            [str(self.binary), "--grpc-port", "0", "--metrics-port", "0",
             "--state-dir", str(self.state_dir)],
            stdout=log, stderr=subprocess.STDOUT,
        )
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            m = re.search(r"listening \(grpc=(\d+)", self._log.read_text())
            if m:
                self.address = f"127.0.0.1:{m.group(1)}"
                return
            if self._proc.poll() is not None:
                break
            time.sleep(0.05)
        self.stop()
        raise RuntimeError(f"kvstore-node did not start:\n{self._log.read_text()}")

    def stop(self) -> None:
        if self._proc and self._proc.poll() is None:
            self._proc.terminate()
            try:
                self._proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self._proc.kill()
                self._proc.wait()
        self._proc = None

    def log(self) -> str:
        return self._log.read_text()

    def restart(self) -> None:
        self.stop()
        self.start()


def _make_node(tmp: pathlib.Path) -> Node:
    binary = _node_binary()
    if binary is None:
        pytest.skip("kvstore-node binary not built (set KVSTORE_NODE_BIN)")
    node = Node(binary, tmp / "state")
    node.start()
    return node


@pytest.fixture(scope="session")
def node():
    with tempfile.TemporaryDirectory() as tmp:
        n = _make_node(pathlib.Path(tmp))
        yield n
        n.stop()


@pytest.fixture
def fresh_node(tmp_path):
    """A node of its own, for tests that restart it."""
    n = _make_node(tmp_path)
    yield n
    n.stop()


@pytest.fixture
def tenant() -> str:
    """A tenant per test, so tests sharing one node never see each other."""
    return f"t-{uuid.uuid4().hex[:12]}"


@pytest.fixture
def client(node, tenant):
    from agent_state_storage import Client

    with Client(node.address, tenant) as c:
        yield c


@pytest.fixture
async def aclient(node, tenant):
    from agent_state_storage import AsyncClient

    c = AsyncClient(node.address, tenant)
    yield c
    await c.close()
