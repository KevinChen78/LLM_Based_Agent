#!/bin/bash
set -e

cd "$(dirname "$0")/.."

mkdir -p build
cd build

echo "[Phase 0] Configuring..."
cmake .. -DCMAKE_BUILD_TYPE=Release

echo "[Phase 0] Building..."
cmake --build . --config Release -j$(nproc)

echo "[Phase 0] Running tests..."
ctest -C Release --output-on-failure

echo "[Phase 0] Done. Run ./bin/api_server"
