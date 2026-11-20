#!/usr/bin/env bash
# Phase 0 (HANDOVER-radv.md): the release browser's numbers before the move
# to RADV, and the same runs again on each later build for an A/B.
#
#   spikes/15-radv-only/phase0/run.sh [case ...]     (default: all cases)
#   OUT=<dir> run.sh ...    (default results/phase0-<date>-<time>/)
#   KEEP=1 run.sh ...       leave the pages on the console (no clean-up)
#   TITLE=PPSA99208 run.sh  another installed build of the browser (default
#                           PPSA99207, the release), e.g. the GPU=radv test
#
# Every page is local (no network, no site names in the logs). The pages and
# clips go to /data/ps5webkit/phase0/; /data/ps5webkit/log-sink points the
# title's log at tools/logserver.sh (must be running; the update check's
# lines are dropped: they name its server); the knob files
# (wc-scene-stats, pad-scroll-test, vr-sim, vr-selftest, test-urls.txt) are
# written per case. All of it is deleted at the end. Each case starts the
# browser (PPSA99207) afresh with its page as the only test URL
# (test-urls.txt: a test run, so the owner's saved session is neither
# restored nor written; a launch argument would add a tab to it), waits, and
# keeps its slice of the title log in $OUT/<case>.log. WebGPU pages are opened with tools/ps5open in
# the running browser (PS5_PIN in the environment if the console has a PIN).
# summary.py prints the table.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
. "$ROOT/env.sh" >/dev/null
FTP="ftp://$PS5_HOST:${PS5_FTP_PORT:-2121}"
D=/data/ps5webkit
P=$D/phase0
T="$ROOT/port/webkit/test"
MEDIA="$ROOT/toolchain/wpe-webkit/media"
OUT="${OUT:-$HERE/../results/phase0-$(date +%Y%m%d-%H%M%S)}"
mkdir -p "$OUT"
LOG="$(ls -t "$ROOT"/logs/title-*.log 2>/dev/null | head -1)"
[[ -n "$LOG" ]] || { echo "no logs/title-*.log: start tools/logserver.sh" >&2; exit 1; }
pgrep -f tools/logserver.sh >/dev/null || { echo "tools/logserver.sh is not running" >&2; exit 1; }
MAC="$(ipconfig getifaddr en0)"
TITLE="${TITLE:-PPSA99207}"
export PS5WK_OWN_TITLES="PPSA99207 $TITLE"

put() { curl -sS --ftp-create-dirs -T "$1" "$FTP$2"; }
putstr() { printf '%s\n' "$1" > "$OUT/.knob"; put "$OUT/.knob" "$2"; }
rmfile() { curl -sS -o /dev/null -Q "DELE $1" "$FTP/" 2>/dev/null || true; }
KNOBS=(wc-scene-stats pad-scroll-test vr-sim vr-selftest test-urls.txt)
clear_knobs() { for k in "${KNOBS[@]}"; do rmfile "$D/$k"; done; }

