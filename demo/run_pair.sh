#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

make demo

PORT_T=18515
PORT_I=18516
ITERS="${ITERS:-2000}"

./demo/rdma_demo target --ip 0.0.0.0 --port "$PORT_T" \
  --peer-ip 127.0.0.1 --peer-port "$PORT_I" --duration 25 &
TPID=$!
disown "$TPID" 2>/dev/null || true
trap 'kill "$TPID" 2>/dev/null || true' EXIT
sleep 0.3

./demo/rdma_demo initiator --ip 0.0.0.0 --port "$PORT_I" \
  --peer-ip 127.0.0.1 --peer-port "$PORT_T" --iters "$ITERS"

kill "$TPID" 2>/dev/null || true
sleep 0.2
wait "$TPID" 2>/dev/null || true
trap - EXIT
