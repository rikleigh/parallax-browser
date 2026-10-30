#!/usr/bin/env bash
# Page-load networking in the payload harness (ps5wk-shot): each URL is
# loaded cold (a new, empty cache directory) and then warm (the same cache
# again), per mode; timing.js prints Navigation/Resource Timing, and
# net-summary.py sums DNS, connect, TLS, TTFB, download and cache hits per
# load (and the load/DOMContentLoaded times).
#
#   port/webkit/test/net/net-ab.sh [url...]
#   MODES="new old" ROUNDS=2 net-ab.sh
#
# Modes: new = this build's child; old = OLD_CHILD (a previous build,
# uploaded to /data/ps5webkit/net/old-child.elf; default: the 51cbcf3
# snapshot); nolock / nomalloc = this child with /data/ps5webkit/no-fastlock
# or no-pasmalloc present for that load (removed afterwards).
# Take the console lock first. Uses ../run-shot.sh.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../../.." && pwd)"
. "$ROOT/env.sh" >/dev/null
FTP="ftp://$PS5_HOST:${PS5_FTP_PORT:-2121}"
ROUNDS="${ROUNDS:-1}"
MODES="${MODES:-old new}"
SETTLE="${SETTLE:-8}"
OLD_CHILD="${OLD_CHILD:-}"
export SHOT_OUT="${SHOT_OUT:-$ROOT/toolchain/wpe-webkit/shots-net/$(date +%Y%m%d-%H%M%S)}"
mkdir -p "$SHOT_OUT"
[ $# -eq 0 ] && set -- https://en.wikipedia.org/wiki/PlayStation_5 https://www.bbc.co.uk/news \
    https://github.com/WebKit/WebKit https://news.ycombinator.com/ https://www.youtube.com/

curl -s --ftp-create-dirs -T "$HERE/timing.js" "$FTP/data/ps5webkit/shot/net-timing.js"
if [[ " $MODES " == *" old "* ]]; then
    [[ -n $OLD_CHILD ]] || { echo "OLD_CHILD=<unstripped or stripped old ps5wk-child> needed for mode old" >&2; exit 2; }
    "$PS5_PAYLOAD_SDK/bin/prospero-strip" -o "$SHOT_OUT/old-child.elf" "$OLD_CHILD"
    curl -s --ftp-create-dirs -T "$SHOT_OUT/old-child.elf" "$FTP/data/ps5webkit/net/old-child.elf"
fi
flag() { # flag <name> on|off
    if [[ $2 == on ]]; then printf '' | curl -s -T - "$FTP/data/ps5webkit/$1"; else curl -s "$FTP/" -Q "DELE /data/ps5webkit/$1" >/dev/null 2>&1 || true; fi
}
first=1
for round in $(seq "$ROUNDS"); do
    for url in "$@"; do
        for mode in $MODES; do
            child=/data/ps5webkit/net/new-child.elf skip=$([ $first = 1 ] && echo "" || echo 1)
            case "$mode" in
                old) child=/data/ps5webkit/net/old-child.elf; skip=1 ;;
                nolock) flag no-fastlock on ;;
                nomalloc) flag no-pasmalloc on ;;
            esac
            [[ $mode != old ]] && first=0
            cache="/data/ps5webkit/net/cache-$mode-$round-$RANDOM"
            for temp in cold warm; do
                name="$(echo "$url" | sed -E 's#https?://##; s#[^A-Za-z0-9]+#_#g' | cut -c1-48)-$mode-$temp-r$round"
                echo "== $name"
                CHILD_REMOTE="$child" SKIP_UPLOAD="$skip" \
                SHOT_ARGS="width=1920 height=1080 settle=$SETTLE cache=$cache onload=/data/ps5webkit/shot/net-timing.js" \
                    "$ROOT/port/webkit/test/run-shot.sh" "$url" 90 > "$SHOT_OUT/$name.out" 2>&1 || true
                skip=1
                log="$(ls -t "$SHOT_OUT"/2*.log | head -1)"
                mv "$log" "$SHOT_OUT/$name.log"
                grep -E "^RESULT ps5wk-shot load|crash |NAV " "$SHOT_OUT/$name.log" | cut -c1-200 | sed 's/^/   /' || true
            done
            case "$mode" in nolock) flag no-fastlock off ;; nomalloc) flag no-pasmalloc off ;; esac
        done
    done
done
python3 -I "$HERE/net-summary.py" "$SHOT_OUT"