setup() {
    echo "==> pages and clips to $P"
    putstr "$MAC:${PS5WK_LOG_PORT:-9999}" "$D/log-sink"
    for f in paint/static.html paint/wcfeed.html paint/scrolljank.html media/video.html; do
        put "$T/$f" "$P/$(basename "$f")"
    done
    put "$T/webgl/readpixels.html" "$P/webgl-readpixels.html"
    put "$T/webgl/busy.html" "$P/webgl-busy.html"
    put "$T/webgl/video.html" "$P/webgl-video.html"
    for f in "$T"/webgpu/*.html "$T"/webgpu/webgpu-test.js; do put "$f" "$P/webgpu/$(basename "$f")"; done
    for f in feed-480x854-h264.mp4 feed-854x480-h264.mp4 feed-480x854-vp9.webm; do put "$MEDIA/$f" "$P/$f"; done
    for f in sync-1080p30-h264.mp4 sync-2160p30-hevc.mp4 sync-2160p30-hevc10.mp4; do put "$MEDIA/$f" "$P/media/$f"; done
}

cleanup() {
    [[ -n "${KEEP:-}" ]] && return
    set +e +o pipefail   # delete everything it can, whatever is missing
    echo "==> clean-up"
    clear_knobs
    rmfile "$D/log-sink"
    # the pad test's and the VR self-test's captures, and the VR simulation's projections file
    curl -s "$FTP$D/shots/" 2>/dev/null | awk '{print $NF}' | grep -E '^(pad|vr)-' | while read -r f; do rmfile "$D/shots/$f"; done
    rmfile "$D/vr-projections-sim.txt"
    for f in $(curl -s "$FTP$P/webgpu/" | awk '$NF!="." && $NF!=".." {print $NF}'); do rmfile "$P/webgpu/$f"; done
    for f in $(curl -s "$FTP$P/media/" | awk '$NF!="." && $NF!=".." {print $NF}'); do rmfile "$P/media/$f"; done
    for f in $(curl -s "$FTP$P/" | awk '$1 !~ /^d/ {print $NF}'); do rmfile "$P/$f"; done
    curl -sS -o /dev/null -Q "RMD $P/webgpu" -Q "RMD $P/media" -Q "RMD $P" "$FTP/" 2>/dev/null || true
    rm -f "$OUT/.knob"
}

# run_case <name> <url> <seconds> [done-pattern] [knob=content ...]
run_case() {
    local name="$1" url="$2" secs="$3" done="${4:-}"; shift 4
    echo "== $name ($url, ${secs} s)"
    clear_knobs
    local kv
    for kv in "$@"; do putstr "${kv#*=}" "$D/${kv%%=*}"; done
    putstr "$url 3600" "$D/test-urls.txt"
    local from=$(($(wc -l < "$LOG") + 1))
    "$ROOT/tools/launch.sh" "$TITLE" >/dev/null
    for _ in $(seq "$secs"); do
        sleep 1
        [[ -n "$done" ]] && tail -n +"$from" "$LOG" | grep -a -q -- "$done" && { sleep 2; break; }
    done
    tail -n +"$from" "$LOG" | grep -a -v '^\[update\]' | awk '!seen[$0]++' > "$OUT/$name.log"
    grep -a -E 'RESULT|\[crash|\[abort\]' "$OUT/$name.log" | cut -c1-180 | sed 's/^/   /' | head -12 || true
}

# open_page <name> <url> <seconds> <done-pattern>: in the running browser
open_page() {
    local name="$1" url="$2" secs="$3" done="$4"
    echo "== $name"
    local from=$(($(wc -l < "$LOG") + 1))
    "$ROOT/tools/ps5open" "$url" >/dev/null
    for _ in $(seq "$secs"); do
        sleep 1
        tail -n +"$from" "$LOG" | grep -a -q -- "$done" && { sleep 2; break; }
    done
    tail -n +"$from" "$LOG" | grep -a -v '^\[update\]' | awk '!seen[$0]++' > "$OUT/$name.log"
    grep -a -E 'RESULT .* summary|\[crash|\[abort\]|device lost' "$OUT/$name.log" | cut -c1-180 | sed 's/^/   /' | head -6 || true
}

PAD="delay=8 secs=4 ry=0.5 pause=2 shots=0"
WEBGPU=(adapter device compute offscreen-readback errors buffer-map triangle cube resize two-canvases
    todataurl copy-external-image video-texture webgl-and-webgpu)

case_static()  { run_case static "file://$P/static.html" 40 "" wc-scene-stats=1 "pad-scroll-test=$PAD"; }
case_feed()    { run_case feed "file://$P/wcfeed.html?mins=1" 80 "WCFEED DONE" wc-scene-stats=1; }
case_layers()  { run_case layers2800 "file://$P/scrolljank.html?layers=2800&cluster=2500" 40 "" wc-scene-stats=1 "pad-scroll-test=$PAD"; }
case_video()   {
    run_case video-1080p "file://$P/video.html?src=media/sync-1080p30-h264.mp4" 30 ""
    run_case video-2160p "file://$P/video.html?src=media/sync-2160p30-hevc.mp4" 30 ""
    run_case video-2160p10 "file://$P/video.html?src=media/sync-2160p30-hevc10.mp4" 30 ""
}
case_webgl()   {
    run_case webgl-readpixels "file://$P/webgl-readpixels.html" 60 "RESULT webgl-readpixels summary"
    run_case webgl-busy "file://$P/webgl-busy.html" 90 "RESULT webgl-busy n=2000"
    run_case webgl-video "file://$P/webgl-video.html?clip=$P/media/sync-1080p30-h264.mp4" 30 ""
    # WebGL 1, as many 360 and VR video players are (on GPU=radv its copy path differs: a WebGL 1
    # page's video aborted the title, 2026-11-20).
    run_case webgl1-video "file://$P/webgl-video.html?gl=1&clip=$P/media/sync-1080p30-h264.mp4" 30 ""
}
case_webgpu()  {
    run_case webgpu-start "file://$P/static.html" 15 ""
    local p
    for p in "${WEBGPU[@]}"; do
        open_page "webgpu-$p" "file://$P/webgpu/$p.html?run=$(date +%s)" 60 "RESULT webgpu-$p summary"
    done
}
case_vr()      {
    # View in VR needs a playing video on the page
    run_case vr-sim "file://$P/video.html?src=media/sync-1080p30-h264.mp4" 180 "RESULT ps5browser vr-selftest" vr-sim=1 vr-selftest=1
}

# 8K HEVC and HLS (not in the default set: a 254 MB upload, and the HLS streams come from
# tools/serve-media.py on this Mac, whose address the logs then hold). The HLS sets
# (test/media/make-media.sh) start on their low variant and switch up; hls-vr's mixed.m3u8
# switches between 2160x1080 and 3840x1920 60 fps every few segments.
case_media()   {
    put "$MEDIA/sync-3840p30-hevc.mp4" "$P/media/sync-3840p30-hevc.mp4"
    run_case video-8k "file://$P/video.html?src=media/sync-3840p30-hevc.mp4" 40 ""
    python3 -I "$ROOT/tools/serve-media.py" --port 8771 "$MEDIA" > "$OUT/.serve-media.log" 2>&1 &
    local server=$!
    sleep 1
    local hls="type=application/x-mpegURL" s
    for s in hls-ts/master hls-fmp4/master hls-vr/master hls-vr/mixed; do
        run_case "$(echo "$s" | tr / -)" "file://$P/video.html?src=http://$MAC:8771/$s.m3u8&$hls" 55 ""
    done
    kill "$server" 2>/dev/null || true
    rm -f "$OUT/.serve-media.log"
    # View in VR on the 8K clip (8K VR video: the decoder and the eye buffers at once)
    run_case vr-sim-8k "file://$P/video.html?src=media/sync-3840p30-hevc.mp4" 180 "RESULT ps5browser vr-selftest" vr-sim=1 vr-selftest=1
}

trap cleanup EXIT
setup
cases=("$@"); [[ ${#cases[@]} -gt 0 ]] || cases=(static feed layers video webgl webgpu vr)
for c in "${cases[@]}"; do "case_$c"; done
clear_knobs
# Leave the release browser on its home page, without the test knobs.
"$ROOT/tools/launch.sh" --stop >/dev/null 2>&1 || true
echo "==> logs in $OUT"
python3 -I "$HERE/summary.py" "$OUT" || true
