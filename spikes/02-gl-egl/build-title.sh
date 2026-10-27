#!/usr/bin/env bash
# Build Spike 2's gl_egl as a registered title against ps5-opengl.
#   spikes/02-gl-egl/build-title.sh [gles|stock]
# Also links Spike 1's "safe" + "jit" tests (-DWITH_PROCS_TESTS), so the same
# process/IPC/memory checks run in title context.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
. "$ROOT/env.sh"
VARIANT="${1:-gles}"
PREFIX="$ROOT/toolchain/ps5-opengl-$VARIANT"
OBJ="$ROOT/spikes/02-gl-egl/build/obj-$VARIANT"; mkdir -p "$OBJ"
CF=(-std=c11 -O2 -g -Wall -Werror -DGL_GLEXT_PROTOTYPES=1 -I"$PREFIX/include" -I"$ROOT/spikes"
    -DLOG_HOST="\"$PS5WK_LOG_HOST\"" -DLOG_PORT="$PS5WK_LOG_PORT")
prospero-clang "${CF[@]}" -DWITH_PROCS_TESTS -c "$ROOT/spikes/02-gl-egl/src/gl_egl.c" -o "$OBJ/gl_egl.o"
make -s -C "$ROOT/spikes/01-procs-ipc" child.elf
prospero-clang "${CF[@]}" -DPROCS_IPC_LIBRARY -DSPIKE01_GROUP=5 -DSPIKE_NAME='"01-procs-ipc-title"' \
    -c "$ROOT/spikes/01-procs-ipc/procs_ipc.c" -o "$OBJ/procs_ipc.o"
(cd "$ROOT/spikes/01-procs-ipc" && prospero-clang -c child_blob.S -o "$OBJ/child_blob.o")
# Clean exit (title_exit.c); psvr2.c holds the export binding it uses.
prospero-clang "${CF[@]}" -c "$ROOT/spikes/common/psvr2/psvr2.c" -o "$OBJ/psvr2.o"
prospero-clang "${CF[@]}" -c "$ROOT/spikes/common/title_exit.c" -o "$OBJ/title_exit.o"
"$ROOT/spikes/02-gl-egl/ps5-opengl/package-title.sh" -p "$PREFIX" -t PPSA99006 \
    -n "ps5webkit spike 02" -o "$ROOT/spikes/02-gl-egl/build/titles" \
    "$OBJ/gl_egl.o" "$OBJ/procs_ipc.o" "$OBJ/child_blob.o" \
    "$OBJ/psvr2.o" "$OBJ/title_exit.o"
