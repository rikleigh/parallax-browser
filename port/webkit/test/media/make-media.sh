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
#   sync-2160p30-hevc.mp4   3840x2160 30 fps HEVC Main (x265 with B-frames), AAC, 30 s  (hardware decoding, step 2b)
#   sync-2160p30-hevc10.mp4 3840x2160 30 fps HEVC Main10, AAC, 30 s
#   sync-3840p30-hevc.mp4   7680x3840 30 fps HEVC Main (the 2:1 size of 8K VR video), AAC, 30 s
#   switch-h264.mp4         H.264 avc3 (parameter sets in band, as BBC News's DASH streams)
#                           changing size mid-stream: 396x704, 540x960, 1280x720, 396x704,
#                           5 s each, AAC with the beeps, 20 s; switch-h264.m2ts the same in MPEG-TS
#   preview-*.mp4           hover previews for preview.html: 380x222/440x248 H.264 High with
#                           B-frames, no audio, 6 s, with and without the VUI reorder limits
#   feed-*                  for feed.html (several players at once): 480x854 and 854x480 H.264
#                           Main 3.1 with B-frames + AAC and 480x854 VP9 + Opus, 30 s; the
#                           480x854 clip as fragmented MP4, video and audio apart (MSE)
#   sound-test.m4a          audio only (no video decoder involved): a 440 Hz tone with a
#                           1760 Hz beep at the start of every second, AAC-LC stereo, 60 s
#   hls-ts/master.m3u8      HLS (as a VR site serves): 640x360 and 1280x720 H.264 + AAC
#                           variants, MPEG-TS segments of 2 s, 40 s, low variant listed first
#                           (hlsdemux starts there and switches up: a size change mid-stream)
#   hls-fmp4/master.m3u8    the same with HEVC (hvc1) 960x540 and 1920x1080, fMP4 segments
#                           (EXT-X-MAP)
#   hls-vr/master.m3u8      H.264 2160x1080 30 fps level 5.0 and two 3840x1920 60 fps level 5.2
#                           variants with different SPS/PPS, TS segments of 4 s; mixed.m3u8
#                           switches between them at fixed segments; refs16.m3u8 asks for more
#                           reference frames than level 5.2 allows at 3840x1920
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
# HEVC for the title's hardware decoder (Videodec2): hvc1 in MP4, as VR sites serve it.
HEVC="-c:v hevc_videotoolbox -tag:v hvc1 -g 60 -b:v"
make sync-2160p30-hevc.mp4   3840x2160 30 30 "-c:v libx265 -preset superfast -tag:v hvc1 -x265-params keyint=60:bframes=3:log-level=error -b:v 20M -pix_fmt yuv420p" "$AAC" # x265: B-frames (display order != decode order)
make sync-2160p30-hevc10.mp4 3840x2160 30 30 "$HEVC 20M -profile:v main10 -pix_fmt p010le" "$AAC"
make sync-3840p30-hevc.mp4   7680x3840 30 30 "$HEVC 40M -pix_fmt yuv420p" "$AAC"

# Resolution switches inside one stream (as adaptive players make them):
# H.264 segments of different sizes with their SPS/PPS in band, in one avc3
# MP4. h264parse sees each new SPS and renegotiates the decoder mid-stream.
# No B-frames: raw H.264 carries no timestamps, so pts = dts here.
if [ ! -s "$OUT/switch-h264.mp4" ]; then
    tmp="$(mktemp -d)"
    for size in 396x704 540x960 1280x720 396x704; do
        $FF -f lavfi -i "testsrc2=size=${size}:rate=30" -vf "drawtext=fontfile=$FONT:fontsize=$(( ${size#*x} / 10 )):fontcolor=white:box=1:boxcolor=black@0.6:x=(w-tw)/2:y=(h-th)/2:text='${size} %{pts\:hms}'" \
            -t 5 -c:v libx264 -profile:v high -preset veryfast -pix_fmt yuv420p -g 30 -bf 0 -b:v 1M -x264-params repeat-headers=1 -f h264 - >> "$tmp/all.h264"
    done
    $FF -r 30 -f h264 -i "$tmp/all.h264" -f lavfi -i "sine=frequency=440:beep_factor=4:sample_rate=48000" -map 0:v -map 1:a \
        -c:v copy -tag:v avc3 -af "volume=2.8,aformat=channel_layouts=stereo" -c:a aac -b:a 96k -t 20 -movflags +faststart "$OUT/switch-h264.mp4"
    # The same in MPEG-TS: no container size, so the caps follow each SPS
    # (the decoder sees a new format, as with MSE init segments).
    $FF -i "$OUT/switch-h264.mp4" -c copy -bsf:v h264_mp4toannexb -f mpegts "$OUT/switch-h264.m2ts"
    rm -rf "$tmp"
    echo "  switch-h264.mp4, switch-h264.m2ts"
