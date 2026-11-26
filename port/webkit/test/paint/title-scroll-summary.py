#!/usr/bin/env python3
"""title-scroll-summary.py <title run log>...: per test URL, the scroll window
(PAINT-SCROLL-START..END): rendering updates ([render1]), Mpx painted per
update, main-thread ms per update, frames on the TV per second (the
renderer's [present] lines: every swap) with the compositor's scene paints
and updates ([wc] lines) or the CPU path's page uploads, rAF/s (RAF-RATE
lines). The [wc] line's own "presents" is not read: until patch 0165 it
counted only frames that carried held update replies, which is none with
async scrolling.

The log: the log server's lines of one PPSA99207 run with test-urls.txt,
test-onload.js = scroll-raf.js and PS5WK_RENDER_LOG=2 in child.env
(COMPOSITING-PLAN.md, M2 results)."""
import re, sys

R1 = re.compile(r"\[render1\] ([\d.]+) ms: update ([\d.]+), paint ([\d.]+) \(([\d.]+) Mpx")
WC = re.compile(r"\[wc\] ([\d.]+) s: (\d+) updates .*?(\d+) paints \(")
PRES = re.compile(r"\[present\] (\d+) frames \(([\d.]+)/s\).*?; (\d+) page uploads")
RAF = re.compile(r"RAF-RATE ([\d.]+)/s( scrolling)?")
URL = re.compile(r"^test URL (\d+)/\d+: (\S+)")
TILES = re.compile(r"Mpx, (\d+) tiles")

def pct(v, q):
    v = sorted(v)
    return v[min(len(v) - 1, int(q * len(v)))] if v else 0

for path in sys.argv[1:]:
    print("##", path.split("/")[-1])
    url, phase, data = None, None, {}
    def new(u):
        return dict(url=u, up=[], wcu=0, wcp=0, wcs=0.0, pres=[], raf=[], rafidle=[], start=False, end=False, tiles=0)
    for line in open(path, errors="replace"):
        m = URL.search(line)
        if m:
            url = m.group(2)
            data[url] = new(url)
            phase = "load"
            continue
        if url is None:
            continue
        d = data[url]
        if "PAINT-SCROLL-START" in line:
            phase = "scroll"; d["start"] = True; continue
        if "PAINT-SCROLL-END" in line:
            phase = "after"; d["end"] = True; d["endline"] = line.strip()[-160:]; continue
        m = RAF.search(line)
        if m:
            (d["raf"] if m.group(2) else d["rafidle"]).append(float(m.group(1)))
        if phase != "scroll":
            continue
        m = R1.search(line)
        if m:
            d["up"].append(tuple(float(m.group(i)) for i in range(1, 5)))
            t = TILES.search(line)
            if t:
                d["tiles"] += int(t.group(1))
        m = WC.search(line)
        if m:
            d["wcs"] += float(m.group(1)); d["wcu"] += int(m.group(2)); d["wcp"] += int(m.group(3))
        m = PRES.search(line)
        if m:
            d["pres"].append((int(m.group(1)), float(m.group(2)), int(m.group(3))))
    for d in data.values():
        if not d["start"]:
            print("  %s: no scroll" % d["url"]); continue
        up = d["up"]
        n = len(up)
        tot = [u[0] for u in up]; paint = [u[2] for u in up]; mpx = [u[3] for u in up]
        zero = sum(1 for x in mpx if x < 0.005)
        # The [present] windows: frames and the seconds they cover (frames / rate).
        fr = sum(p[0] for p in d["pres"]); upl = sum(p[2] for p in d["pres"])
        secs = sum(p[0] / p[1] for p in d["pres"] if p[1]) or 1.0
        pres = ""
        if d["wcs"]:
            pres = "wc scene paints %.1f/s (updates %.1f/s)" % (d["wcp"] / d["wcs"], d["wcu"] / d["wcs"])
            if d["pres"]:
                pres += ", %.1f frames/s on the TV" % (fr / secs)
        elif d["pres"]:
            pres = "cpu page uploads %.1f/s of %.1f frames/s" % (upl / secs, fr / secs)
        raf = "rAF %.1f/s avg %.1f max (idle %.1f)" % (sum(d["raf"]) / len(d["raf"]) if d["raf"] else 0, max(d["raf"] or [0]),
                                                     sum(d["rafidle"]) / len(d["rafidle"]) if d["rafidle"] else 0)
        print("  %s: %d updates (%.1f/s); main thread %.1f ms avg p50 %.1f p95 %.1f max %.0f; paint %.1f avg p95 %.1f; Mpx/update %.3f avg p95 %.2f max %.2f, %d of %d painted nothing, %d tiles; %s; %s" % (
            d["url"][:60], n, n / 10.0, sum(tot) / max(n, 1), pct(tot, .5), pct(tot, .95), max(tot or [0]), sum(paint) / max(n, 1), pct(paint, .95),
            sum(mpx) / max(n, 1), pct(mpx, .95), max(mpx or [0]), zero, n, d["tiles"], pres, raf))
