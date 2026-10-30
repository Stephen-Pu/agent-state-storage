#!/usr/bin/env bash
# Regenerate the Python gRPC stubs for agentstate.v1 into the SDK package.
# Use grpcio-tools==1.71.0 (pip install grpcio-tools==1.71.0): its stubs need
# only protobuf>=5.29 and grpcio>=1.71 at runtime, the floors in
# src/sdk/python/pyproject.toml. Newer tools raise both floors.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/src/sdk/python/agent_state_storage/_proto"
PY="${PYTHON:-python3}"
mkdir -p "$OUT"
"$PY" -m grpc_tools.protoc \
    --proto_path="$ROOT/src/core/proto" \
    --python_out="$OUT" --pyi_out="$OUT" --grpc_python_out="$OUT" \
    "$ROOT/src/core/proto/agentstate.proto"
# protoc emits an absolute import; make it package-relative.
sed -i.bak 's/^import agentstate_pb2 as/from . import agentstate_pb2 as/' "$OUT/agentstate_pb2_grpc.py"
rm -f "$OUT/agentstate_pb2_grpc.py.bak"
touch "$OUT/__init__.py"