fi

# Hover previews (test/media/preview.html): small H.264 High clips without
# audio, as listing pages play them on hover. -novui: the SPS without the VUI
# bitstream_restriction (strip-vui-restriction.py), as some encoders write
# them; a decoder must then hold the level's whole DPB (16 frames at
# 380x222 level 2.2 or 440x248 level 3.0, 7 at 380x222 level 2.0) before the
# first picture. Videodec2 never output a picture for such VR site previews
# (2026-10-30) while the proxy stopped at 16 frames in flight.
preview() { # <name> <WxH> <level> [novui]
    local name=$1 size=$2 level=$3 tmp
    [ -s "$OUT/$name" ] && { echo "  $name (exists)"; return; }
    tmp="$(mktemp -d)"
    $FF -f lavfi -i "testsrc2=size=$size:rate=30" -vf "drawtext=fontfile=$FONT:fontsize=$(( ${size#*x} / 5 )):fontcolor=white:box=1:boxcolor=black@0.6:x=(w-tw)/2:y=(h-th)/2:text='%{eif\:t\:d}.%{eif\:mod(n\,30)\:d\:2}'" \
        -t 6 -an -c:v libx264 -profile:v high -level "$level" -preset veryfast -pix_fmt yuv420p -g 60 -bf 3 -b:v 400k "$tmp/a.mp4"
    if [ "${4:-}" = novui ]; then
        python3 -I "$HERE/strip-vui-restriction.py" "$tmp/a.mp4" "$OUT/$name"
    else
        mv "$tmp/a.mp4" "$OUT/$name"
    fi
    rm -rf "$tmp"
    echo "  $name ($(du -h "$OUT/$name" | cut -f1))"
}
preview preview-380x222-l22.mp4       380x222 2.2
preview preview-380x222-l22-novui.mp4 380x222 2.2 novui
preview preview-440x248-l30-novui.mp4 440x248 3.0 novui
preview preview-380x222-l20-novui.mp4 380x222 2.0 novui

# Audio only, for "is there sound at all" checks on the TV (sine is 1/8 of
# full scale: peak about 0.35).
if [ ! -s "$OUT/sound-test.m4a" ]; then
    $FF -f lavfi -i "sine=frequency=440:beep_factor=4:sample_rate=48000" -af "volume=2.8,aformat=channel_layouts=stereo" \
        -t 60 -c:a aac -b:a 128k -movflags +faststart "$OUT/sound-test.m4a"
    echo "  sound-test.m4a"
fi

