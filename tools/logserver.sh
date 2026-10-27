#!/usr/bin/env bash
# Log sink for spikes that run as registered titles (no elfldr socket).
# Titles call spike_log_connect("<mac ip>", PS5WK_LOG_PORT). Each connection
# is appended to logs/title-<timestamp>.log and echoed here.
#   tools/logserver.sh            # listens on $PS5WK_LOG_PORT (9999)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/env.sh"
mkdir -p "$ROOT/logs"
log="$ROOT/logs/title-$(date +%Y%m%d-%H%M%S).log"
mac_ip="$(ipconfig getifaddr en0 2>/dev/null || true)"
echo "==> listening on ${mac_ip:-<this mac>}:$PS5WK_LOG_PORT (log: $log)" >&2
socat -u "TCP-LISTEN:$PS5WK_LOG_PORT,reuseaddr,fork" STDOUT | tee -a "$log"
