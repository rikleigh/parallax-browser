#!/usr/bin/env bash
# Test media for Milestone 2 (video), generated on the Mac with FFmpeg, so
# nothing third-party is committed: test patterns with a running clock and a
# beep plus a white flash once per second (A/V sync by eye and ear).
#
#   port/webkit/test/media/make-media.sh            # -> toolchain/wpe-webkit/media/
#   port/webkit/test/media/make-media.sh --upload   # + FTP to /data/ps5webkit/media/ with video.html
#
# Files (all progressive MP4 with +faststart, or WebM):
#   sync-720p30-h264.mp4    1280x720 30 fps H.264 High, AAC-LC 48 kHz stereo, 60 s
#   sync-1080p30-h264.mp4   1920x1080 30 fps H.264 High, AAC-LC, 60 s
#   sync-1080p60-h264.mp4   1920x1080 60 fps H.264 High, AAC-LC, 30 s
#   sync-1080p30-vp9.webm   1920x1080 30 fps VP9, Opus, 30 s
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../../.." && pwd)"
. "$ROOT/env.sh" >/dev/null
OUT="$ROOT/toolchain/wpe-webkit/media"
mkdir -p "$OUT"
FF="ffmpeg -hide_banner -loglevel error -y"
FONT=/System/Library/Fonts/Supplemental/Arial.ttf
[ -f "$FONT" ] || FONT=/System/Library/Fonts/Helvetica.ttc

# Video: testsrc2 + a big clock (s.frame) + a white box during the first
# 100 ms of every second. Audio: 440 Hz with a 1760 Hz beep at the start of
# every second (sine's beep_factor), so the flash and the beep coincide.
make() { # <name> <WxH> <fps> <seconds> <vcodec args> <acodec args>
    local name=$1 size=$2 fps=$3 secs=$4 vargs=$5 aargs=$6
    [ -s "$OUT/$name" ] && { echo "  $name (exists)"; return; }
    local fs=$(( ${size#*x} / 6 ))
    local vf="drawtext=fontfile=$FONT:fontsize=$fs:fontcolor=white:box=1:boxcolor=black@0.6:x=(w-tw)/2:y=(h-th)/2:text='%{eif\:t\:d}.%{eif\:mod(n\,$fps)\:d\:2}',drawbox=x=0:y=0:w=iw/8:h=ih/8:color=white:t=fill:enable='lt(mod(t\,1)\,0.1)',drawtext=fontfile=$FONT:fontsize=$((fs/3)):fontcolor=yellow:x=20:y=h-th-20:text='$name'"
    $FF -f lavfi -i "testsrc2=size=$size:rate=$fps" -f lavfi -i "sine=frequency=440:beep_factor=4:sample_rate=48000" \
        -filter_complex "[0:v]$vf[v];[1:a]volume=0.25,aformat=channel_layouts=stereo[a]" -map "[v]" -map "[a]" \
        -t "$secs" $vargs $aargs "$OUT/$name"
    echo "  $name ($(du -h "$OUT/$name" | cut -f1))"
}
echo "==> $OUT"
H264="-c:v libx264 -profile:v high -preset veryfast -pix_fmt yuv420p -g 60 -b:v"
AAC="-c:a aac -b:a 128k -movflags +faststart"
make sync-720p30-h264.mp4  1280x720  30 60 "$H264 3M"  "$AAC"
make sync-1080p30-h264.mp4 1920x1080 30 60 "$H264 6M"  "$AAC"
make sync-1080p60-h264.mp4 1920x1080 60 30 "$H264 9M"  "$AAC"
make sync-1080p30-vp9.webm 1920x1080 30 30 "-c:v libvpx-vp9 -deadline realtime -cpu-used 8 -row-mt 1 -g 60 -b:v 4M" "-c:a libopus -b:a 128k"

if [ "${1:-}" = --upload ]; then
    FTP="ftp://$PS5_HOST:${PS5_FTP_PORT:-2121}"
    for f in "$OUT"/*.mp4 "$OUT"/*.webm; do
        echo "  upload $(basename "$f")"
        curl -s --ftp-create-dirs -T "$f" "$FTP/data/ps5webkit/media/$(basename "$f")"
    done
    curl -s --ftp-create-dirs -T "$HERE/video.html" "$FTP/data/ps5webkit/video.html"
    curl -s --ftp-create-dirs -T "$ROOT/port/webkit/runtime/start.html" "$FTP/data/ps5webkit/start.html"
    echo "uploaded to /data/ps5webkit/media/, video.html, start.html"
fi
