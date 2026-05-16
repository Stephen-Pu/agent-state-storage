#!/usr/bin/env bash
# Convenience wrapper: configure + build + test the whole tree.
# Mirrors what CI runs. Safe to invoke from any directory.
set -euo pipefail

cd "$(dirname "$0")/.."

BUILD_DIR="${BUILD_DIR:-build}"
BUILD_TYPE="${BUILD_TYPE:-Debug}"
GENERATOR="${GENERATOR:-Ninja}"
JOBS="${JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu)}"

echo "==> Configuring (build=${BUILD_DIR}, type=${BUILD_TYPE}, gen=${GENERATOR})"
cmake -S src -B "${BUILD_DIR}" -G "${GENERATOR}" -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"

echo "==> Building (-j${JOBS})"
cmake --build "${BUILD_DIR}" -j "${JOBS}"

echo "==> Running ctest"
ctest --test-dir "${BUILD_DIR}" --output-on-failure --timeout 60

echo "==> Done. libkvcache.so at ${BUILD_DIR}/core-abi/libkvcache.so"
