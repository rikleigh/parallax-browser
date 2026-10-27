#!/usr/bin/env bash
# Build Spike 8 as a registered title (PPSA99009) with server.elf embedded.
# The title links ps5-opengl's runtime only because package-title.sh does;
# it draws nothing.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
. "$ROOT/env.sh"
HERE="$ROOT/spikes/08-title-ipc"
PREFIX="$ROOT/toolchain/ps5-opengl-gles"
OBJ="$HERE/build/obj"; mkdir -p "$OBJ"
CF=(-std=c11 -O2 -g -Wall -Werror -I"$PREFIX/include" -I"$ROOT/spikes"
    -DLOG_HOST="\"$PS5WK_LOG_HOST\"" -DLOG_PORT="$PS5WK_LOG_PORT")
make -s -C "$HERE" server.elf
prospero-clang "${CF[@]}" -c "$HERE/client.c" -o "$OBJ/client.o"
prospero-clang "${CF[@]}" -c "$ROOT/spikes/common/psvr2/psvr2.c" -o "$OBJ/psvr2.o"
(cd "$HERE" && prospero-clang -c server_blob.S -o "$OBJ/server_blob.o")
"$ROOT/spikes/02-gl-egl/ps5-opengl/package-title.sh" -p "$PREFIX" -t PPSA99009 \
    -n "ps5webkit spike 08" -o "$HERE/build/titles" "$OBJ/client.o" "$OBJ/psvr2.o" "$OBJ/server_blob.o"
