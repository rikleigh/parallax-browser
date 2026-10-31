#!/usr/bin/env bash
# Profiles page loads (and optionally scrolling) in the payload harness:
# ps5wk-shot at 1920x1080 with PS5WK_PROFILE in its children's env
# (apps/common/profiler.c), page.js for the load milestones, [render] lines
# per update; then fetches the .prof files of both children and prints
# prof.py's summary per run.
#
#   port/webkit/test/prof/prof-run.sh <url>...
#   TEMPS="cold warm" SETTLE=12 SCROLL=1 DIAG=1 PROFILE=1 EXTRA_ENV="PS5WK_JIT=0" prof-run.sh <url>
#
# cold = a new, empty cache directory; warm = the same cache again. PROFILE:
# 1 (CPU time, every thread) or wall (main thread). Take the console lock first.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../../.." && pwd)"
. "$ROOT/env.sh" >/dev/null
FTP="ftp://$PS5_HOST:${PS5_FTP_PORT:-2121}"
TEMPS="${TEMPS:-cold warm}"
SETTLE="${SETTLE:-10}"
PROFILE="${PROFILE:-1}"
export CHILD_REMOTE="${CHILD_REMOTE:-/data/ps5webkit/prof/ps5wk-child.elf}"
export SHOT_OUT="${SHOT_OUT:-$ROOT/toolchain/wpe-webkit/shots-prof/$(date +%Y%m%d-%H%M%S)}"
mkdir -p "$SHOT_OUT"
js="$SHOT_OUT/page.js"
: > "$js"
[[ -n ${YTCONSENT:-} ]] && cat "$HERE/yt-consent.js" >> "$js"
cat "$HERE/${ONLOAD:-page.js}" >> "$js"
if [[ -n ${DIAG:-} ]]; then
    printf '\n;%s\n' "$(cat "$HERE/page-diag.js")" >> "$js"
fi
if [[ -n ${SCROLL:-} ]]; then
    # scroll.js starts 0 s after the load; give the page 2 s first.
    printf '\n;setTimeout(function () {\n%s\n}, 2000);\n' "$(cat "$ROOT/port/webkit/test/paint/scroll.js")" >> "$js"
fi
curl -s --ftp-create-dirs -T "$js" "$FTP/data/ps5webkit/shot/prof-page.js"
# The child once, then a pause: let the console settle after a big transfer.
. "$PS5_PAYLOAD_SDK/toolchain/prospero.sh"
mkdir -p "$SHOT_OUT/stripped"
prospero-strip -o "$SHOT_OUT/stripped/ps5wk-child.elf" "$ROOT/toolchain/wpe-webkit/build/bin/ps5wk-child"
if [[ -z ${SKIP_UPLOAD:-} ]]; then
    curl -s --ftp-create-dirs -T "$SHOT_OUT/stripped/ps5wk-child.elf" "$FTP$CHILD_REMOTE"
    sleep 5
fi
# Wait (up to 60 s) until elfldr has no payload running but the ps probe
# itself, then 12 s more: a shot sent within a few seconds of the previous
# payload (even this probe) found elfldr refusing the connections it makes to
# launch the children (ECONNREFUSED on 127.0.0.1:9021); 14 s apart worked.
waitidle() {
    local i out
    for i in $(seq 12); do
        out="$({ cat "$ROOT/port/webkit/test/ps/ps.elf"; echo; } | socat -t 10 - "TCP:$PS5_HOST:$PS5_PORT" 2>/dev/null || true)"
        local elfldr; elfldr="$(echo "$out" | awk '$NF=="accept" && /elfldr/ {print $2}')"
        local busy; busy="$(echo "$out" | awk -v e="$elfldr" '$1=="pid" && $4==e' | wc -l | tr -d ' ')"
        if [[ -n $elfldr && $busy -le 1 ]]; then
            sleep 12 # a payload sent within seconds of the last one found elfldr refusing its child launches
            return 0
        fi
        sleep 5
    done
    echo "   (elfldr still has payloads after 60 s)"
}
listprof() { curl -s "$FTP/data/ps5webkit/prof/" | awk '{print $NF}' | grep '\.prof$' || true; }
for url in "$@"; do
    cache="/data/ps5webkit/prof/cache-$RANDOM"
    for temp in $TEMPS; do
        name="$(echo "$url" | sed -E 's#https?://##; s#[^A-Za-z0-9]+#_#g' | cut -c1-48)-$temp${TAG:+-$TAG}"
        echo "== $name"
        waitidle
        before="$(listprof)"
        SKIP_UPLOAD=1 \
        SHOT_ARGS="width=1920 height=1080 settle=$SETTLE cache=$cache onload=/data/ps5webkit/shot/prof-page.js childenv=PS5WK_PROFILE=$PROFILE;PS5WK_RENDER_LOG=2;PS5WK_CPU_LOG=all${EXTRA_ENV:+;$EXTRA_ENV}" \
            "$ROOT/port/webkit/test/run-shot.sh" "$url" 120 > "$SHOT_OUT/$name.out" 2>&1 || true
        sleep 3 # the payloads wind down
        log="$(ls -t "$SHOT_OUT"/2*.log | head -1)"
        mv "$log" "$SHOT_OUT/$name.log"
        mkdir -p "$SHOT_OUT/$name"
        for f in $(comm -13 <(echo "$before" | sort) <(listprof | sort)); do
            curl -s "$FTP/data/ps5webkit/prof/$f" -o "$SHOT_OUT/$name/$f"
            curl -s "$FTP/" -Q "DELE /data/ps5webkit/prof/$f" >/dev/null 2>&1 || true
        done
        grep -E "^RESULT ps5wk-shot load|^load (started|committed|finished|redirected)|crash |ONLOAD-JS|\[profiler\]|CONSOLE.*(DIAG|PAGE ttfb|YTCONSENT)" "$SHOT_OUT/$name.log" | cut -c1-260 | sed 's/^/   /' || true
        ls "$SHOT_OUT/$name" | sed 's/^/   prof: /'
    done
    curl -s "$FTP/" -Q "RMD $cache" >/dev/null 2>&1 || true
done
echo "out: $SHOT_OUT (python3 $HERE/prof.py $SHOT_OUT/<run>/*.prof)"
