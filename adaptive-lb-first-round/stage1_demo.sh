#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT_DIR"

if [[ ! -x ./server || ! -x ./client ]]; then
    echo "Build the programs first with: make" >&2
    exit 1
fi

RUN_ID="$(date +%Y%m%d-%H%M%S)"
OUTPUT_DIR="$ROOT_DIR/results/stage1-$RUN_ID"
mkdir -p "$OUTPUT_DIR"

server_pids=()
cleanup() {
    for pid in "${server_pids[@]:-}"; do
        kill "$pid" 2>/dev/null || true
    done
    for pid in "${server_pids[@]:-}"; do
        wait "$pid" 2>/dev/null || true
    done
}
trap cleanup EXIT INT TERM

echo "Stage 1 demo output: $OUTPUT_DIR"
for server_id in 1 2 3; do
    port=$((5000 + server_id))
    ./server "$port" "$server_id" 100 >"$OUTPUT_DIR/server-$server_id.log" 2>&1 &
    server_pids+=("$!")
done

for port in 5001 5002 5003; do
    ready=0
    for _ in {1..50}; do
        if (exec 3<>"/dev/tcp/127.0.0.1/$port") 2>/dev/null; then
            ready=1
            break
        fi
        sleep 0.1
    done
    if [[ "$ready" -ne 1 ]]; then
        echo "Server on port $port did not become ready. See $OUTPUT_DIR." >&2
        exit 1
    fi
done

for server_id in 1 2 3; do
    port=$((5000 + server_id))
    echo "Running 20 concurrent clients against server $server_id (port $port)..."
    ./client 127.0.0.1 "$port" 20 5 | tee "$OUTPUT_DIR/client-server-$server_id.log"
    if ! grep -q 'succeeded=100 failed=0' "$OUTPUT_DIR/client-server-$server_id.log"; then
        echo "Requests failed against server $server_id; see $OUTPUT_DIR." >&2
        exit 1
    fi
done

echo "Simulating server 3 failure..."
kill "${server_pids[2]}"
wait "${server_pids[2]}" 2>/dev/null || true
server_pids[2]=""
if ./client 127.0.0.1 5003 1 1 >"$OUTPUT_DIR/failed-server-check.log" 2>&1; then
    echo "Expected the connection to failed server 3 to fail, but it succeeded." >&2
    exit 1
fi
cat "$OUTPUT_DIR/failed-server-check.log"
if ! grep -q 'succeeded=0 failed=1' "$OUTPUT_DIR/failed-server-check.log"; then
    echo "The expected failure was not recorded; see $OUTPUT_DIR." >&2
    exit 1
fi

echo "Stage 1 demo passed. Logs are in: $OUTPUT_DIR"
