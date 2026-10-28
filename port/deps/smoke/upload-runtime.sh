#!/usr/bin/env bash
# Mirror the staged runtime tree (toolchain/wpe-sysroot/share/ps5webkit-runtime:
# etc/ssl/cert.pem, etc/fonts/fonts.conf, fonts/) to /data/ps5webkit on the
# console through its FTP server (port 2121). Re-uploads every file.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
. "$ROOT/env.sh"
FTP_PORT="${FTP_PORT:-2121}"
SRC="$ROOT/toolchain/wpe-sysroot/share/ps5webkit-runtime"
DST="${PS5_RUNTIME:-/data/ps5webkit}"
[ -d "$SRC" ] || { echo "missing $SRC: run port/deps/build.sh cacert fonts" >&2; exit 1; }
cd "$SRC"
find . -type f | sort | while read -r f; do
    f="${f#./}"
    echo "put $DST/$f"
    curl -sS --ftp-create-dirs -T "$f" "ftp://$PS5_HOST:$FTP_PORT$DST/$f"
done
# Writable fontconfig cache dir (the FTP server has no MKD via -Q; creating
# a placeholder file creates the directory).
printf '' | curl -sS --ftp-create-dirs -T - "ftp://$PS5_HOST:$FTP_PORT$DST/cache/fontconfig/.keep"
