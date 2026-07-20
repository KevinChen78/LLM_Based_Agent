#!/bin/bash
# Quick start for Phase 0

set -e

cd "$(dirname "$0")/.."

# Start LLM Gateway in background
python3 llm_gateway/main.py &
GATEWAY_PID=$!
trap "kill $GATEWAY_PID 2>/dev/null || true" EXIT

# Wait for gateway
sleep 2

# Run API server
./build/bin/api_server
