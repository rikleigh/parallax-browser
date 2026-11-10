#!/usr/bin/env python3
"""title-anim-summary.py <title run log>...: per phase of animations.html?auto
(ANIM-PHASE idle / busy / idle2 markers): rendering updates per second and
main-thread ms per update ([render1]), Mpx painted per update, the scene's
paints per second and how many of them only ran accelerated animations, the
longest gap between two scene paints ([wc] lines), presents/s, and the page's
own ANIM-STATS line (rAF rate and longest rAF gap).

The log: the log server's lines of one PPSA99207 run with test-urls.txt =
file:///data/ps5webkit/paint/animations.html?auto and PS5WK_RENDER_LOG=2 in
child.env (COMPOSITING-PLAN.md, M3 results)."""
import re, sys

R1 = re.compile(r"\[render1\] ([\d.]+) ms: update ([\d.]+), paint ([\d.]+) \(([\d.]+) Mpx")
WC = re.compile(r"\[wc\] ([\d.]+) s: (\d+) updates .*?, (\d+) presents.*?(\d+) paints \(([\d.]+) ms avg, ([\d.]+) max; (\d+) for animations only, longest gap ([\d.]+) ms(?:, (\d+) waited for)?\)")
PHASE = re.compile(r"ANIM-PHASE (\w+)")
STATS = re.compile(r"ANIM-STATS (.*)")

def pct(v, q):
    v = sorted(v)
    return v[min(len(v) - 1, int(q * len(v)))] if v else 0

for path in sys.argv[1:]:
    print("##", path.split("/")[-1])
    phase, data, order = None, {}, []
    for line in open(path, errors="replace"):
        m = PHASE.search(line)
        if m:
            phase = m.group(1)
            if phase != "end" and phase not in data:
                data[phase] = dict(up=[], wc=[], stats="")
                order.append(phase)
            continue
        m = STATS.search(line)
        if m:
            name = m.group(1).split(":")[0]
            if name in data:
                data[name]["stats"] = m.group(1).strip()
            continue
        if phase not in data:
            continue
        m = R1.search(line)
        if m:
            data[phase]["up"].append(tuple(float(m.group(i)) for i in range(1, 5)))
        m = WC.search(line)
        if m:
            data[phase]["wc"].append((float(m.group(1)), int(m.group(2)), int(m.group(3)), int(m.group(4)), int(m.group(7)), float(m.group(8))))
    for name in order:
        d = data[name]
        up, wc = d["up"], d["wc"]
        seconds = sum(w[0] for w in wc) or 1
        total = [u[0] for u in up]
        mpx = [u[3] for u in up]
        print(f"  {name}: {len(up) / seconds:.1f} updates/s; main thread {sum(total) / max(1, len(total)):.1f} ms avg p95 {pct(total, .95):.1f} max {max(total or [0]):.1f}; "
              f"{sum(mpx) / max(1, len(mpx)):.3f} Mpx/update avg; scene paints {sum(w[3] for w in wc) / seconds:.1f}/s "
              f"({sum(w[4] for w in wc) / seconds:.1f}/s animation only), longest paint gap {max([w[5] for w in wc] or [0]):.1f} ms; presents {sum(w[2] for w in wc) / seconds:.1f}/s")
        if d["stats"]:
            print(f"    page: {d['stats']}")
