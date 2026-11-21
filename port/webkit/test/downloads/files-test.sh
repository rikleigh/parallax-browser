#!/usr/bin/env bash
# Moving and copying (apps/common/ps5files.c) on the Mac:
#   port/webkit/test/downloads/files-test.sh
# Same-drive moves are renames; a 64 MB disk image (hdiutil) is the "other
# drive", so moves to it copy and delete. Also compiles ps5files.c for the
# PS5 when the toolchain is there.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../../../.." && pwd)"
work="$(mktemp -d)"
volume=""
cleanup() {
    [ -z "$volume" ] || hdiutil detach -quiet "$volume" 2>/dev/null || true
    chmod -R u+w "$work" 2>/dev/null || true
    rm -rf "$work"
}
trap cleanup EXIT
fails=0
passes=0
pass() { echo "PASS $*"; passes=$((passes + 1)); }
failed() { echo "FAIL $*"; fails=$((fails + 1)); }

drive="$work/files-drive"
if cc -std=c11 -O2 -Wall -Wextra -Werror -o "$drive" "$here/files/files-drive.c" "$root/port/webkit/apps/common/ps5files.c" 2>"$work/cc.log"; then
    pass "builds without warnings (-Wall -Wextra)"
else
    cat "$work/cc.log"
    failed "doesn't build without warnings"
    exit 1
fi
sdk="$root/toolchain/ps5-payload-sdk"
if [ -x "$sdk/bin/prospero-clang" ] && [ -f "$root/env.sh" ]; then
    if (. "$root/env.sh" >/dev/null 2>&1; "$sdk/bin/prospero-clang" -std=c11 -O2 -Wall -Wextra -Werror -c "$root/port/webkit/apps/common/ps5files.c" \
        -o "$work/ps5files.o") 2>"$work/ps5cc.log"; then
        pass "ps5files.c compiles for the PS5 (no warnings)"
    else
        cat "$work/ps5cc.log"
        failed "ps5files.c doesn't compile for the PS5"
    fi
fi

# check <description> <expected (a glob)> <files-drive args...>
check() {
    local what="$1" want="$2"
    shift 2
    local got
    got="$("$drive" "$@")"
    # shellcheck disable=SC2053
    if [[ "$got" == $want ]]; then pass "$what: $got"; else failed "$what: got '$got', want '$want'"; return 1; fi
}
tree() { # a folder of nested files, an empty folder, a name with spaces
    mkdir -p "$1/sce_sys" "$1/data/deep/er" "$1/empty"
    head -c 3000000 /dev/urandom > "$1/eboot.bin"
    head -c 9000000 /dev/urandom > "$1/data/deep/er/big file.bin"
    echo '{"titleId":"PPSA00001"}' > "$1/sce_sys/param.json"
    : > "$1/data/zero"
}
same() { diff -r "$1" "$2" >/dev/null; }

hdiutil create -quiet -size 64m -fs HFS+ -volname ps5files -o "$work/other.dmg"
volume="$(hdiutil attach -nobrowse "$work/other.dmg" | awk -F'\t' '/\/Volumes\//{print $NF}')"
other="$volume"

src="$work/src"
tree "$src/App"
cp -R "$src/App" "$work/reference"

check "copy a folder, same drive" "OK" copy "$src/App" "$work/copy1" && { same "$work/reference" "$work/copy1" && [ -d "$src/App" ] \
    && pass "  the copy is the same, the original stays" || failed "  copy differs"; }
[ "$(stat -f %Lp "$work/copy1/eboot.bin")" = 777 ] && [ "$(stat -f %Lp "$work/copy1/data/deep")" = 777 ] && pass "  files and folders 0777" \
    || failed "  modes $(stat -f %Lp "$work/copy1/eboot.bin")"
check "copy a file" "OK" copy "$src/App/eboot.bin" "$work/eboot-copy.bin" && { cmp -s "$src/App/eboot.bin" "$work/eboot-copy.bin" \
    && pass "  same bytes" || failed "  bytes differ"; }
check "there already: refused" "REFUSED There is already something called copy1 there" copy "$src/App" "$work/copy1"
check "into itself: refused" "REFUSED It can't go into itself" move "$src/App" "$src/App/data/x"
check "onto itself: refused" "REFUSED It can't go into itself" copy "$src/App" "$src/App/"
check "not there: refused" "REFUSED nothing isn't there" copy "$work/nothing" "$work/x"

