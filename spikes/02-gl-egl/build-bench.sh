#!/usr/bin/env bash
# Build Spike 2b (gl_draw_bench) as a title against one ps5-opengl install.
#   spikes/02-gl-egl/build-bench.sh [VARIANT [TITLE_ID]]
# VARIANT names toolchain/ps5-opengl-<VARIANT> (default gles; gles-noprof is
# the same tree installed with PS5_DRAW_PROFILE=0 PS5_RUNTIME_QUIET=1).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
. "$ROOT/env.sh"
VARIANT="${1:-gles}" TITLE="${2:-PPSA99010}"
PREFIX="$ROOT/toolchain/ps5-opengl-$VARIANT"
OBJ="$ROOT/spikes/02-gl-egl/build/obj-bench-$VARIANT"; mkdir -p "$OBJ"
CF=(-std=c11 -O2 -g -Wall -Werror -DGL_GLEXT_PROTOTYPES=1 -I"$PREFIX/include" -I"$ROOT/spikes"
    -DLOG_HOST="\"$PS5WK_LOG_HOST\"" -DLOG_PORT="$PS5WK_LOG_PORT" -DBENCH_VARIANT="\"$VARIANT\"")
prospero-clang "${CF[@]}" -c "$ROOT/spikes/02-gl-egl/src/gl_draw_bench.c" -o "$OBJ/gl_draw_bench.o"
prospero-clang "${CF[@]}" -c "$ROOT/spikes/common/psvr2/psvr2.c" -o "$OBJ/psvr2.o"
prospero-clang "${CF[@]}" -c "$ROOT/spikes/common/title_exit.c" -o "$OBJ/title_exit.o"
"$ROOT/spikes/02-gl-egl/ps5-opengl/package-title.sh" -p "$PREFIX" -t "$TITLE" \
    -n "ps5webkit spike 02b $VARIANT" -o "$ROOT/spikes/02-gl-egl/build/titles" \
    "$OBJ/gl_draw_bench.o" "$OBJ/psvr2.o" "$OBJ/title_exit.o"
