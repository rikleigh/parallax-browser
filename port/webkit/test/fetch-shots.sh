#!/usr/bin/env bash
# Fetch the title's debug screenshots (/data/ps5webkit/shots/*.ppm, written
# when /data/ps5webkit/debug-screenshot exists) over FTP and convert them to
# PNG: port/webkit/test/title-<name>.png.
#   port/webkit/test/fetch-shots.sh [prefix]     (default prefix: title-)
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
. "$ROOT/env.sh" >/dev/null
prefix="${1:-title-}"
ftp="ftp://$PS5_HOST:${PS5_FTP_PORT:-2121}/data/ps5webkit/shots/"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
for f in $(curl -sS "$ftp" | tr -d '\r' | awk '{print $NF}' | grep '\.ppm$' | sort); do
    curl -sS -o "$tmp/$f" "$ftp$f"
    out="$HERE/$prefix${f%.ppm}.png"
    python3 -I - "$tmp/$f" "$out" <<'PY'
import struct, sys, zlib
src, dst = sys.argv[1], sys.argv[2]
data = open(src, "rb").read()
parts = data.split(b"\n", 3)
assert parts[0] == b"P6", "not a binary PPM"
w, h = map(int, parts[1].split())
pix = parts[3]
assert len(pix) == w * h * 3, (len(pix), w, h)
rows = b"".join(b"\0" + pix[y * w * 3:(y + 1) * w * 3] for y in range(h))
def chunk(t, d):
    return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) \
    + chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b"")
open(dst, "wb").write(png)
print(f"{dst} {w}x{h}")
PY
done
