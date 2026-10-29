#!/usr/bin/env bash
# CPU painting A/B in the payload harness (ps5wk-shot, 1920x1080): each URL
# is loaded once per mode, ROUNDS times, alternating; 2 s after the load the
# page scrolls down and up for 10 s (scroll.js). The WebProcess logs every
# rendering update (PS5WK_RENDER_LOG=2) and paint-ab-summary.py prints, per
# site and mode, the updates during load and during the scroll: count,
# updates/s, total/update/paint ms (avg, p50, p95, max).
#
#   port/webkit/test/paint/paint-ab.sh [url...]      (default: the sites below)
#   ROUNDS=1 MODES="base t1 t4" paint-ab.sh
#   FRAMES=1 ...: also fetch each run's whole last frame (<name>.ppm) to compare pixels
#
# Modes (child env): base = before (1 thread, full-frame readback, scrolls
# repaint), tN = N paint threads with scroll copy (t1: the readback and scroll
# fixes only), tNns = N threads, scrolls repaint, rec1 = record and replay
# on one thread (the recording's cost), aN = record, then replay on N pool
# threads while the main thread goes on; anything else is taken as a
# literal "K=V;K=V" child env.
# Take the console lock first. Uses ../run-shot.sh (uploads the child once).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../../.." && pwd)"
. "$ROOT/env.sh" >/dev/null
FTP="ftp://$PS5_HOST:${PS5_FTP_PORT:-2121}"
ROUNDS="${ROUNDS:-1}"
MODES="${MODES:-base t4ns t1 t4}"
SETTLE="${SETTLE:-15}"
export CHILD_REMOTE="${CHILD_REMOTE:-/data/ps5webkit/paint/ps5wk-child.elf}"
export SHOT_OUT="${SHOT_OUT:-$ROOT/toolchain/wpe-webkit/shots-paint/$(date +%Y%m%d-%H%M%S)}"
mkdir -p "$SHOT_OUT"
[ $# -eq 0 ] && set -- https://www.youtube.com/ https://www.youtube.com/watch?v=aqz-KE-bpKQ \
    https://www.bbc.co.uk/news https://old.reddit.com/ https://www.reddit.com/ \
    https://en.wikipedia.org/wiki/PlayStation_5 https://github.com/WebKit/WebKit https://news.ycombinator.com/

curl -s --ftp-create-dirs -T "$HERE/scroll.js" "$FTP/data/ps5webkit/shot/paint-scroll.js"
# Local, unchanging page (file:///data/ps5webkit/shot/paint-static.html, ?plain without the
# sticky header): the final frame's fnv64 must be the same in every mode.
curl -s --ftp-create-dirs -T "$HERE/static.html" "$FTP/data/ps5webkit/shot/paint-static.html"
envfor() {
    case "$1" in
        base) echo "PS5WK_PAINT_THREADS=1;PS5WK_SHM_READBACK=1;PS5WK_SCROLL_COPY=0" ;;
        t[0-9]*ns) n=${1#t}; echo "PS5WK_PAINT_THREADS=${n%ns};PS5WK_SCROLL_COPY=0" ;;
        rec1) echo "PS5WK_PAINT_THREADS=1;PS5WK_PAINT_RECORD=1" ;;
        a[0-9]*) echo "PS5WK_PAINT_THREADS=${1#a};PS5WK_PAINT_ASYNC=1" ;;
        t[0-9]*) echo "PS5WK_PAINT_THREADS=${1#t}" ;;
        *) echo "$1" ;;
    esac
}
first=1
for url in "$@"; do
    for round in $(seq "$ROUNDS"); do
        for mode in $MODES; do
            extra=""
            case "$url" in *youtube.com/watch*) extra="fakevdec=1" ;; esac
            name="$(echo "$url" | sed -E 's#https?://##; s#[^A-Za-z0-9]+#_#g' | cut -c1-56)-$(echo "$mode" | tr -c 'A-Za-z0-9\n' _)-r$round"
            echo "== $name"
            if [ -n "${FRAMES:-}" ]; then extra="$extra frame=/data/ps5webkit/shot/paint-frame.ppm"; fi
            SKIP_UPLOAD=$([ $first = 1 ] && echo "" || echo 1) \
            SHOT_ARGS="width=1920 height=1080 settle=$SETTLE onload=/data/ps5webkit/shot/paint-scroll.js $extra childenv=PS5WK_RENDER_LOG=2;PS5WK_CPU_LOG=all;$(envfor "$mode")" \
                "$ROOT/port/webkit/test/run-shot.sh" "$url" 90 > "$SHOT_OUT/$name.out" 2>&1 || true
            first=0
            log="$(ls -t "$SHOT_OUT"/2*.log | head -1)"
            mv "$log" "$SHOT_OUT/$name.log"
            if [ -n "${FRAMES:-}" ]; then curl -s "$FTP/data/ps5webkit/shot/paint-frame.ppm" -o "$SHOT_OUT/$name.ppm"; fi
            grep -E "^RESULT ps5wk-shot (load|webprocess)|crash |\[render\] " "$SHOT_OUT/$name.log" | cut -c1-200 | sed 's/^/   /' || true
        done
    done
done
python3 -I "$HERE/paint-ab-summary.py" "$SHOT_OUT"
