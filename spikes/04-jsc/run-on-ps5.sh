#!/usr/bin/env bash
# Send jsc.elf to the PS5 elfldr and run JS on it. Output comes back on the
# same socket (elfldr dup2()s the connection onto the payload's stdio).
#
# Usage:
#   spikes/04-jsc/run-on-ps5.sh [JS_FILE...]       # default: hello, correctness, bench
#   PS5_HOST=192.168.1.50 spikes/04-jsc/run-on-ps5.sh js/bench.js
#   JSC_OPTS="--useConcurrentGC=0" spikes/04-jsc/run-on-ps5.sh js/bench.js
#   JSC_VARIANT=wasm spikes/04-jsc/run-on-ps5.sh      # WASM=ON build, adds js/wasm.js
#   JSC_VARIANT=slim ...                               # slim ICU data (35 MB instead of 49 MB)
#
# Modes (JSC_MODE):
#   stdin (default) - one payload launch per JS file: `cat jsc.elf file.js` is
#                     streamed to elfldr; elfldr reads exactly the ELF (it sizes
#                     it from the ELF headers), the rest of the stream becomes
#                     the payload's stdin, and PS5Main.cpp runs it via `jsc -e`.
#                     Needs ps5-payload-dev/elfldr (reads by ELF header, no
#                     16 MiB cap). Old john-tornblom elfldr reads to EOF and
#                     caps payloads at 16 MiB: use mode "uri" with that one.
#   uri             - jsc.elf and the JS files must already be on the console
#                     (e.g. via an FTP payload) under $PS5_DIR (default
#                     /data/jsc). Sends "file://$PS5_DIR/jsc.elf?args=..." to
#                     elfldr, which passes real argv (newer elfldr only).
# Results are also appended to spikes/04-jsc/results/<timestamp>.log
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
# Values given on the command line win over env.sh / env.local.sh.
_host="${PS5_HOST:-}"; _port="${PS5_PORT:-}"
[ -f "$ROOT/env.sh" ] && . "$ROOT/env.sh"
PS5_HOST="${_host:-${PS5_HOST:-ps5}}"
PS5_PORT="${_port:-${PS5_PORT:-9021}}"
PS5_DIR="${PS5_DIR:-/data/jsc}"
JSC_MODE="${JSC_MODE:-stdin}"
JSC_OPTS="${JSC_OPTS:-}"
JSC_VARIANT="${JSC_VARIANT:-}"          # "", slim (smaller ICU data), wasm, wasm-slim
case "$JSC_VARIANT" in
    "") DEF_ELF="$HERE/build/jsc-ps5/bin/jsc.elf" ;;
    slim) DEF_ELF="$HERE/build/jsc-ps5/bin/jsc-slim.elf" ;;
    wasm) DEF_ELF="$HERE/build/jsc-ps5-wasm/bin/jsc.elf" ;;
    wasm-slim) DEF_ELF="$HERE/build/jsc-ps5-wasm/bin/jsc-slim.elf" ;;
    *) echo "unknown JSC_VARIANT=$JSC_VARIANT" >&2; exit 2 ;;
esac
ELF="${JSC_ELF:-$DEF_ELF}"
[ -f "$ELF" ] || { echo "missing $ELF (run build.sh first)" >&2; exit 1; }
TIMEOUT="${JSC_TIMEOUT:-600}"

if [ "$#" -eq 0 ]; then
    set -- "$HERE/js/hello.js" "$HERE/js/correctness.js" "$HERE/js/bench.js"
    case "$JSC_VARIANT" in wasm*) set -- "$@" "$HERE/js/wasm.js" ;; esac
fi

mkdir -p "$HERE/results"
LOG="$HERE/results/$(date +%Y%m%d-%H%M%S)-$PS5_HOST.log"

send() {
    # socat half-closes after stdin EOF and keeps reading until the peer closes.
    if command -v socat >/dev/null 2>&1; then
        socat -t "$TIMEOUT" -T "$TIMEOUT" - "TCP:$PS5_HOST:$PS5_PORT"
    else
        # BSD nc on macOS: -N shuts down the write side on EOF (if supported).
        nc -N "$PS5_HOST" "$PS5_PORT" 2>/dev/null || nc "$PS5_HOST" "$PS5_PORT"
    fi
}

echo "# jsc on $PS5_HOST:$PS5_PORT mode=$JSC_MODE elf=$ELF ($(wc -c <"$ELF") bytes)" | tee -a "$LOG"
for js in "$@"; do
    echo "# ===== $(basename "$js") =====" | tee -a "$LOG"
    start=$(date +%s)
    case "$JSC_MODE" in
        stdin)
            {
                cat "$ELF"
                # Optional first-line options understood by PS5Main.cpp.
                [ -n "$JSC_OPTS" ] && printf '//jsc: %s\n' "$JSC_OPTS"
                cat "$js"
            } | send 2>&1 | tee -a "$LOG"
            ;;
        uri)
            args="$JSC_OPTS $PS5_DIR/$(basename "$js")"
            args="${args# }"
            # elfldr splits args on spaces; URL-encode spaces as %20 is not
            # supported, it uses backslash escapes, which we do not need here.
            printf 'file://%s/jsc.elf?args=%s\n' "$PS5_DIR" "$args" | send 2>&1 | tee -a "$LOG"
            ;;
        *) echo "unknown JSC_MODE=$JSC_MODE" >&2; exit 2 ;;
    esac
    echo "# wall time incl. upload: $(( $(date +%s) - start ))s" | tee -a "$LOG"
done
echo "# log: $LOG"
