#!/usr/bin/env bash
# JIT on/off A/B in the payload harness (ps5wk-shot): each URL is loaded
# with PS5WK_JIT=0 and with the JIT, ROUNDS times each, alternating, and a
# table is printed: load finished, first paint (the page's own
# first-contentful-paint), DOMContentLoaded/load events, WebProcess CPU over
# the run, JIT code size, video frames dropped (YouTube) and crashes.
#
#   port/webkit/test/jit-ab.sh [url...]       (default: the four sites below)
#   BLD=... CHILD_REMOTE=... ROUNDS=2 SETTLE=15 MODES="0 1" jit-ab.sh
#
# Take the console lock first. Uses run-shot.sh (uploads the child once).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
. "$ROOT/env.sh" >/dev/null
FTP="ftp://$PS5_HOST:${PS5_FTP_PORT:-2121}"
ROUNDS="${ROUNDS:-2}"
SETTLE="${SETTLE:-15}"
MODES="${MODES:-0 1}"
export SHOT_OUT="${SHOT_OUT:-$ROOT/toolchain/wpe-webkit/shots-jit/$(date +%Y%m%d-%H%M%S)}"
mkdir -p "$SHOT_OUT"
[ $# -eq 0 ] && set -- https://www.youtube.com/watch?v=aqz-KE-bpKQ https://www.bbc.co.uk/news \
    https://www.reddit.com/ https://en.wikipedia.org/wiki/PlayStation_5

curl -s --ftp-create-dirs -T "$HERE/jit-perf.js" "$FTP/data/ps5webkit/shot/jit-perf.js"
first=1
for url in "$@"; do
    for round in $(seq "$ROUNDS"); do
        for mode in $MODES; do
            extra=""
            case "$url" in *youtube.com/watch*) extra="fakevdec=1" ;; esac
            name="$(echo "$url" | sed -E 's#https?://##; s#[^A-Za-z0-9]+#_#g' | cut -c1-40)-jit$mode-r$round"
            echo "== $name"
            SKIP_UPLOAD=$([ $first = 1 ] && echo "" || echo 1) \
            SHOT_ARGS="settle=$SETTLE js=/data/ps5webkit/shot/jit-perf.js $extra childenv=PS5WK_JIT=$mode;PS5WK_CPU_LOG=all;PS5WK_MEM_LOG=10" \
                "$HERE/run-shot.sh" "$url" 90 > "$SHOT_OUT/$name.out" 2>&1 || true
            first=0
            log="$(ls -t "$SHOT_OUT"/2*.log | head -1)"
            mv "$log" "$SHOT_OUT/$name.log"
            grep -E "^RESULT ps5wk-shot (load|webprocess)|crash " "$SHOT_OUT/$name.log" | cut -c1-160 | sed 's/^/   /' || true
        done
    done
done
python3 -I "$HERE/jit-ab-summary.py" "$SHOT_OUT"
