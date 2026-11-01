#!/usr/bin/env bash
# Payload-side first light: upload the child payload to the console (FTP),
# send ps5wk-shot to elfldr with its arguments on stdin, and decode the
# thumbnail it prints.
#
#   port/webkit/test/run-shot.sh [url] [timeout-s]
#   SHOT_ARGS="settle=20 js=/data/ps5webkit/shot/diag.js" run-shot.sh <url>   (see ps5wk-shot.c)
#
# Output: toolchain/wpe-webkit/shots/<time>.log and .ppm/.png (sips).
# The ELFs are stripped copies of the build (debug info stays on the Mac).
# BLD=<build dir>; CHILD_REMOTE=<path on the console> uploads the child there
# and passes child=<path> (a second build next to the shared one, e.g.
# CHILD_REMOTE=/data/ps5webkit/jit/ps5wk-child.elf); SHOT_OUT=<dir> for logs.
# SHOT_REMOTE=<path on the console>: ps5wk-shot is uploaded there (FTP, unless
# SKIP_UPLOAD) and started from there by test/shotrelay (a small payload that
# sends it to the local elfldr), instead of sending 129 MB to elfldr each run.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
. "$ROOT/env.sh" >/dev/null
. "$PS5_PAYLOAD_SDK/toolchain/prospero.sh"
WK="$ROOT/toolchain/wpe-webkit"
BLD="${BLD:-$WK/build}"
URL="${1:-https://example.com/}"
TIMEOUT="${2:-150}"
FTP="ftp://$PS5_HOST:${PS5_FTP_PORT:-2121}"
OUTD="${SHOT_OUT:-$WK/shots}"; mkdir -p "$OUTD"
CHILD_REMOTE="${CHILD_REMOTE:-/data/ps5webkit/ps5wk-child.elf}"
STAMP="$(date +%Y%m%d-%H%M%S)"
LOG="$OUTD/$STAMP.log"

find_elf() { for d in "$BLD/bin" "$BLD/ps5-apps"; do [[ -f $d/$1 ]] && { echo "$d/$1"; return; }; done; echo "missing $1" >&2; exit 1; }
CHILD="$(find_elf ps5wk-child)"
SHOT="$(find_elf ps5wk-shot)"
mkdir -p "$OUTD/stripped"
prospero-strip -o "$OUTD/stripped/ps5wk-child.elf" "$CHILD"
prospero-strip -o "$OUTD/stripped/ps5wk-shot.elf" "$SHOT"
ls -la "$OUTD/stripped"

ICUDAT="$ROOT/toolchain/icu/src-76.1/icu/source/data/in/icudt76l.dat"
if ! curl -s --max-time 5 "$FTP/data/ps5webkit/share/icu/" | grep -q icudt76l.dat; then
    echo "==> uploading ICU data to /data/ps5webkit/share/icu/"
    curl -s --ftp-create-dirs -T "$ICUDAT" "$FTP/data/ps5webkit/share/icu/icudt76l.dat"
fi
if [[ -z ${SKIP_UPLOAD:-} ]]; then
    echo "==> uploading ps5wk-child.elf to $CHILD_REMOTE (FTP)"
    curl -s --ftp-create-dirs -T "$OUTD/stripped/ps5wk-child.elf" "$FTP$CHILD_REMOTE"
fi

PAYLOAD="$OUTD/stripped/ps5wk-shot.elf"
PREFIX=
if [[ -n ${SHOT_REMOTE:-} ]]; then
    prospero-clang -O2 -o "$OUTD/stripped/shotrelay.elf" "$HERE/shotrelay/shotrelay.c"
    if [[ -z ${SKIP_UPLOAD:-} ]]; then
        echo "==> uploading ps5wk-shot.elf to $SHOT_REMOTE (FTP)"
        curl -s --ftp-create-dirs -T "$OUTD/stripped/ps5wk-shot.elf" "$FTP$SHOT_REMOTE"
    fi
    PAYLOAD="$OUTD/stripped/shotrelay.elf"
    PREFIX="$SHOT_REMOTE "
fi
echo "==> ps5wk-shot $URL (timeout $TIMEOUT s)" | tee "$LOG"
{ cat "$PAYLOAD"; printf '%surl=%s timeout=%s child=%s %s %s\n' "$PREFIX" "$URL" "$TIMEOUT" "$CHILD_REMOTE" "${SHOT_ARGS:-}" "${SHOT_DEBUG:+debug=$SHOT_DEBUG}"; } |
    socat -t $((TIMEOUT + 60)) -T $((TIMEOUT + 60)) - "TCP:$PS5_HOST:$PS5_PORT" 2>&1 | tee -a "$LOG" | grep -v '^THUMB ' || true

grep '^THUMB ' "$LOG" | cut -c7- | tr -d '\n' | base64 -D > "$OUTD/$STAMP.ppm" 2>/dev/null || true
if [[ -s "$OUTD/$STAMP.ppm" ]]; then
    sips -s format png "$OUTD/$STAMP.ppm" --out "$OUTD/$STAMP.png" >/dev/null 2>&1 || true
    echo "thumbnail: $OUTD/$STAMP.png"
fi
echo "log: $LOG"
grep '^RESULT' "$LOG" || true
