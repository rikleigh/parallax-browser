#!/usr/bin/env bash
# Fails if an executable linked with ps5locks.c imports a pthread mutex,
# condition variable or rwlock function (or their attributes) that ps5locks
# does not wrap: that function would read ps5locks's words as a libc object.
#   check-lock-imports.sh <elf> <wrapped function>...
set -euo pipefail
elf="$1"; shift
READELF="${READELF:-/opt/homebrew/opt/llvm@18/bin/llvm-readelf}"
[[ -x $READELF ]] || READELF="$(command -v llvm-readelf || command -v readelf)"
bad=()
for sym in $("$READELF" --dyn-syms "$elf" | awk '$7 == "UND" { sub(/@.*/, "", $8); print $8 }' |
        grep -E '^pthread_(mutex|cond|rwlock)' | sort -u); do
    found=0
    for w in "$@"; do [[ $sym == "$w" ]] && found=1 && break; done
    (( found )) || bad+=("$sym")
done
if (( ${#bad[@]} )); then
    echo "check-lock-imports: $(basename "$elf") imports pthread functions ps5locks does not wrap: ${bad[*]}" >&2
    echo "  add them to ps5locks.c and PS5_LOCK_FUNCTIONS (apps/CMakeLists.txt)" >&2
    exit 1
fi
echo "check-lock-imports: $(basename "$elf"): every pthread mutex/cond/rwlock import is wrapped"
