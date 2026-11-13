#!/usr/bin/env bash
# RADV as one object a title can link next to ps5-opengl (which carries its
# own Mesa 26.2: 4163 global symbols in common).
#   spikes/12-webgpu/p0b-radv/radv-object.sh   -> build/radv-ps5/radv-local.o
# Needs build/radv-ps5/install from build-radv.sh. Used by the spike titles
# and by port/webkit/apps/title/package.sh (WebGPU builds).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
. "$ROOT/env.sh" >/dev/null
RADV="$ROOT/build/radv-ps5"
LLVM=/opt/homebrew/opt/llvm@18/bin
A="$RADV/install/lib/libvulkan_radeon.ps5.a"
[[ -f $A ]] || { echo "run spikes/12-webgpu/p0b-radv/build-radv.sh first" >&2; exit 1; }
[[ -f $RADV/radv-local.o && ! $A -nt $RADV/radv-local.o ]] && exit 0
# The SDK's prospero-lld wrapper adds a link script that breaks -r.
"$LLVM/ld.lld" -m elf_x86_64 -r --whole-archive "$A" -o "$RADV/radv-whole.o"
# Mesa's dispatch tables name every driver entry point through a weak
# reference (radv_Foo, else vk_common_Foo). Those nothing defines must be
# null, and the title converter refuses an undefined symbol: define them
# as absolute 0 here, inside the object.
"$LLVM/llvm-nm" -u "$RADV/radv-whole.o" | awk '$1 == "w" { print ".globl " $2 "\n.set " $2 ", 0" }' > "$RADV/weak0.s"
"$PS5_PAYLOAD_SDK/bin/prospero-clang" -c "$RADV/weak0.s" -o "$RADV/weak0.o"
"$LLVM/ld.lld" -m elf_x86_64 -r "$RADV/radv-whole.o" "$RADV/weak0.o" -o "$RADV/radv-whole2.o"
# Everything local but the ICD's entry points: the two Mesas never meet.
printf '%s\n' vk_icdGetInstanceProcAddr vk_icdNegotiateLoaderICDInterfaceVersion vk_icdGetPhysicalDeviceProcAddr > "$RADV/keep.txt"
"$LLVM/llvm-objcopy" --strip-debug --keep-global-symbols="$RADV/keep.txt" "$RADV/radv-whole2.o" "$RADV/radv-local.o"
rm -f "$RADV/radv-whole.o" "$RADV/radv-whole2.o"
echo "RADV object: $(wc -l < "$RADV/weak0.s" | awk '{print $1/2}') weak entry points nulled, $("$LLVM/llvm-size" "$RADV/radv-local.o" | tail -1)"
