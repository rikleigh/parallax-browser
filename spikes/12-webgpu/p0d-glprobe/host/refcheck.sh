#!/usr/bin/env bash
# Checks the probe itself, not the PS5 driver: the same probe sources over
# ps5-opengl's EGL facade and Mesa 26.2 with Mesa's software rasteriser
# (softpipe) in place of the ps5 Gallium screen. Every group runs for real
# here, so a FAIL is either a mistake in the probe or something softpipe does
# not do (no real multisampling, for one). Run it after changing a test, so a
# FAIL on the console can be read as the driver's.
#
# Mesa's version is overridden to GL 4.6 / ES 3.2 (softpipe offers less), so
# the facade creates the same two contexts as on the console.
# Uses spike 2's host Mesa build read-only (its compile commands and
# libraries); softpipe's objects are built here.
#   spikes/12-webgpu/p0d-glprobe/host/refcheck.sh
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROBE="$(cd "$HERE/.." && pwd)"
ROOT="$(cd "$PROBE/../../.." && pwd)"
SPIKE2="$ROOT/spikes/02-gl-egl/ps5-opengl"
T="$SPIKE2/build/gles"; O2="$SPIKE2/build/hostcheck"; M="$T/third_party/mesa-26.2.0"
O="$PROBE/build/ref"; mkdir -p "$O/obj" "$O/sp" "$PROBE/results"
[[ -f $O2/mesa/compile_commands.json ]] || { echo "run spikes/02-gl-egl/ps5-opengl/hostcheck/run.sh first" >&2; exit 1; }
NM=/opt/homebrew/opt/llvm@18/bin/llvm-nm
arch=(-arch x86_64 -mmacosx-version-min=13.0)
cc() { /usr/bin/clang "${arch[@]}" "$@"; }

# 1. softpipe and the null software winsys, with the host Mesa build's own
#    compile commands, objects written here.
python3 - "$O2/mesa/compile_commands.json" "$O/sp" <<'PY'
import json, os, shlex, subprocess, sys
from concurrent.futures import ThreadPoolExecutor
commands, out = json.load(open(sys.argv[1])), sys.argv[2]
def build(c):
    if "/drivers/softpipe/" not in c["file"] and "winsys/sw/null" not in c["file"]:
        return
    obj = os.path.join(out, os.path.basename(c["file"]) + ".o")
    src = os.path.join(c["directory"], c["file"])
    if os.path.exists(obj) and os.path.getmtime(obj) > os.path.getmtime(src):
        return
    args, keep, skip = shlex.split(c["command"]), [], 0
    for a in args:
        if skip:
            skip -= 1
        elif a in ("-MD", "-MP"):
            pass
        elif a in ("-MQ", "-MF", "-o"):
            skip = 1
        else:
            keep.append(a)
    subprocess.run(keep + ["-w", "-o", obj], cwd=c["directory"], check=True)
with ThreadPoolExecutor(8) as pool:
    list(pool.map(build, commands))
PY

# 2. The facade with softpipe as its screen.
common=(-std=c11 -O1 -g -w -DHAVE_FUNC_ATTRIBUTE_PACKED=1 -I"$O2/mesa/src" -I"$M/src" -I"$M/include"
        -I"$M/src/gallium/include" -I"$T/src/gallium/ps5" -I"$M/src/gallium/auxiliary")
cc "${common[@]}" -DHAVE_PTHREAD=1 -DPS5_NATIVE_TITLE_RUNTIME=1 -DPS5_FRAME_SUSPEND=1 -DPS5_RUNTIME_QUIET=1 \
   -DPS5_GPU_PRESENT_BATCH=1 -DPS5_MULTIDRAW_BATCH=1 -DPS5_DEFERRED_DRAW_BATCH=1 -DHAVE_STRUCT_TIMESPEC=1 \
   -DPS5_ENABLE_COMPRESSED_FALLBACK_CANDIDATE=1 -DPS5_ENABLE_CORE_CONTEXT_CANDIDATE=1 \
   -DHAVE_OPENGL=1 -DHAVE_OPENGL_ES_1=0 -DHAVE_OPENGL_ES_2=1 -I"$M/src/mesa" \
   -Dps5_screen_create=ref_screen_create -c "$T/src/egl/ps5_egl.c" -o "$O/obj/ps5_egl.o"
cc "${common[@]}" -DHAVE_PTHREAD=1 -DHAVE_STRUCT_TIMESPEC=1 -c "$HERE/ref_glue.c" -o "$O/obj/ref_glue.o"

# 3. The probe, every group counted.
pf=(-std=c11 -O1 -g -Wall -Werror -DHOST_CHECK=1 -DPROBE_REF=1 '-DPROBE_SUITE="12-p0d-glprobe-ref"'
    -DEGL_NO_PLATFORM_SPECIFIC_TYPES=1 -I"$M/include")
for f in glprobe glprobe_gpu; do cc "${pf[@]}" -c "$PROBE/src/$f.c" -o "$O/obj/$f.o"; done
cc "${pf[@]}" -c "$HERE/main_host.c" -o "$O/obj/main_host.o"

libs=(src/mesa/glapi/glapi/libglapi_bridge.a src/mesa/libmesa.a src/mesa/libmesa_sse41.a
      src/gallium/auxiliary/libgallium.a src/mesa/glapi/shared-glapi/libglapi.a
      src/compiler/glsl/libglsl.a src/compiler/glsl/glcpp/libglcpp.a src/compiler/glsl/libglsl_util.a
      src/compiler/spirv/libvtn.a src/compiler/nir/libnir.a src/compiler/libcompiler.a
      src/util/libmesa_util.a src/util/libmesa_util_simd.a src/util/libmesa_util_clflush.a
      src/util/libmesa_util_clflushopt.a src/util/blake3/libblake3.a src/c11/impl/libmesa_util_c11.a)
mlibs=(); for l in "${libs[@]}"; do mlibs+=("$O2/mesa/$l"); done
weak=(); for w in $("$NM" "$T"/build/core33-native-runtime/ps5_egl.o | awk '$1=="w"{print $2}' | sort -u); do
    weak+=("-Wl,-U,_$w"); done
/usr/bin/clang++ "${arch[@]}" -o "$O/glprobe_ref" "$O"/obj/*.o "$O"/sp/*.o "$O2"/obj/u_framebuffer.o "$O2"/obj/platform_stubs.o \
    "${mlibs[@]}" "${mlibs[@]}" "${weak[@]}" 2> "$O/link.log" || { grep -E "Undefined|\"_|error" "$O/link.log" | head -30; exit 1; }

log="$PROBE/results/refcheck-softpipe-$(date +%Y-%m-%d).log"
echo "running $O/glprobe_ref under Rosetta (log: $log)"
{ echo "# refcheck $(date '+%Y-%m-%d %H:%M'): the probe over softpipe (not the PS5 driver), GL 4.6 / ES 3.2 by override"
  MESA_GL_VERSION_OVERRIDE=4.6 MESA_GLSL_VERSION_OVERRIDE=460 MESA_GLES_VERSION_OVERRIDE=3.2 \
      arch -x86_64 "$O/glprobe_ref" 2>&1; } > "$log" || true
grep -E "^RESULT|PASS,|^==" "$log" | cut -c1-330
