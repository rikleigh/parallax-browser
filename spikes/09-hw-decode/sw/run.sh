#!/usr/bin/env bash
# Upload the clips for the software-decode payload, write swjobs.txt, send it.
#   spikes/09-hw-decode/sw/run.sh [threads]
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
. "$ROOT/env.sh" >/dev/null
T=${1:-16}
FTP="ftp://$PS5_HOST:${PS5_FTP_PORT:-2121}"
REMOTE=/data/ps5webkit/hwdec
listing=$(curl -s --max-time 10 "$FTP$REMOTE/" || true)
tmp=$(mktemp)
for f in vrclip_4320x2160.mp4 hevc_4k.mp4 h264_4k.mp4 vp9_4k.mp4 vp9_57k.mp4 av1_4k.mp4 vrclip_7680x3840.mp4 av1_8k.mp4; do
    size=$(stat -f %z "$HERE/../media/$f")
    grep -qE " $size [A-Z][a-z]{2} .* $f\$" <<<"$listing" || curl -sS -T "$HERE/../media/$f" "$FTP$REMOTE/$f"
    echo "$REMOTE/$f $T 20" >> "$tmp"
done
curl -sS -T "$tmp" "$FTP$REMOTE/swjobs.txt"; rm -f "$tmp"
"$ROOT/tools/send.sh" "$HERE/sw_decode.elf"
