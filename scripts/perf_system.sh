#!/usr/bin/env bash
set -euo pipefail

# System-wide kernel-cycle measurement on the receiver machine.
# This can include NIC/NAPI/softirq work that is not charged to udp_bench.
# Keep the receiver machine otherwise idle when comparing runs.
#
# Usage:
#   ./scripts/perf_system.sh recv --cpu 4
#   ./scripts/perf_system.sh uring --queue-depth 256 --cpu 4

if [[ $# -lt 1 ]]; then
    echo "usage: $0 recv|uring|uring-oneshot [receiver options...]" >&2
    exit 2
fi

backend=$1
shift
bin=${UDP_BENCH_BIN:-./build/udp_bench}

exec sudo perf stat -a \
    -e cycles:k \
    -e instructions:k \
    -e context-switches \
    -e cpu-migrations \
    -- "$bin" receiver --backend "$backend" --once --quiet "$@"
