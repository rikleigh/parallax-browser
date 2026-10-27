#!/usr/bin/env bash
# Send an ELF payload to elfldr and stream its stdout back. The output is
# also saved to logs/<payload>-<timestamp>.log for the feasibility report.
#   tools/send.sh path/to/payload.elf
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/env.sh"

elf="${1:?usage: send.sh payload.elf}"
[[ -f "$elf" ]] || { echo "no such file: $elf" >&2; exit 1; }
command -v socat >/dev/null || { echo "socat missing: brew install socat" >&2; exit 1; }

mkdir -p "$ROOT/logs"
log="$ROOT/logs/$(basename "$elf" .elf)-$(date +%Y%m%d-%H%M%S).log"
echo "==> $elf -> $PS5_HOST:$PS5_PORT (log: $log)" >&2
# -t keeps the connection open after the upload: elfldr wires the payload's
# stdout/stderr to it.
socat -t 9999999 - "TCP:$PS5_HOST:$PS5_PORT" < "$elf" | tee "$log"
