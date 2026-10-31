#!/usr/bin/env bash
# Fetches the newest profiles of apps/common/profiler.c (PS5WK_PROFILE in
# /data/ps5webkit/child.env) from the console and summarizes them with
# test/prof/prof.py (the browser title's children, or any other run).
#   port/webkit/test/prof.sh [count=2] [--clear] [prof.py options]
# For page runs in the payload harness use test/prof/prof-run.sh.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
. "$ROOT/env.sh" >/dev/null
FTP="ftp://$PS5_HOST:${PS5_FTP_PORT:-2121}"
OUT="$ROOT/toolchain/wpe-webkit/prof"; mkdir -p "$OUT"
count=2; clear=
[[ ${1:-} =~ ^[0-9]+$ ]] && { count=$1; shift; }
[[ ${1:-} == --clear ]] && { clear=1; shift; }
files=$(curl -s "$FTP/data/ps5webkit/prof/" | awk '{print $NF}' | grep '\.prof$' | sort -t- -k2 -n | tail -"$count")
local_files=()
for f in $files; do
    curl -s "$FTP/data/ps5webkit/prof/$f" -o "$OUT/$f"
    local_files+=("$OUT/$f")
    [[ -n $clear ]] && curl -s "$FTP/" -Q "DELE /data/ps5webkit/prof/$f" >/dev/null 2>&1 || true
done
(( ${#local_files[@]} )) || { echo "no .prof files on the console"; exit 1; }
python3 -I "$HERE/prof/prof.py" "$@" "${local_files[@]}"
