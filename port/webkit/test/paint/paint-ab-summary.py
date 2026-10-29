#!/usr/bin/env python3
"""Summarise paint-ab.sh logs: per site and mode, the rendering updates while
the page loads (before the scroll) and during the 10 s scroll."""
import collections, glob, os, re, sys

UPDATE = re.compile(r"\[render1\] ([\d.]+) ms: update ([\d.]+), paint ([\d.]+) \(([\d.]+) Mpx(, bands)?(?:, forward [\d.]+)?(?:, record ([\d.]+), replay ([\d.]+))?\), copy ([\d.]+)")
FNV = re.compile(r"RESULT ps5wk-shot paint \w+ .*fnv64 ([0-9a-f]+)")
ASYNC = re.compile(r"\[render1\] async raster ([\d.]+) ms")
WAIT = re.compile(r"\[render\] .*frame done ([\d.]+) ms avg")

def pct(values, q):
    if not values:
        return 0.0
    values = sorted(values)
    return values[min(len(values) - 1, int(q * len(values)))]

def stats(rows, col):
    vals = [r[col] for r in rows]
    if not vals:
        return "-"
    return "%.1f/%.1f/%.1f/%.0f" % (sum(vals) / len(vals), pct(vals, .5), pct(vals, .95), max(vals))

def main(d):
    groups = collections.OrderedDict()
    for log in sorted(glob.glob(os.path.join(d, "*.log"))):
        name = os.path.basename(log)[:-4]
        m = re.match(r"(.*)-(.+)-r(\d+)$", name)
        if not m:
            continue
        site, mode = m.group(1), m.group(2)
        phase, load, scroll, waits, crash, rasters, fnv = "load", [], [], [], False, [], []
        for line in open(log, errors="replace"):
            if "PAINT-SCROLL-START" in line:
                phase = "scroll"
            elif "PAINT-SCROLL-END" in line:
                phase = "after"
            elif "[crash " in line or "RESULT ps5wk-shot webprocess FAIL" in line:
                crash = True
            u = UPDATE.search(line)
            if u:
                row = dict(total=float(u.group(1)), update=float(u.group(2)), paint=float(u.group(3)), mpx=float(u.group(4)),
                           banded=bool(u.group(5)), record=float(u.group(6) or 0), replay=float(u.group(7) or 0))
                (load if phase == "load" else scroll if phase == "scroll" else []).append(row)
            f = FNV.search(line)
            if f:
                fnv.append(f.group(1)[:8])
            a = ASYNC.search(line)
            if a and phase == "scroll":
                rasters.append(float(a.group(1)))
            w = WAIT.search(line)
            if w:
                waits.append(float(w.group(1)))
        g = groups.setdefault((site, mode), dict(load=[], scroll=[], waits=[], runs=0, crash=0, scrolled=0, rasters=[], fnv=[]))
        g["load"] += load
        g["scroll"] += scroll
        g["waits"] += waits
        g["rasters"] += rasters
        g["fnv"] += fnv
        g["runs"] += 1
        g["crash"] += crash
        g["scrolled"] += 1 if scroll else 0
    print("total/update/paint: avg/p50/p95/max ms per rendering update; scroll: updates/s over the 10 s scroll")
    print("%-34s %-6s | %5s %-22s %-22s | %6s %-22s %-22s %-22s %5s %11s" % ("site", "mode", "load#", "load total", "load paint", "scr/s", "scroll total", "scroll update", "scroll paint", "Mpx", "rec/replay"))
    for (site, mode), g in groups.items():
        sc, ld = g["scroll"], g["load"]
        rate = len(sc) / (10.0 * max(1, g["scrolled"]))
        mpx = sum(r["mpx"] for r in sc) / len(sc) if sc else 0
        bands = [r for r in sc if r["banded"]]
        rr = "%.1f/%.1f" % (sum(r["record"] for r in bands) / len(bands), sum(r["replay"] for r in bands) / len(bands)) if bands else "-"
        if g["rasters"]:
            rr = "%.1f/a%.1f" % (sum(r["record"] for r in bands) / max(1, len(bands)), sum(g["rasters"]) / len(g["rasters"]))
        print("%-34s %-6s | %5d %-22s %-22s | %6.1f %-22s %-22s %-22s %5.2f %11s %s%s" % (site[:34], mode, len(ld), stats(ld, "total"), stats(ld, "paint"),
            rate, stats(sc, "total"), stats(sc, "update"), stats(sc, "paint"), mpx, rr, ",".join(g["fnv"]), "  CRASH" if g["crash"] else ""))

if __name__ == "__main__":
    main(sys.argv[1])
