#!/usr/bin/env bash
# Build the release archive: the title, built and packaged with no log host
# (a development build sends its log to this Mac), and the runtime files the
# title reads from /data/ps5webkit (fonts, CA bundle, ICU data, XKB data, the
# start page), laid out as on the console so the archive's data/ folder
# copies straight onto /data.
#   tools/make-release.sh [version]     -> dist/parallax-browser-<version>.zip
# The version defaults to `git describe --tags --always --dirty`. The title's
# debug binaries (for crash addresses) stay in dist/<name>-debug/. The build
# directory's log host is put back afterwards, so development builds keep
# logging.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/env.sh" >/dev/null
BLD="$ROOT/toolchain/wpe-webkit/build"
RUNTIME="$ROOT/toolchain/wpe-sysroot/share/ps5webkit-runtime"
ICUDAT="$ROOT/toolchain/icu/src-76.1/icu/source/data/in/icudt76l.dat"
version="${1:-$(git -C "$ROOT" describe --tags --always --dirty)}"
name="parallax-browser-$version"
out="$ROOT/dist/$name"
stage="$ROOT/dist/$name-debug"

[[ -f $BLD/build.ninja ]] || { echo "missing $BLD: run port/webkit/build.sh" >&2; exit 1; }
[[ -f $RUNTIME/etc/ssl/cert.pem ]] || { echo "missing $RUNTIME: run port/deps/build.sh cacert fonts xkeyboard_config" >&2; exit 1; }
[[ -f $ICUDAT ]] || { echo "missing $ICUDAT: run spikes/04-jsc/build.sh" >&2; exit 1; }

# 1. The title with no log host.
devhost="$(sed -n 's/^PS5WK_LOG_HOST:STRING=//p' "$BLD/CMakeCache.txt")"
restore() {
    echo "==> putting the build's log host back ($devhost)"
    cmake -DPS5WK_LOG_HOST="$devhost" "$BLD" >/dev/null && ninja -C "$BLD" ps5browser >/dev/null
}
trap restore EXIT
echo "==> building with no log host"
cmake -DPS5WK_LOG_HOST= "$BLD" >/dev/null
ninja -C "$BLD" ps5wk-child ps5browser
rm -rf "$stage"
PS5WK_LOG_HOST= OUT="$stage" "$ROOT/port/webkit/apps/title/package.sh"
TITLE="$stage/PPSA99020"
# No address of this Mac or the console in the binaries.
for host in "$devhost" "${PS5WK_LOG_HOST:-}" "${PS5_HOST:-}"; do
    [[ $host =~ ^[0-9]+(\.[0-9]+){3}$ ]] || continue
    if grep -rlaF "$host" "$TITLE" >/dev/null; then
        echo "$host is still in the title: $(grep -rlaF "$host" "$TITLE" | tr '\n' ' ')" >&2; exit 1
    fi
done

# 2. The archive.
rm -rf "$out" "$out.zip"
mkdir -p "$out/data/homebrew" "$out/data/ps5webkit/share/icu"
rsync -a --exclude .DS_Store "$TITLE/" "$out/data/homebrew/PPSA99020/"
rsync -aL --exclude .DS_Store "$RUNTIME/" "$out/data/ps5webkit/"
mkdir -p "$out/data/ps5webkit/cache/fontconfig"
: > "$out/data/ps5webkit/cache/fontconfig/.keep"   # FTP clients skip empty folders
cp "$ICUDAT" "$out/data/ps5webkit/share/icu/icudt76l.dat"
cp "$ROOT/port/webkit/runtime/parallax-logo.png" "$out/data/ps5webkit/"
# The start page without its test links (they play local test media).
sed '/<!-- tests:/,/<!-- \/tests -->/d' "$ROOT/port/webkit/runtime/start.html" > "$out/data/ps5webkit/start.html"
! grep -q 'media/' "$out/data/ps5webkit/start.html" || { echo "start.html still links test media" >&2; exit 1; }
cp "$ROOT/LICENSE" "$ROOT/THIRD-PARTY-NOTICES.md" "$out/"

# Nothing from Sony and no media: only our ELFs and the clean-room libc.prx.
bad="$(cd "$out" && find . -type f \( -name '*.sprx' -o -name '*.self' -o -name '*.mp4' -o -name '*.ts' \
    -o -name '*.webm' -o -name '*.pkg' -o \( -name '*.prx' ! -path './data/homebrew/PPSA99020/sce_module/libc.prx' \) \))"
[[ -z $bad ]] || { echo "unexpected files in the release:" >&2; echo "$bad" >&2; exit 1; }
cmp -s "$out/data/homebrew/PPSA99020/sce_module/libc.prx" "$ROOT/ref/PS5VR/app/tools/native-app/runtime/libc.prx" ||
    { echo "sce_module/libc.prx is not PS5VR's clean-room libc" >&2; exit 1; }

commit="$(git -C "$ROOT" rev-parse --short HEAD)"
cat > "$out/INSTALL.txt" <<EOF
Parallax Browser $version (source: commit $commit)
https://github.com/rikleigh/parallax-browser

A homebrew web browser for a jailbroken PS5, with View in VR for the PS VR2.
Experimental: tested on firmware 13.60 only.

Requirements
  - A jailbroken PS5 (developed on 13.60) running kstuff and etaHEN, with an
    FTP server. The jailbreak is tethered: run it again after every reboot.
  - Something that registers homebrew folders in /data/homebrew, such as
    etaHEN or ShadowMount+.
  - For View in VR: a PS VR2, paired and set up in the system settings.

Install
  1. Copy the data folder in this archive onto the console's /data over FTP,
     merging folders. That puts the title in /data/homebrew/PPSA99020 and its
     runtime files in /data/ps5webkit (about $(du -sh "$out/data/ps5webkit" | cut -f1 | tr -d ' ')).
  2. Register /data/homebrew/PPSA99020 with your homebrew tool. Parallax
     Browser then appears on the home screen.

Update
  Copy the data folder again, replacing files. Bookmarks, history, settings
  and downloads are kept: they live in /data/ps5webkit next to the runtime
  files, which the archive doesn't contain.

Uninstall
  Remove the title with your homebrew tool, then delete /data/homebrew/PPSA99020
  and, to remove your browsing data too, /data/ps5webkit.

Not affiliated with or endorsed by Sony Interactive Entertainment. Licensed
under the GPL-3.0 (LICENSE); third-party components are listed in
THIRD-PARTY-NOTICES.md. The source, with the build steps, is at the address
above.
EOF

(cd "$ROOT/dist" && COPYFILE_DISABLE=1 zip -qry -X "$name.zip" "$name")
rm -rf "$out"
ls -la "$ROOT/dist/$name.zip"
shasum -a 256 "$ROOT/dist/$name.zip"
