#!/usr/bin/env bash
# Test clips for "View in VR", generated on the Mac with FFmpeg (nothing
# third-party, nothing committed): a labelled equirectangular grid per eye
# (a cell every 30 degrees with its yaw/pitch, FRONT / RIGHT / BACK / LEFT /
# UP / DOWN, the front cell green), projected with FFmpeg's v360 into each
# format the browser can show, with a running clock at the front of each
# eye. Stereo clips have different eyes: a red "LEFT EYE" grid and a blue
# "RIGHT EYE" grid, so a swapped or wrong eye mapping is obvious. The file
# names carry the keywords the projection guess reads.
#
#   port/webkit/test/media/make-vr-media.sh            # -> toolchain/wpe-webkit/media/vr/
#   port/webkit/test/media/make-vr-media.sh --upload   # + FTP to /data/ps5webkit/media/
#
#   vrtest_360_mono.mp4   3840x1920  360 mono
#   vrtest_360_TB.mp4     3840x3840  360 top/bottom (left eye on top)
#   vrtest_360_SBS.mp4    3840x1920  360 side by side (left eye left)
#   vrtest_180_mono.mp4   1920x1920  180 mono (half equirectangular)
#   vrtest_180_LR.mp4     3840x1920  180 side by side
#   vrtest_180_TB.mp4     2048x4096  180 top/bottom (2048 per eye, a size VR sites use; portrait,
#                                    which Videodec2 opens only as a square decoder)
#   vrtest_MKX200_LR.mp4  3840x1920  200 degree equidistant fisheye per eye, side by side
# All HEVC (hvc1, VideoToolbox), 30 fps, 60 s, with a quiet AAC tone.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../../.." && pwd)"
. "$ROOT/env.sh" >/dev/null
OUT="$ROOT/toolchain/wpe-webkit/media/vr"
mkdir -p "$OUT"
FF="ffmpeg -hide_banner -loglevel error -y"
FONT=/System/Library/Fonts/Supplemental/Arial.ttf
[ -f "$FONT" ] || FONT=/System/Library/Fonts/Helvetica.ttc
SECS=60

# One eye's 360 grid, 3840x1920: <out.png> <background> <eye label>
master() {
    local out=$1 bg=$2 label=$3 W=3840 H=1920
    [ -s "$out" ] && return
    local f="drawgrid=x=160:y=160:w=320:h=320:t=3:c=white@0.85"
    f+=",drawbox=x=1760:y=800:w=320:h=320:t=fill:c=0x1e7a32"            # front cell
    local lon lat x y
    for lat in 60 30 0 -30 -60; do
        y=$(( 960 - lat * 32 / 3 ))
        for lon in -150 -120 -90 -60 -30 0 30 60 90 120 150; do
            x=$(( 1920 + lon * 32 / 3 ))
            f+=",drawtext=fontfile=$FONT:fontsize=30:fontcolor=white@0.9:x=$x-tw/2:y=$y+70:text='Y$lon P$lat'"
        done
        f+=",drawtext=fontfile=$FONT:fontsize=30:fontcolor=white@0.9:x=-tw/2:y=$y+70:text='Y180 P$lat'"
        f+=",drawtext=fontfile=$FONT:fontsize=30:fontcolor=white@0.9:x=$W-tw/2:y=$y+70:text='Y180 P$lat'"
    done
    big() { f+=",drawtext=fontfile=$FONT:fontsize=$1:fontcolor=$2:borderw=4:bordercolor=black:x=$3-tw/2:y=$4-th/2:text='$5'"; }
    big 96 white 1920 960 FRONT
    big 96 white $(( 1920 + 960 )) 960 'RIGHT 90'
    big 96 white $(( 1920 - 960 )) 960 'LEFT 90'
    big 96 white 0 960 BACK
    big 96 white $W 960 BACK
    for x in 480 1440 2400 3360; do big 64 white $x 70 UP; big 64 white $x 1850 DOWN; done
    big 80 yellow 1920 640 "$label"
    big 80 yellow 1920 1280 "$label"
    big 60 yellow $(( 1920 + 960 )) 640 "$label"
    big 60 yellow $(( 1920 - 960 )) 640 "$label"
    big 60 yellow 0 640 "$label"
    big 60 yellow $W 640 "$label"
    $FF -f lavfi -i "color=c=$bg:s=${W}x$H:d=1" -vf "$f" -frames:v 1 "$out"
}

