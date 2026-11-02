#!/usr/bin/env python3
"""Where scrolllinked.html's bars are in pad-scroll-test captures (P6 .ppm).

The magenta bar (moved from rAF) and the cyan bar (moved from scroll events)
belong 300 CSS px below the top of the view; in a capture at the TV's scale
(960x540 for a 1920x1080 view) that is row 150. The offset of the bar's top
row from there is how far behind (or ahead of) the presented scroll position
the WebProcess's frame was made, in capture px (x2 for CSS px).
Usage: scrolllinked-shots.py <ppm or dir>..."""
import os
import sys


def read_ppm(path):
    with open(path, 'rb') as f:
        data = f.read()
    parts = []
    i = 0
    while len(parts) < 4:
        while data[i:i + 1].isspace():
            i += 1
        if data[i:i + 1] == b'#':
            while data[i:i + 1] != b'\n':
                i += 1
            continue
        j = i
        while not data[j:j + 1].isspace():
            j += 1
        parts.append(data[i:j])
        i = j
    w, h = int(parts[1]), int(parts[2])
    return w, h, data[i + 1:]


def bar_row(w, h, px, x0, x1, want):
    for y in range(h):
        hits = 0
        n = 0
        for x in range(x0, x1, 8):
            o = (y * w + x) * 3
            r, g, b = px[o], px[o + 1], px[o + 2]
            n += 1
            if want(r, g, b):
                hits += 1
        if hits > n * 0.8:
            return y
    return None


def main():
    paths = []
    for a in sys.argv[1:]:
        if os.path.isdir(a):
            paths += sorted(os.path.join(a, f) for f in os.listdir(a) if f.endswith('.ppm'))
        else:
            paths.append(a)
    offs = []
    for p in paths:
        w, h, px = read_ppm(p)
        scale = h / 540.0
        expected = round(150 * scale)
        mag = bar_row(w, h, px, 8, w // 2 - 8, lambda r, g, b: r > 200 and g < 80 and b > 200)
        cyan = bar_row(w, h, px, w // 2 + 8, w - 8, lambda r, g, b: r < 80 and g > 200 and b > 200)
        m = None if mag is None else mag - expected
        c = None if cyan is None else cyan - expected
        if m is not None:
            offs.append(abs(m))
        print('%s: rAF bar %s, scroll-event bar %s (capture px from row %d)' % (os.path.basename(p), 'missing' if m is None else '%+d' % m, 'missing' if c is None else '%+d' % c, expected))
    if offs:
        print('rAF bar: %d captures, |offset| avg %.1f, max %d capture px; %d at 0' % (len(offs), sum(offs) / len(offs), max(offs), sum(1 for o in offs if o == 0)))


main()
