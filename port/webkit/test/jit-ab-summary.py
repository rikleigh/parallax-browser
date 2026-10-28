#!/usr/bin/env python3
"""Table for jit-ab.sh: one row per ps5wk-shot log (<site>-jit<mode>-r<n>.log)."""
import json, re, sys
from pathlib import Path

def parse(path):
    text = path.read_text(errors="replace")
    row = {"run": path.stem}
    m = re.search(r"load started at ([\d.]+) s", text)
    start = float(m.group(1)) if m else None
    m = re.search(r"RESULT ps5wk-shot load PASS .* finished at ([\d.]+) s", text)
    row["load"] = round(float(m.group(1)) - start, 1) if m and start is not None else None
    m = re.search(r"JS PERF (\{.*\})", text)
    perf = json.loads(m.group(1)) if m else {}
    row["fcp"] = perf.get("fcp")
    row["dcl"] = perf.get("dcl")
    row["onload"] = perf.get("load")
    cores = [float(c) for c in re.findall(r"\[cpu\] web\d+: ([\d.]+) cores", text)]
    row["cpu_s"] = round(sum(cores) * 5, 1)  # 5 s windows
    row["cpu_peak"] = max(cores) if cores else None
    mem = re.findall(r"\[mem\] web\d+ .*", text)
    if mem:
        last = mem[-1]
        jm = re.search(r"JIT code ([\d.]+) MB \(committed ([\d.]+)", last)
        row["jit_mb"] = jm.group(1) + "/" + jm.group(2) if jm else ("off" if "JIT off" in last else "?")
        fm = re.search(r"(\d+) failed", last)
        row["mmap_fail"] = int(fm.group(1)) if fm else None
        rm = re.search(r"peak rss ([\d.]+) MB", last)
        row["rss"] = rm.group(1) if rm else None
    drops = re.findall(r"dropped \((\d+) in all\)", text)
    row["dropped"] = int(drops[-1]) if drops else None
    late = re.findall(r"(\d+) late, ", text)
    row["late_au"] = int(late[-1]) if late else None
    renders = [float(x) for x in re.findall(r"\[render\] .* ([\d.]+) ms max", text)]
    row["render_max"] = max(renders) if renders else None
    row["crash"] = len(re.findall(r"\[crash ", text)) + len(re.findall(r"RESULT ps5wk-shot webprocess FAIL", text))
    return row

rows = [parse(p) for p in sorted(Path(sys.argv[1]).glob("*-jit*-r*.log"))]
cols = ["run", "load", "fcp", "dcl", "onload", "cpu_s", "cpu_peak", "jit_mb", "rss", "mmap_fail", "dropped", "late_au", "render_max", "crash"]
print("  ".join(c.rjust(8) if c != "run" else c.ljust(46) for c in cols))
for r in rows:
    print("  ".join((str(r.get(c)) if r.get(c) is not None else "-").rjust(8) if c != "run" else r[c].ljust(46) for c in cols))
print("load: s from load start to load finished; fcp/dcl/onload: ms (page's own timing); cpu_s: WebProcess core-seconds over the run")