master "$OUT/eye-mono.png" 0x26323f "MONO"
master "$OUT/eye-left.png" 0x4a1c1c "LEFT EYE"
master "$OUT/eye-right.png" 0x1c2a5a "RIGHT EYE"

# The clock at the front (centre) of one eye's picture, after projection.
clock() { echo "drawtext=fontfile=$FONT:fontsize=$1:fontcolor=yellow:box=1:boxcolor=black@0.7:x=(w-tw)/2:y=h/2+h/9:text='%{pts\\:hms}'"; }

# <name> <filter_complex using [0:v] (mono/left) and [1:v] (right), ending in [v]>
clip() {
    local name=$1 graph=$2
    [ -s "$OUT/$name" ] && { echo "  $name (exists)"; return; }
    $FF -loop 1 -framerate 30 -t $SECS -i "$3" -loop 1 -framerate 30 -t $SECS -i "$4" \
        -f lavfi -t $SECS -i "sine=frequency=440:beep_factor=4:sample_rate=48000" \
        -filter_complex "$graph;[2:a]volume=0.15,aformat=channel_layouts=stereo[a]" -map "[v]" -map "[a]" \
        -c:v hevc_videotoolbox -tag:v hvc1 -b:v 8M -g 60 -pix_fmt yuv420p -c:a aac -b:a 96k -movflags +faststart -t $SECS "$OUT/$name"
    echo "  $name ($(ffprobe -v error -select_streams v:0 -show_entries stream=width,height -of csv=p=0 "$OUT/$name"), $(du -h "$OUT/$name" | cut -f1))"
}

M="$OUT/eye-mono.png" L="$OUT/eye-left.png" R="$OUT/eye-right.png"
HE="v360=input=e:output=he:w=1920:h=1920"
HE2K="v360=input=e:output=he:w=2048:h=2048"
FE="v360=input=e:output=fisheye:h_fov=200:v_fov=200:w=1920:h=1920"
echo "==> $OUT"
clip vrtest_360_mono.mp4 "[0:v]$(clock 72),format=yuv420p[v]" "$M" "$M"
clip vrtest_360_TB.mp4 "[0:v]$(clock 72)[l];[1:v]$(clock 72)[r];[l][r]vstack,format=yuv420p[v]" "$L" "$R"
clip vrtest_360_SBS.mp4 "[0:v]scale=1920:1920,$(clock 40)[l];[1:v]scale=1920:1920,$(clock 40)[r];[l][r]hstack,format=yuv420p[v]" "$L" "$R"
clip vrtest_180_mono.mp4 "[0:v]$HE,$(clock 72),format=yuv420p[v]" "$M" "$M"
clip vrtest_180_LR.mp4 "[0:v]$HE,$(clock 72)[l];[1:v]$HE,$(clock 72)[r];[l][r]hstack,format=yuv420p[v]" "$L" "$R"
clip vrtest_180_TB.mp4 "[0:v]$HE2K,$(clock 72)[l];[1:v]$HE2K,$(clock 72)[r];[l][r]vstack,format=yuv420p[v]" "$L" "$R"
clip vrtest_MKX200_LR.mp4 "[0:v]$FE,$(clock 72)[l];[1:v]$FE,$(clock 72)[r];[l][r]hstack,format=yuv420p[v]" "$L" "$R"

if [ "${1:-}" = --upload ]; then
    FTP="ftp://$PS5_HOST:${PS5_FTP_PORT:-2121}"
    for f in "$OUT"/*.mp4; do
        echo "  upload $(basename "$f")"
        curl -s --ftp-create-dirs -T "$f" "$FTP/data/ps5webkit/media/$(basename "$f")"
    done
    curl -s --ftp-create-dirs -T "$ROOT/port/webkit/runtime/start.html" "$FTP/data/ps5webkit/start.html"
fi