mv "$work/copy1" "$work/same-drive-src"
check "move a folder, same drive (a rename)" "OK" move "$work/same-drive-src" "$work/moved-here" && { same "$work/reference" "$work/moved-here" \
    && [ ! -e "$work/same-drive-src" ] && pass "  moved whole" || failed "  same-drive move"; }

check "move a folder to the other drive" "OK" move "$work/moved-here" "$other/App" --margin 1048576 && { same "$work/reference" "$other/App" \
    && [ ! -e "$work/moved-here" ] && [ ! -e "$other/App.part" ] && pass "  copied, then the original deleted, no .part left" \
    || failed "  cross-drive move"; }
check "move a file to the other drive" "OK" move "$work/eboot-copy.bin" "$other/eboot.bin" --margin 1048576 && { cmp -s "$src/App/eboot.bin" "$other/eboot.bin" \
    && [ ! -e "$work/eboot-copy.bin" ] && pass "  same bytes, original gone" || failed "  cross-drive file move"; }

mkdir -p "$work/big"
head -c 80000000 /dev/urandom > "$work/big/80mb.bin"
check "too big for the other drive: refused before writing" "REFUSED Not enough space there*" copy "$work/big" "$other/big" \
    && { [ ! -e "$other/big.part" ] && [ ! -e "$other/big" ] && pass "  nothing written" || failed "  something written"; }

tree "$work/cancel-src"
cp -R "$work/cancel-src" "$work/cancel-ref"
check "cancelled half way" "CANCELLED*" copy "$work/cancel-src" "$other/cancelled" --margin 1048576 --cancel-after 1 \
    && { [ ! -e "$other/cancelled.part" ] && [ ! -e "$other/cancelled" ] && [ -f "$work/cancel-src/data/deep/er/big file.bin" ] \
    && pass "  .part deleted, the original untouched" || failed "  after cancel"; }
check "a cancelled move leaves the original" "CANCELLED*" move "$work/cancel-src" "$other/cancelled" --margin 1048576 --cancel-after 1 \
    && { same "$work/cancel-ref" "$work/cancel-src" && [ ! -e "$other/cancelled.part" ] && pass "  original whole, no .part" || failed "  original changed"; }

tree "$work/link-src"
ln -s /etc/passwd "$work/link-src/data/link"
check "a link inside: refused before writing" "REFUSED Links aren't moved or copied: link" copy "$work/link-src" "$other/linked" \
    && { [ ! -e "$other/linked.part" ] && pass "  nothing written" || failed "  .part left"; }
tree "$work/fifo-src"
mkfifo "$work/fifo-src/pipe"
check "a FIFO inside: refused" "REFUSED Special files aren't moved or copied: pipe" copy "$work/fifo-src" "$other/fifo"
mkdir -p "$other/busy.part"
check "a .part of that name there: refused" "REFUSED There is already something called busy.part there" copy "$src/App" "$other/busy"

# races (the review of a0226d3): what appears once the checks are done
tree "$work/race-src"
cp -R "$work/race-src" "$work/race-ref"
check "another's .part appears first: refused, theirs kept" "FAILED*" copy "$work/race-src" "$other/race" --margin 1048576 \
    --create-at-start "$other/race.part/" && { [ -d "$other/race.part" ] && [ ! -e "$other/race" ] && pass "  their .part untouched" \
    || failed "  their .part was deleted"; }
rmdir "$other/race.part"
check "dest appears during the copy: not replaced" "REFUSED Something called race appeared there meanwhile" copy "$work/race-src" "$other/race" \
    --margin 1048576 --create-at-start "$other/race" && { [ "$(cat "$other/race")" = planted ] && [ ! -e "$other/race.part" ] \
    && pass "  theirs kept, ours cleaned up" || failed "  dest replaced or .part left"; }
rm -f "$other/race"
check "a file written into a moved folder meanwhile: the original kept" "OK Copied, but the original changed meanwhile: it was kept" \
    move "$work/race-src" "$other/race" --margin 1048576 --create-at-end "$work/race-src/late.txt" \
    && { [ -f "$work/race-src/late.txt" ] && [ -d "$other/race" ] && pass "  nothing lost: the source and its new file are still there" \
    || failed "  source deleted"; }
rm -rf "$other/race"
mkdir -p "$work/CaseApp/inner"
check "into itself by another spelling (a drive that ignores case)" "REFUSED It can't go into itself" copy "$work/CaseApp" "$work/caseapp/inner/x"

echo "files-test: $passes passed, $fails failed"
[ "$fails" = 0 ]
