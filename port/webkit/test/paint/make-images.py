#!/usr/bin/env python3
"""Generate the synthetic thumbnails for images.html (COMPOSITING-PLAN.md,
TV test 2): N 640x360 JPEGs (~90 KB each) with a
different colour, gradient and shapes each, so a capture shows which have
been drawn. Pure Python PNG, converted to JPEG with macOS `sips`.

    port/webkit/test/paint/make-images.py <outdir> [count, default 120]
    tools/serve-media.py --port 8775 --page port/webkit/test/paint/images.html <outdir>
    -> http://<mac>:8775/images.html
"""
import os
import random
import struct
import subprocess
import sys
import zlib

W, H = 640, 360


def png(path, rows):
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    raw = b"".join(b"\0" + r for r in rows)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 1)) + chunk(b"IEND", b""))


def image(index):
    rnd = random.Random(index)
    base = [rnd.randrange(40, 220) for _ in range(3)]
    other = [rnd.randrange(40, 220) for _ in range(3)]
    shapes = [(rnd.randrange(W), rnd.randrange(H), rnd.randrange(30, 140), [rnd.randrange(256) for _ in range(3)]) for _ in range(6)]
    noise = bytes(rnd.randrange(24) for _ in range(W * 3 + 97))
    rows = []
    for y in range(H):
        t = y / H
        row = bytearray(W * 3)
        for x in range(0, W, 4):
            u = x / W
            c = [int(base[i] * (1 - u) * (1 - t) + other[i] * (u + t) / 2) for i in range(3)]
            for cx, cy, r, col in shapes:
                if (x - cx) * (x - cx) + (y - cy) * (y - cy) < r * r:
                    c = col
                    break
            px = bytes(min(255, max(0, v)) for v in c) * 4
            row[x * 3:x * 3 + 12] = px
        off = (y * 7) % 97
        rows.append(bytes((row[i] + noise[off + i] - 12) & 255 for i in range(W * 3)))
    return rows


def main():
    out = sys.argv[1]
    count = int(sys.argv[2]) if len(sys.argv) > 2 else 120
    os.makedirs(out, exist_ok=True)
    for i in range(count):
        jpg = os.path.join(out, "thumb-%03d.jpg" % i)
        if os.path.exists(jpg):
            continue
        tmp = os.path.join(out, "tmp-%03d.png" % i)
        png(tmp, image(i))
        subprocess.run(["sips", "-s", "format", "jpeg", "-s", "formatOptions", "80", tmp, "--out", jpg],
                       check=True, stdout=subprocess.DEVNULL)
        os.remove(tmp)
    print("%d thumbnails in %s" % (count, out))


if __name__ == "__main__":
    main()
