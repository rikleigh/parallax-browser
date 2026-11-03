#!/usr/bin/env python3
"""Parallax Browser: the logo for the UI, made from sce_sys/icon0.png.

The icon is the mark on its near-black navy (#01001A). Here the background
becomes transparency (colour to alpha against that navy: the panes are
light on dark, so each pixel is the navy plus the pane's light), cropped to
the panes and scaled down by area averaging.

  brand-assets.py mark <size> <out.bgra>   for the title's canvas (browserdraw.c):
                                           "PXMK", width, height (u32 LE), then
                                           premultiplied 0xAARRGGBB words (LE)
  brand-assets.py png <size> <out.png>     RGBA PNG (the start page's logo)

Reads the icon through sips (macOS) as a BMP; no Python packages needed.
"""
import os
import struct
import subprocess
import sys
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ICON = os.path.join(HERE, "sce_sys", "icon0.png")
BG = (0x01, 0x00, 0x1A)       # the icon's background
CROP = (76, 72, 360, 360)     # x, y, w, h: the panes and their glow, square


def read_icon():
    with tempfile.TemporaryDirectory() as tmp:
        bmp = os.path.join(tmp, "icon.bmp")
        subprocess.run(["sips", "-s", "format", "bmp", ICON, "--out", bmp], check=True, stdout=subprocess.DEVNULL)
        data = open(bmp, "rb").read()
    offset = struct.unpack_from("<I", data, 10)[0]
    width, height = struct.unpack_from("<ii", data, 18)
    bpp = struct.unpack_from("<H", data, 28)[0] // 8
    top_down = height < 0
    height = abs(height)
    stride = (width * bpp + 3) & ~3
    rgb = []
    for y in range(height):
        row = offset + (y if top_down else height - 1 - y) * stride
        rgb.append([(data[row + x * bpp + 2], data[row + x * bpp + 1], data[row + x * bpp]) for x in range(width)])
    return rgb


def unblend(rgb):
    """Premultiplied RGBA floats (0-1) of the crop, the navy taken out."""
    x0, y0, w, h = CROP
    out = []
    for y in range(y0, y0 + h):
        row = []
        for x in range(x0, x0 + w):
            c = rgb[y][x]
            a = max(max(0, c[i] - BG[i]) / (255 - BG[i]) for i in range(3))
            a = min(1.0, a)
            # c = a * fg + (1 - a) * bg  ->  a * fg (premultiplied)
            row.append(tuple(max(0.0, (c[i] - (1 - a) * BG[i]) / 255) for i in range(3)) + (a,))
        out.append(row)
    return out


def scale(img, size):
    """Area average of premultiplied pixels to size x size."""
    n = len(img)
    f = n / size
    out = []
    for oy in range(size):
        sy0, sy1 = oy * f, (oy + 1) * f
        row = []
        for ox in range(size):
            sx0, sx1 = ox * f, (ox + 1) * f
            acc = [0.0, 0.0, 0.0, 0.0]
            total = 0.0
            for y in range(int(sy0), min(n, int(sy1 + 0.9999))):
                wy = min(sy1, y + 1) - max(sy0, y)
                for x in range(int(sx0), min(n, int(sx1 + 0.9999))):
                    wgt = wy * (min(sx1, x + 1) - max(sx0, x))
                    p = img[y][x]
                    for k in range(4):
                        acc[k] += p[k] * wgt
                    total += wgt
            row.append(tuple(v / total for v in acc))
        out.append(row)
    return out


def write_mark(img, path):
    size = len(img)
    words = bytearray(b"PXMK" + struct.pack("<II", size, size))
    for row in img:
        for r, g, b, a in row:
            q = [min(255, int(v * 255 + 0.5)) for v in (r, g, b, a)]
            q[0], q[1], q[2] = (min(q[3], v) for v in q[:3])  # premultiplied: never over alpha
            words += struct.pack("<I", (q[3] << 24) | (q[0] << 16) | (q[1] << 8) | q[2])
    open(path, "wb").write(words)


def write_png(img, path):
    size = len(img)
    raw = bytearray()
    previous = bytes(size * 4)
    for row in img:
        line = bytearray()
        for r, g, b, a in row:
            if a < 3 / 255:
                r = g = b = a = 0.0  # the background's noise
            elif a > 0:
                r, g, b = (min(1.0, v / a) for v in (r, g, b))  # PNG is not premultiplied
            line += bytes(min(255, int(v * 255 + 0.5)) for v in (r, g, b, a))
        raw.append(2)  # "Up" filter: smaller
        raw += bytes((line[i] - previous[i]) & 0xFF for i in range(len(line)))
        previous = line

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b"")
    open(path, "wb").write(png)


def main():
    if len(sys.argv) != 4 or sys.argv[1] not in ("mark", "png"):
        sys.exit(__doc__)
    kind, size, path = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    img = scale(unblend(read_icon()), size)
    (write_mark if kind == "mark" else write_png)(img, path)


if __name__ == "__main__":
    main()
