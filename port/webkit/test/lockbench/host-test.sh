#!/usr/bin/env bash
# Logic tests of apps/common/ps5locks.c and ps5malloc.c on the Mac (see host-test.c).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="${TMPDIR:-/tmp}/ps5locks-host-test"
clang -std=gnu11 -O2 -g -Wall -Wno-deprecated-declarations -fsanitize=${SANITIZE:-thread} "$HERE/host-test.c" -o "$OUT"
"$OUT"
