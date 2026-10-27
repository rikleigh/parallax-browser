#!/usr/bin/env bash
# Build Spike 9 (hw_decode) as registered title PPSA99014, linked against
# ps5-opengl (the gl job draws on the TV; the decoder itself needs no GL).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
. "$ROOT/env.sh"
HERE="$ROOT/spikes/09-hw-decode"
PREFIX="$ROOT/toolchain/ps5-opengl-${1:-gles}"
OBJ="$HERE/build/obj"; mkdir -p "$OBJ"
CF=(-std=gnu11 -O2 -g -Wall -Werror -DGL_GLEXT_PROTOTYPES=1 -I"$PREFIX/include" -I"$ROOT/spikes"
    -DLOG_HOST="\"$PS5WK_LOG_HOST\"" -DLOG_PORT="$PS5WK_LOG_PORT")
for f in hw_decode vdec2 au; do
    prospero-clang "${CF[@]}" -c "$HERE/$f.c" -o "$OBJ/$f.o"
done
prospero-clang "${CF[@]}" -c "$ROOT/spikes/common/psvr2/psvr2.c" -o "$OBJ/psvr2.o"
prospero-clang "${CF[@]}" -c "$ROOT/spikes/common/title_exit.c" -o "$OBJ/title_exit.o"
"$ROOT/spikes/02-gl-egl/ps5-opengl/package-title.sh" -p "$PREFIX" -t PPSA99014 \
    -n "ps5webkit spike 09" -o "$HERE/build/titles" "$OBJ"/{hw_decode,vdec2,au,psvr2,title_exit}.o
# PS5VR's param.json attributes (a title that decodes with Videodec2 and
# drives PS VR2); keep ps5-opengl's memory settings.
python3 - "$HERE/build/titles/PPSA99014/sce_sys/param.json" "$ROOT/ref/PS5VR/app/sce_sys/param.json" <<'PY'
import json, sys
p = json.load(open(sys.argv[1])); vr = json.load(open(sys.argv[2]))
for k in ("attribute", "attribute2", "attribute3"):
    p[k] = vr.get(k, 0)
open(sys.argv[1], "w").write(json.dumps(p, indent=2) + "\n")
print("param.json: attribute 0x%x attribute3 0x%x" % (p["attribute"], p["attribute3"]))
PY
