#!/usr/bin/env python3
"""shot-diff.py <dir A> <dir B> [glob]: compare the title's debug screenshots
(PPM, /data/ps5webkit/shots, debug-screenshot knob) of two runs with the same
test-urls.txt, e.g. the CPU path and the WC path (COMPOSITING-PLAN.md, M1/M3).
For each file name in both directories: the share of pixels whose largest
channel difference is over 24 and over 64, and the bounding box of the
latter. Optional glob (default *.ppm), e.g. '*-10s.ppm'."""
import fnmatch, os, sys

def load(path):
    data = open(path, "rb").read()
    parts = data.split(b"\n", 3)
    assert parts[0] == b"P6", path
    w, h = map(int, parts[1].split())
    return w, h, parts[3]

a, b = sys.argv[1], sys.argv[2]
pattern = sys.argv[3] if len(sys.argv) > 3 else "*.ppm"
for name in sorted(set(os.listdir(a)) & set(os.listdir(b))):
    if not fnmatch.fnmatch(name, pattern):
        continue
    wa, ha, pa = load(os.path.join(a, name))
    wb, hb, pb = load(os.path.join(b, name))
    if (wa, ha) != (wb, hb):
        print(f"{name}: sizes differ {wa}x{ha} vs {wb}x{hb}")
        continue
    over24 = over64 = 0
    x0, y0, x1, y1 = wa, ha, -1, -1
    for i in range(0, len(pa), 3):
        d = max(abs(pa[i] - pb[i]), abs(pa[i + 1] - pb[i + 1]), abs(pa[i + 2] - pb[i + 2]))
        if d > 24:
            over24 += 1
            if d > 64:
                over64 += 1
                p = i // 3
                x, y = p % wa, p // wa
                x0, y0, x1, y1 = min(x0, x), min(y0, y), max(x1, x), max(y1, y)
    n = wa * ha
    box = f", > 64 within x {x0}-{x1} y {y0}-{y1}" if over64 else ""
    print(f"{name}: > 24 {100.0 * over24 / n:.2f} %, > 64 {100.0 * over64 / n:.2f} %{box}")
