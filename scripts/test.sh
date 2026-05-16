#!/usr/bin/env bash
# Run the full test suite — C++ ctest + Go tests + Python E2E.
# Each stage is independent; a failure in one stage does not skip the others.
set -uo pipefail

cd "$(dirname "$0")/.."

BUILD_DIR="${BUILD_DIR:-build}"
fail=0

echo "==[ C++ ]=="
if ! ./scripts/build.sh; then
    echo "  C++ stage FAILED"
    fail=1
fi

echo
echo "==[ Go control-plane ]=="
( cd src/control-plane && go test -short -count=1 ./... ) || fail=1

echo
echo "==[ Go vet operator + kvctl ]=="
( cd src/operator      && go vet ./... ) || fail=1
( cd src/kvctl         && go vet ./... ) || fail=1

echo
echo "==[ Python E2E adapter ]=="
if command -v pytest >/dev/null 2>&1; then
    KVCACHE_LIB="$PWD/${BUILD_DIR}/core-abi/libkvcache.so" \
        pytest src/adapters/vllm/tests -v || fail=1
else
    echo "  pytest not installed; skipping (pip install cffi pytest)"
fi

echo
if [ "$fail" -eq 0 ]; then
    echo "==> ALL STAGES PASSED"
else
    echo "==> ONE OR MORE STAGES FAILED"
fi
exit $fail
