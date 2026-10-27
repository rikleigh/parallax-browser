#!/usr/bin/env bash
# Make Spike 9's test clips on the Mac (Homebrew ffmpeg 8 with libx264/libx265).
#
# Each clip is side-by-side 3D-shaped: two testsrc2 halves (the right one
# hue-shifted) with the frame number burned into each eye and temporal noise
# added, so the encoder spends tube-site bitrates (40-80 Mbit/s) rather than
# the few Mbit/s a clean test pattern compresses to. Decoder throughput
# depends on bitrate (entropy decoding), so clean clips would flatter it.
#
# Output (gitignored): media/<name>.{h264,h265} raw Annex-B elementary
# streams (VP9: .ivf), and media/<name>.mp4 fragmented MP4 (CMAF-style: empty moov +
# one moof/mdat per GOP, what MSE's SourceBuffer is fed).
#
#   spikes/09-hw-decode/make-clips.sh [name...]     default: all
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$HERE/media"; mkdir -p "$OUT"
FF=/opt/homebrew/bin/ffmpeg
FONT="/System/Library/Fonts/Supplemental/Arial Bold.ttf"
FRAMES=${FRAMES:-240}

# name codec width height fps bitrate pix_fmt
CLIPS=(
  "h264_1080p   h264 1920 1080 30 12M  yuv420p"
  "h264_4k      h264 3840 2160 30 50M  yuv420p"
  "hevc_4k      hevc 3840 2160 30 40M  yuv420p"
  "hevc_6k      hevc 5760 2880 30 60M  yuv420p"
  "hevc_8k      hevc 7680 3840 30 80M  yuv420p"
  "hevc10_8k    hevc 7680 3840 30 80M  yuv420p10le"
  "hevc_8192    hevc 8192 4096 30 80M  yuv420p"
  "vp9_4k       vp9  3840 2160 30 30M  yuv420p"
  "vp9_57k      vp9  5760 2880 30 40M  yuv420p"
  "vp9p2_4k     vp9  3840 2160 30 30M  yuv420p10le"
)

make_clip() {
    local name=$1 codec=$2 w=$3 h=$4 fps=$5 br=$6 pix=$7
    local eye=$((w / 2)) fs=$((h / 6))
    local text="drawtext=fontfile='$FONT':text='%{n}':fontsize=$fs:fontcolor=white:box=1:boxcolor=black@0.6:x=(w-tw)/2:y=(h-th)/2"
    local src="testsrc2=s=${eye}x${h}:r=$fps:d=$((FRAMES / fps + 1))"
    local fg="[0:v]split[a][b];[a]$text,noise=alls=12:allf=t[l];[b]hue=h=120,$text,noise=alls=12:allf=t[r];[l][r]hstack,format=$pix[v]"
    local enc ext params
    if [[ $codec == vp9 ]]; then
        # VP9 has no Annex-B: the elementary stream is IVF (32-byte header,
        # then 12-byte frame headers), and superframes stay as muxed.
        ext=ivf
        [[ $pix == yuv420p10le ]] && prof=2 || prof=0
        enc=(-c:v libvpx-vp9 -profile:v $prof -deadline realtime -cpu-used 8 -row-mt 1 -threads 14
             -g 60 -keyint_min 60 -tile-columns 4 -frame-parallel 0 -auto-alt-ref 1 -lag-in-frames 16)
    elif [[ $codec == h264 ]]; then
        ext=h264
        enc=(-c:v libx264 -preset faster -profile:v high -x264-params "keyint=60:min-keyint=60:aud=1:bframes=3")
    else
        ext=h265
        params="keyint=60:min-keyint=60:aud=1:bframes=4:open-gop=0:repeat-headers=1"
        [[ $pix == yuv420p10le ]] && prof=main10 || prof=main
        enc=(-c:v libx265 -preset superfast -profile:v $prof -x265-params "$params:log-level=error")
    fi
    local rc=(-b:v "$br" -maxrate "$br" -bufsize "$br")
    echo "==> $name: $codec ${w}x${h} $pix $br, $FRAMES frames"
    local t0=$SECONDS
    # Encode once into fragmented MP4 (proper timestamps; one fragment per
    # GOP), then copy the access units out as Annex-B with the mp4toannexb
    # filter, which puts the parameter sets in front of every keyframe.
    local tag=() bsf=h264_mp4toannexb fmt=h264
    case $codec in
        hevc) tag=(-tag:v hvc1); bsf=hevc_mp4toannexb; fmt=hevc ;;
        vp9) bsf=; fmt=ivf ;;
    esac
    "$FF" -hide_banner -loglevel error -y -f lavfi -i "$src" -filter_complex "$fg" -map "[v]" \
        -frames:v "$FRAMES" "${enc[@]}" "${rc[@]}" ${tag[@]+"${tag[@]}"} \
        -movflags +frag_keyframe+empty_moov+default_base_moof+cmaf "$OUT/$name.mp4"
    "$FF" -hide_banner -loglevel error -y -i "$OUT/$name.mp4" -c copy ${bsf:+-bsf:v $bsf} \
        -f $fmt "$OUT/$name.$ext"
    echo "    $(du -h "$OUT/$name.$ext" | cut -f1) es, $(du -h "$OUT/$name.mp4" | cut -f1) fmp4, $((SECONDS - t0)) s"
}

want=("$@")
for c in "${CLIPS[@]}"; do
    read -r name rest <<<"$c"
    if [[ ${#want[@]} -eq 0 || " ${want[*]} " == *" $name "* ]]; then
        # shellcheck disable=SC2086
        make_clip $name $rest
    fi
done
