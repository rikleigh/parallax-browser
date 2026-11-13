#!/bin/bash
# Spike 12 P2: builds and runs dawn-assumptions.cpp on the host, against the null-backend Dawn that
# `spikes/12-webgpu/p0a-dawn/build.sh host` builds from the same patched source.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
DAWN="$HERE/../p0a-dawn/build/dawn-host/install"
[ -f "$DAWN/lib/libwebgpu_dawn.a" ] || { echo "no $DAWN: run spikes/12-webgpu/p0a-dawn/build.sh host" >&2; exit 1; }
mkdir -p "$HERE/obj"
clang++ -std=c++20 -O1 -Wall -fno-exceptions -fno-rtti -I"$DAWN/include" \
    "$HERE/dawn-assumptions.cpp" "$DAWN/lib/libwebgpu_dawn.a" \
    -framework CoreFoundation -framework IOKit -framework IOSurface -framework Foundation \
    -o "$HERE/obj/dawn-assumptions" 2> >(grep -v '^ld: warning' >&2)
exec "$HERE/obj/dawn-assumptions"