# HLS: two variants of the same clip with keyframes every 2 s at the same
# frames (no scene cuts), so hlsdemux can switch at any segment. Each
# variant draws its name. Serve the directory over HTTP (tools/serve-media.py)
# or play it from file:// (video.html's HLS tiles).
hls() { # <dir> <segment type> <vcodec args> <WxH low> <bitrate low> <WxH high> <bitrate high>
    local dir=$1 type=$2 vargs=$3 lo=$4 lob=$5 hi=$6 hib=$7 fs
    [ -s "$OUT/$dir/master.m3u8" ] && { echo "  $dir (exists)"; return; }
    rm -rf "$OUT/$dir"; mkdir -p "$OUT/$dir"
    label() { # <WxH> <name> -> drawtext chain for one variant
        fs=$(( ${1#*x} / 6 ))
        echo "scale=${1/x/:},drawtext=fontfile=$FONT:fontsize=$fs:fontcolor=white:box=1:boxcolor=black@0.6:x=(w-tw)/2:y=(h-th)/2:text='%{eif\:t\:d}.%{eif\:mod(n\,30)\:d\:2}',drawbox=x=0:y=0:w=iw/8:h=ih/8:color=white:t=fill:enable='lt(mod(t\,1)\,0.1)',drawtext=fontfile=$FONT:fontsize=$((fs/3)):fontcolor=yellow:x=20:y=h-th-20:text='$2 $1'"
    }
    ( cd "$OUT/$dir" && $FF -f lavfi -i "testsrc2=size=1920x1080:rate=30" -f lavfi -i "sine=frequency=440:beep_factor=4:sample_rate=48000" \
        -filter_complex "[0:v]split=2[a][b];[a]$(label "$lo" "$dir low")[v0];[b]$(label "$hi" "$dir high")[v1];[1:a]volume=0.25,aformat=channel_layouts=stereo,asplit=2[a0][a1]" \
        -map "[v0]" -map "[a0]" -map "[v1]" -map "[a1]" -t 40 \
        $vargs -g 60 -keyint_min 60 -sc_threshold 0 -b:v:0 "$lob" -b:v:1 "$hib" -c:a aac -b:a 128k \
        -f hls -hls_time 2 -hls_playlist_type vod -hls_segment_type "$type" -hls_flags independent_segments \
        -master_pl_name master.m3u8 -var_stream_map "v:0,a:0,name:low v:1,a:1,name:high" \
        -hls_segment_filename "%v/seg%03d.$([ "$type" = fmp4 ] && echo m4s || echo ts)" "%v/index.m3u8" )
    echo "  $dir ($(du -sh "$OUT/$dir" | cut -f1))"
}
hls hls-ts   mpegts "-c:v libx264 -profile:v high -preset veryfast -pix_fmt yuv420p" 640x360 800k 1280x720 3M
hls hls-fmp4 fmp4   "-c:v libx265 -preset superfast -tag:v hvc1 -pix_fmt yuv420p -x265-params log-level=error:open-gop=0" 960x540 1M 1920x1080 5M

# HLS shaped like a VR site's adaptive H.264 (the 2026-10-30 freeze: the
# picture stopped for good at 3840x1920 while the audio went on, Videodec2
# failing every AU with 0x811d0304): a 2160x1080 30 fps Main level 5.0
# variant and two 3840x1920 60 fps Main level 5.2 variants of the same size
# with different encoder settings (references, B-frames, bitrate: different
# SPS/PPS), MPEG-TS segments of 4 s cut at the same times (a keyframe every
# 2 s in every variant). master.m3u8 lists the low variant first (hlsdemux2
# starts there and switches up by bandwidth); mixed.m3u8 is one media
# playlist whose segments alternate between the three variants the way an
# adaptive player's switches arrive (no discontinuity tags), so every switch
# (2160 -> 3840 A -> 3840 B -> 3840 A -> 2160 ...) happens on every run, also
# from file://. refs16.m3u8: 3840x1920 level 5.2 asking for 16 reference
# frames, more than level 5.2's DPB holds at that size (6): not conforming,
# for a decoder sized from the level. The variants draw their names.
hls_vr() {
    local dir=hls-vr secs=48 v
    [ -s "$OUT/$dir/mixed.m3u8" ] && { echo "  $dir (exists)"; return; }
    rm -rf "$OUT/$dir"; mkdir -p "$OUT/$dir"
    vr_variant() { # <name> <WxH> <fps> <level> <x264 args> <bitrate>
        local name=$1 size=$2 fps=$3 level=$4 xargs=$5 rate=$6 fs
        fs=$(( ${size#*x} / 8 ))
        mkdir -p "$OUT/$dir/$name"
        ( cd "$OUT/$dir/$name" && $FF -f lavfi -i "testsrc2=size=$size:rate=$fps" -f lavfi -i "sine=frequency=440:beep_factor=4:sample_rate=48000" \
            -filter_complex "[0:v]drawtext=fontfile=$FONT:fontsize=$fs:fontcolor=white:box=1:boxcolor=black@0.6:x=(w-tw)/2:y=(h-th)/2:text='%{eif\:t\:d}.%{eif\:mod(n\,$fps)\:d\:2}',drawbox=x=0:y=0:w=iw/8:h=ih/8:color=white:t=fill:enable='lt(mod(t\,1)\,0.1)',drawtext=fontfile=$FONT:fontsize=$((fs/2)):fontcolor=yellow:x=40:y=h-th-40:text='$name $size $fps fps level $level'[v];[1:a]volume=0.25,aformat=channel_layouts=stereo[a]" \
            -map "[v]" -map "[a]" -t $secs \
            -c:v libx264 -profile:v main -level:v "$level" -preset veryfast -pix_fmt yuv420p -x264-params "$xargs" \
            -b:v "$rate" -maxrate "$rate" -bufsize "$rate" -force_key_frames "expr:gte(t,n_forced*2)" -sc_threshold 0 \
            -c:a aac -b:a 128k \
            -f hls -hls_time 4 -hls_playlist_type vod -hls_segment_type mpegts -hls_flags independent_segments \
            -hls_segment_filename "seg%03d.ts" index.m3u8 )
    }
    vr_variant low  2160x1080 30 5.0 "ref=3:bframes=3"                      6M
    vr_variant a    3840x1920 60 5.2 "ref=3:bframes=3:b-pyramid=normal"     16M
    vr_variant b    3840x1920 60 5.2 "ref=5:bframes=1:b-pyramid=none:weightp=0:8x8dct=0" 12M
    vr_variant refs16 3840x1920 60 5.2 "ref=16:bframes=3"                   16M
    {   echo "#EXTM3U"; echo "#EXT-X-VERSION:3"; echo "#EXT-X-INDEPENDENT-SEGMENTS"
        echo '#EXT-X-STREAM-INF:BANDWIDTH=6500000,RESOLUTION=2160x1080,FRAME-RATE=30.000,CODECS="avc1.4d4032,mp4a.40.2"'; echo "low/index.m3u8"
        echo '#EXT-X-STREAM-INF:BANDWIDTH=12500000,RESOLUTION=3840x1920,FRAME-RATE=60.000,CODECS="avc1.4d4034,mp4a.40.2"'; echo "b/index.m3u8"
        echo '#EXT-X-STREAM-INF:BANDWIDTH=16500000,RESOLUTION=3840x1920,FRAME-RATE=60.000,CODECS="avc1.4d4034,mp4a.40.2"'; echo "a/index.m3u8"
    } > "$OUT/$dir/master.m3u8"
    # mixed.m3u8: segment i from the variant in the pattern (the segment
    # lines of one variant playlist, with the variant's directory).
    local pattern=(low low a a b b a low b a b a) i=0 line
    {   echo "#EXTM3U"; echo "#EXT-X-VERSION:3"; echo "#EXT-X-TARGETDURATION:4"; echo "#EXT-X-MEDIA-SEQUENCE:0"
        echo "#EXT-X-PLAYLIST-TYPE:VOD"; echo "#EXT-X-INDEPENDENT-SEGMENTS"
        while read -r line; do
            case "$line" in
                "#EXTINF:"*) echo "$line" ;;
                seg*.ts) v=${pattern[$((i % ${#pattern[@]}))]}; echo "$v/$line"; i=$((i + 1)) ;;
            esac
        done < "$OUT/$dir/low/index.m3u8"
        echo "#EXT-X-ENDLIST"; } > "$OUT/$dir/mixed.m3u8"
    sed 's|^seg|refs16/seg|' "$OUT/$dir/refs16/index.m3u8" > "$OUT/$dir/refs16.m3u8"
    echo "  $dir ($(du -sh "$OUT/$dir" | cut -f1)): master.m3u8, mixed.m3u8 (${pattern[*]}), refs16.m3u8"
}
hls_vr

# A feed of small videos (test/media/feed.html: several players open at
# once, autoplaying, paused, seeking, scrolled and removed, as social feeds
# run them): 480x854 and 854x480 H.264 Main level 3.1 with B-frames (VUI
# reorder 2) and AAC, a keyframe every 2 s, 30 s, progressive MP4; a VP9 +
# Opus WebM; and the 480x854 clip as fragmented MP4, video and audio apart,
# for the page's MSE players (two SourceBuffers, as DASH players use them).
feed() { # <name> <WxH>
    local name=$1 size=$2 fs
    [ -s "$OUT/$name" ] && { echo "  $name (exists)"; return; }
    fs=$(( ${size%x*} / 5 ))
    $FF -f lavfi -i "testsrc2=size=$size:rate=30" -f lavfi -i "sine=frequency=440:beep_factor=4:sample_rate=48000" \
        -filter_complex "[0:v]drawtext=fontfile=$FONT:fontsize=$fs:fontcolor=white:box=1:boxcolor=black@0.6:x=(w-tw)/2:y=(h-th)/2:text='%{eif\:t\:d}.%{eif\:mod(n\,30)\:d\:2}',drawbox=x=0:y=0:w=iw/8:h=ih/8:color=white:t=fill:enable='lt(mod(t\,1)\,0.1)',drawtext=fontfile=$FONT:fontsize=$((fs/3)):fontcolor=yellow:x=10:y=h-th-10:text='$name'[v];[1:a]volume=0.25,aformat=channel_layouts=stereo[a]" \
        -map "[v]" -map "[a]" -t 30 \
        $( [ "${name##*.}" = webm ] && echo "-c:v libvpx-vp9 -deadline realtime -cpu-used 8 -g 60 -b:v 800k -c:a libopus -b:a 64k" \
            || echo "-c:v libx264 -profile:v main -level:v 3.1 -preset veryfast -pix_fmt yuv420p -bf 2 -g 60 -keyint_min 60 -sc_threshold 0 -b:v 1M -c:a aac -b:a 96k -movflags +faststart" ) \
        "$OUT/$name"
    echo "  $name ($(du -h "$OUT/$name" | cut -f1))"
}
feed feed-480x854-h264.mp4 480x854
feed feed-854x480-h264.mp4 854x480
feed feed-480x854-vp9.webm 480x854
if [ ! -s "$OUT/feed-mse-480x854-video.mp4" ]; then
    $FF -i "$OUT/feed-480x854-h264.mp4" -an -c copy -movflags frag_keyframe+empty_moov+default_base_moof "$OUT/feed-mse-480x854-video.mp4"
    $FF -i "$OUT/feed-480x854-h264.mp4" -vn -c copy -frag_duration 2000000 -movflags empty_moov+default_base_moof "$OUT/feed-mse-480x854-audio.mp4"
    echo "  feed-mse-480x854-video.mp4, feed-mse-480x854-audio.mp4"
fi

if [ "${1:-}" = --upload ]; then
    FTP="ftp://$PS5_HOST:${PS5_FTP_PORT:-2121}"
    for f in "$OUT"/*.mp4 "$OUT"/*.webm "$OUT"/*.m4a "$OUT"/*.m2ts; do
        echo "  upload $(basename "$f")"
        curl -s --ftp-create-dirs -T "$f" "$FTP/data/ps5webkit/media/$(basename "$f")"
    done
    for f in "$OUT"/hls-*/*.m3u8 "$OUT"/hls-*/*/*; do
        r="${f#$OUT/}"; echo "  upload $r"
        curl -s --ftp-create-dirs -T "$f" "$FTP/data/ps5webkit/media/$r"
    done
    curl -s --ftp-create-dirs -T "$HERE/video.html" "$FTP/data/ps5webkit/video.html"
    curl -s --ftp-create-dirs -T "$HERE/preview.html" "$FTP/data/ps5webkit/preview.html"
    curl -s --ftp-create-dirs -T "$HERE/feed.html" "$FTP/data/ps5webkit/feed.html"
    curl -s --ftp-create-dirs -T "$ROOT/port/webkit/runtime/start.html" "$FTP/data/ps5webkit/start.html"
    curl -s --ftp-create-dirs -T "$ROOT/port/webkit/runtime/parallax-logo.png" "$FTP/data/ps5webkit/parallax-logo.png"
    echo "uploaded to /data/ps5webkit/media/, video.html, start.html (and its logo)"
fi
