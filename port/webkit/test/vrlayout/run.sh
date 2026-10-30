#!/usr/bin/env bash
# View in VR's layout guess over test clips, on the Mac (no console):
# builds vrlayout-test with apps/title/vrlayout.c, has FFmpeg make the
# title's 128x128 thumbnails of three frames 200 ms apart (FFmpeg's area
# average, and the title shader's 8 x 8 taps per cell from whole frames),
# and checks each guess. Clips: test/media/make-vr-media.sh (guess_*,
# vrtest_*) and make-media.sh (flat test patterns, 2:1 HLS variants).
#
#   port/webkit/test/vrlayout/run.sh
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../../.." && pwd)"
MEDIA="$ROOT/toolchain/wpe-webkit/media"
BIN="$(mktemp -d)"
trap 'rm -rf "$BIN"' EXIT
cc -std=c11 -O2 -Wall -Wextra -I"$ROOT/port/webkit/apps/title" "$HERE/vrlayout-test.c" "$ROOT/port/webkit/apps/title/vrlayout.c" -lm -o "$BIN/vrlayout-test"

# Projections (WPEDisplayPS5.h): 0 flat, 1 360, 2 360 SBS, 3 360 TB, 4 180,
# 5 180 SBS, 6 180 TB, 7 fisheye SBS; "-": print only (the keywords decide
# those clips in the browser).
checks=(
    "vr/guess_a_3840x1920.mp4 5"
    "vr/guess_b_3840x1920.mp4 1"
    "vr/guess_c_2048x2048.mp4 3"
    "vr/guess_d_1440x2880.mp4 6"
    "vr/guess_e_3840x1920.mp4 7"
    "vr/guess_f_3840x1920.mp4 0"
    "vr/guess_g_1920x1080.mp4 0"
    "hls-vr/low/index.m3u8 0"
    "hls-vr/a/index.m3u8 0"
    "sync-1080p30-h264.mp4 0"
    "sync-3840p30-hevc.mp4 0"
    "vr/vrtest_180_LR.mp4 -"
    "vr/vrtest_360_mono.mp4 -"
    "vr/vrtest_360_TB.mp4 -"
    "vr/vrtest_180_TB.mp4 -"
    "vr/vrtest_MKX200_LR.mp4 -"
)
fail=0 n=0
for c in "${checks[@]}"; do
    read -r clip want <<<"$c"
    f="$MEDIA/$clip"
    [ -e "$f" ] || { echo "SKIP $clip (not made; run make-media.sh / make-vr-media.sh)"; continue; }
    size="$(ffprobe -v error -select_streams v:0 -show_entries stream=width,height -of csv=p=0 "$f")"
    w=${size%,*} h=${size#*,}
    # area: FFmpeg's exact cell averages at 2 s and 9 s; taps: the title's
    # shader sampling (8 x 8 bilinear taps per cell) at 5 s.
    for run in area:2 area:9 taps:5; do
        mode=${run%:*} start=${run#*:}
        if [ "$mode" = area ]; then
            out="$(ffmpeg -hide_banner -loglevel error -ss "$start" -i "$f" -vf "fps=5,scale=128:128:flags=area,format=rgb24" -frames:v 3 -f rawvideo - |
                "$BIN/vrlayout-test" "$w" "$h" "$want")" && ok=1 || ok=0
        else
            out="$(ffmpeg -hide_banner -loglevel error -ss "$start" -i "$f" -vf "fps=5,format=rgb24" -frames:v 3 -f rawvideo - |
                "$BIN/vrlayout-test" "$w" "$h" "$want" taps)" && ok=1 || ok=0
        fi
        n=$((n + 1))
        if [ "$ok" = 1 ]; then echo "PASS $clip $mode@${start}s: $out"; else echo "FAIL $clip $mode@${start}s (want $want): $out"; fail=$((fail + 1)); fi
    done
done
echo "RESULT vrlayout $([ $fail = 0 ] && echo PASS || echo FAIL): $((n - fail))/$n"
[ $fail = 0 ]
