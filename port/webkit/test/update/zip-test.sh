#!/usr/bin/env bash
# The release zip unpacker (apps/common/ps5zip.c) on the Mac:
#   port/webkit/test/update/zip-test.sh [title folder]
# Unpacks the title folder zipped as make-release.sh zips it and compares
# the result byte for byte, then checks that bad archives are refused.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../../../.." && pwd)"
title="${1:-$root/toolchain/wpe-webkit/titles/PPSA99207}"
work="$(mktemp -d)"
trap 'chmod -R u+w "$work" 2>/dev/null; rm -rf "$work"' EXIT
cc -O2 -Wall -Wextra -o "$work/zip-test" "$here/zip-test.c" "$root/port/webkit/apps/common/ps5zip.c" -lz
fails=0
check() { # expect name archive [prefix] [readonly]
    local expect="$1" name="$2" archive="$3" prefix="${4:-PPSA99207/}"
    chmod -R u+w "$work/out" 2>/dev/null || true
    rm -rf "$work/out" && mkdir "$work/out"
    [[ -z ${5:-} ]] || chmod 0555 "$work/out"
    local got
    got="$("$work/zip-test" "$archive" "$prefix" "$work/out" || true)"
    if [[ "$got" == "$expect"* ]]; then echo "PASS $name: $got"; else echo "FAIL $name: $got (expected $expect)"; fails=$((fails + 1)); fi
}

if [ -d "$title" ]; then
    mkdir -p "$work/release"
    cp -R "$title" "$work/release/PPSA99207"
    (cd "$work/release" && COPYFILE_DISABLE=1 zip -qry -X ../release.zip PPSA99207)
    check OK "the title as make-release.sh zips it" "$work/release.zip"
    if diff -r "$work/release/PPSA99207" "$work/out" >/dev/null; then echo "PASS same files"; else echo "FAIL the unpacked files differ"; fails=$((fails + 1)); fi
    bad=$(find "$work/out" -mindepth 1 ! -perm 777 | head -3)
    if [ -z "$bad" ]; then echo "PASS every file and folder 0777"; else echo "FAIL not 0777: $bad"; fails=$((fails + 1)); fi
else
    echo "SKIP no title folder at $title"
fi

python3 - "$work" <<'PY'
import sys, zipfile, stat
w = sys.argv[1]
def make(name, entries, method=zipfile.ZIP_DEFLATED):
    with zipfile.ZipFile(f"{w}/{name}.zip", "w", method) as z:
        for e in entries:
            if isinstance(e, zipfile.ZipInfo):
                z.writestr(e, b"x")
            else:
                z.writestr(e[0], e[1])
make("good", [("PPSA99207/eboot.bin", b"elf" * 1000), ("PPSA99207/sce_sys/param.json", b"{}"), ("PPSA99207/empty", b"")])
make("stored", [("PPSA99207/a", b"stored data")], zipfile.ZIP_STORED)
make("parent", [("PPSA99207/../evil", b"x")])
make("absolute", [("/PPSA99207/x", b"x")])
make("outside", [("PPSA99207/a", b"x"), ("other/b", b"x")])
make("backslash", [("PPSA99207\\..\\x", b"x")])
make("dotpart", [("PPSA99207/./x", b"x")])
link = zipfile.ZipInfo("PPSA99207/link")
link.create_system = 3
link.external_attr = (stat.S_IFLNK | 0o777) << 16
make("symlink", [link])
fifo = zipfile.ZipInfo("PPSA99207/fifo")
fifo.create_system = 3
fifo.external_attr = (stat.S_IFIFO | 0o666) << 16
make("fifo", [fifo])
with zipfile.ZipFile(f"{w}/dup.zip", "w") as z:
    z.writestr("PPSA99207/a", b"1")
    import warnings
    warnings.simplefilter("ignore")
    z.writestr("PPSA99207/a", b"2")
# A CRC that doesn't match: change one byte of the stored data.
make("crc", [("PPSA99207/a", b"stored data here")], zipfile.ZIP_STORED)
data = bytearray(open(f"{w}/crc.zip", "rb").read())
i = data.find(b"stored data here")
data[i] ^= 1
open(f"{w}/crc.zip", "wb").write(data)
open(f"{w}/notzip.zip", "wb").write(b"not a zip at all" * 10)
PY
check OK "small deflated archive" "$work/good.zip"
check OK "stored archive" "$work/stored.zip"
check REFUSED "a ../ entry" "$work/parent.zip"
check REFUSED "an absolute path" "$work/absolute.zip"
check REFUSED "an entry outside PPSA99207/" "$work/outside.zip"
check REFUSED "a backslash path" "$work/backslash.zip"
check REFUSED "a ./ part" "$work/dotpart.zip"
check REFUSED "a symbolic link" "$work/symlink.zip"
check REFUSED "a fifo" "$work/fifo.zip"
check REFUSED "an entry twice" "$work/dup.zip"
check REFUSED "a CRC that doesn't match" "$work/crc.zip"
check REFUSED "not a zip" "$work/notzip.zip"
check REFUSED "another title's prefix" "$work/good.zip" "PPSA00001/"
check FAILED "a folder that can't be written to (not the archive's fault)" "$work/good.zip" PPSA99207/ readonly
[ "$fails" = 0 ] && echo "zip-test: all passed" || { echo "zip-test: $fails failed"; exit 1; }
