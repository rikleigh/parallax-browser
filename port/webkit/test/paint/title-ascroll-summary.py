#!/usr/bin/env python3
"""title-ascroll-summary.py <title run log>...: async scrolling summary (COMPOSITING-PLAN.md, "Async scrolling").

Per run: seconds with page motion, frames that moved the page per second
(avg over seconds with >= 20 pad events), pad -> present latency (pooled from
the per-second p50/p95/max: avg of avgs, max of p95s, max), WebProcess
updates/s in the same seconds ([wc] lines), wheel routing, clamps, lead,
SJ scroll events/s and lazy loads, crashes."""
import re
import sys

ASCROLL = re.compile(r"\[ascroll\] ([\d.]+) s: (\d+) frames moved the page \((\d+) by WebProcess updates, (\d+) by the title\); (\d+) pad wheel events; "
                     r"pad -> present ([\d.]+) ms avg, p50 ([\d.]+), p95 ([\d.]+), max ([\d.]+) \((\d+), (\d+) stale\); step ([\d.]+) px avg, ([\d.]+) max")
ROUTE = re.compile(r"\[ascroll\] ([\d.]+) s: wheel events in the title (\d+), sent to the WebProcess first (\d+); clamped at the painted edge (\d+) \((\d+) px\); "
                   r"lead over the WebProcess ([\d.]+) px avg, ([\d.]+) max; (\d+) tree commits")
WC = re.compile(r"\[wc\] ([\d.]+) s: (\d+) updates .*?(\d+) presents;.*?(\d+) paints \(([\d.]+) ms avg")
SJ = re.compile(r"SJ [\d.]+ s: scroll events (\d+), rAF (\d+), scrollY (\S+), long tasks (\d+), lazy loaded (\d+), prevented (\d+)")


def summarize(path):
    lines = open(path, errors="replace").read().splitlines()
    moved, lat_avg, lat_p95, lat_max, inputs, steps = [], [], [], [], [], []
    wc_updates, wc_paints, sj_scroll, sj_raf = [], [], [], []
    routes = [0, 0, 0, 0]
    lead_max = 0.0
    lazy = prevented = 0
    by_update = by_title = 0
    pending_scroll_second = False
    for line in lines:
        m = ASCROLL.search(line)
        if m:
            secs = float(m.group(1))
            n_in = int(m.group(5))
            pending_scroll_second = n_in >= 20
            if pending_scroll_second:
                moved.append(int(m.group(2)) / secs)
                by_update += int(m.group(3))
                by_title += int(m.group(4))
                inputs.append(n_in / secs)
                if int(m.group(10)):
                    lat_avg.append(float(m.group(6)))
                    lat_p95.append(float(m.group(8)))
                    lat_max.append(float(m.group(9)))
                steps.append(float(m.group(12)))
            continue
        m = ROUTE.search(line)
        if m:
            routes[0] += int(m.group(2))
            routes[1] += int(m.group(3))
            routes[2] += int(m.group(4))
            routes[3] += int(m.group(5))
            lead_max = max(lead_max, float(m.group(7)))
            continue
        m = WC.search(line)
        if m and pending_scroll_second:
            secs = float(m.group(1))
            wc_updates.append(int(m.group(2)) / secs)
            wc_paints.append(float(m.group(5)))
            continue
        m = SJ.search(line)
        if m:
            if int(m.group(1)):
                sj_scroll.append(int(m.group(1)))
                sj_raf.append(int(m.group(2)))
            lazy += int(m.group(5))
            prevented += int(m.group(6))

    def avg(v):
        return sum(v) / len(v) if v else 0.0
    crashes = sum(1 for l in lines if "crash" in l.lower() and ("webprocess" in l.lower() or "abort" in l.lower()))
    print(f"{path}: {len(moved)} scrolling seconds; moved frames/s {avg(moved):.1f} (by updates {by_update}, by the title {by_title}); "
          f"pad events/s {avg(inputs):.0f}; pad->present avg {avg(lat_avg):.1f} ms, p95 max {max(lat_p95 or [0]):.1f}, max {max(lat_max or [0]):.1f}; "
          f"step {avg(steps):.1f} px; WebProcess updates/s {avg(wc_updates):.1f}, scene paint {avg(wc_paints):.2f} ms; "
          f"wheel title/WebProcess {routes[0]}/{routes[1]}, clamped {routes[2]} ({routes[3]} px), lead max {lead_max:.0f}; "
          f"SJ scroll events/s {avg(sj_scroll):.1f}, rAF/s {avg(sj_raf):.1f}, lazy {lazy}, prevented {prevented}; crash-ish lines {crashes}")


for p in sys.argv[1:]:
    summarize(p)
