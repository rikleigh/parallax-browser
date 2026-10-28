#!/usr/bin/env bash
# Send the smoke payload to elfldr and print its output; RESULT lines are
# also saved to port/deps/smoke/results/<timestamp>.log.
#   port/deps/smoke/run.sh [elf]
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
. "$ROOT/env.sh"
ELF="${1:-$ROOT/toolchain/wpe-deps/build/smoke/smoke.elf}"
mkdir -p "$HERE/results"
LOG="$HERE/results/$(date +%Y%m%d-%H%M%S).log"
socat -t 300 -T 300 - "TCP:$PS5_HOST:$PS5_PORT" < "$ELF" | tee "$LOG"
