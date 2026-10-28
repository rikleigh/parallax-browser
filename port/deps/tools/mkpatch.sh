#!/usr/bin/env bash
# mkpatch.sh <pkg> <patch-name> <file-in-src>...
# Diff files in toolchain/wpe-deps/src/<pkg> against <file>.orig copies
# (made by `mkpatch.sh --orig <pkg> <file>...`) into port/deps/patches/<pkg>/.
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"; ROOT="$(cd "$HERE/../.." && pwd)"
if [ "$1" = --orig ]; then
    pkg=$2; shift 2
    for f in "$@"; do cp -n "$ROOT/toolchain/wpe-deps/src/$pkg/$f" "$ROOT/toolchain/wpe-deps/src/$pkg/$f.orig"; done
    exit 0
fi
pkg=$1 name=$2; shift 2
out="$HERE/patches/$pkg/$name"
mkdir -p "$HERE/patches/$pkg"; : > "$out"
cd "$ROOT/toolchain/wpe-deps/src/$pkg"
for f in "$@"; do
    diff -u --label "a/$f" --label "b/$f" "$f.orig" "$f" >> "$out" || true
done
echo "wrote $out ($(wc -l < "$out") lines)"
