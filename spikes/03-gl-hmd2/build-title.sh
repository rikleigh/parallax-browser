#!/usr/bin/env bash
# Build Spike 3b (gl_hmd2) as a registered title against ps5-opengl with the
# texture-descriptor export (the "gles" variant carries both patches).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
. "$ROOT/env.sh"
PREFIX="$ROOT/toolchain/ps5-opengl-${1:-gles}"
OBJ="$ROOT/spikes/03-gl-hmd2/build/obj"; mkdir -p "$OBJ"
CF=(-std=c11 -O2 -g -Wall -Werror -DGL_GLEXT_PROTOTYPES=1 -I"$PREFIX/include" -I"$ROOT/spikes"
    -DLOG_HOST="\"$PS5WK_LOG_HOST\"" -DLOG_PORT="$PS5WK_LOG_PORT")
prospero-clang "${CF[@]}" -c "$ROOT/spikes/03-gl-hmd2/gl_hmd2.c" -o "$OBJ/gl_hmd2.o"
prospero-clang "${CF[@]}" -c "$ROOT/spikes/common/psvr2/psvr2.c" -o "$OBJ/psvr2.o"
"$ROOT/spikes/02-gl-egl/ps5-opengl/package-title.sh" -p "$PREFIX" -t PPSA99007 \
    -n "ps5webkit spike 03b" -o "$ROOT/spikes/03-gl-hmd2/build/titles" "$OBJ/gl_hmd2.o" "$OBJ/psvr2.o"
# PS VR2 titles declare it in param.json: Hmd2 refused this title with
# 0x8a72001f while its attribute words were 0. Take PS5VR's (a working VR
# title); keep ps5-opengl's memory (amm/kernel) settings.
python3 - "$ROOT/spikes/03-gl-hmd2/build/titles/PPSA99007/sce_sys/param.json" \
    "$ROOT/ref/PS5VR/app/sce_sys/param.json" <<'PY'
import json, sys
p = json.load(open(sys.argv[1])); vr = json.load(open(sys.argv[2]))
for k in ("attribute", "attribute2", "attribute3"):
    p[k] = vr.get(k, 0)
open(sys.argv[1], "w").write(json.dumps(p, indent=2) + "\n")
print("param.json: attribute 0x%x attribute3 0x%x" % (p["attribute"], p["attribute3"]))
PY
