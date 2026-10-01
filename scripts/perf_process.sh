#!/usr/bin/env bash
set -euo pipefail

# Measure CPU/cycles charged directly to the receiver process.
# Start this on the receiver, then launch the sender on the other machine.
#
# Usage:
#   ./scripts/perf_process.sh recv --cpu 4
#   ./scripts/perf_process.sh uring --queue-depth 256 --cpu 4

if [[ $# -lt 1 ]]; then
    echo "usage: $0 recv|uring|uring-oneshot [receiver options...]" >&2
    exit 2
fi

backend=$1
shift
bin=${UDP_BENCH_BIN:-./build/udp_bench}

exec perf stat \
    -e task-clock \
    -e cycles:u \
    -e cycles:k \
    -e instructions:u \
    -e instructions:k \
    -e context-switches \
    -e cpu-migrations \
    -- "$bin" receiver --backend "$backend" --once --quiet "$@"
