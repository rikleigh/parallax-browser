#!/usr/bin/env python3
"""One title run's numbers from a slice of the title log (tools/logserver.sh).
   parse-run.py <log> <start byte offset>   -> one line of key=value
Fields: title pid; children launched / connected; median elfldr send ms of
the 130 MB children; tile-upload throughput (MB per ms, windows with >= 20 MB)
and small-tile ms per upload (tiles < 0.15 MB); WebProcess raster ms per
megapixel (first [render] wc tiles window with >= 0.5 Mpx/s); quiesce and
wind-down lines (CONSOLE-HEALTH.md)."""
import re, sys, statistics as st
path, off = sys.argv[1], int(sys.argv[2])
with open(path, 'rb') as f:
    f.seek(off)
    text = f.read().decode('utf-8', 'replace')
lines = text.splitlines()
starts = [i for i, l in enumerate(lines) if 'ps5browser: WPE WebKit' in l]
if not starts:
    print('run=none'); sys.exit(0)
lines = lines[starts[-1]:]
pid = re.search(r'pid (\d+)', lines[0])[1]
sends, launched, connected, thr, small, rast = [], 0, 0, [], [], []
quiesce = winddown = brake = ''
for l in lines:
    if '[ps5-launch] launching ' in l: launched += 1
    if re.search(r'process \d+ is payload pid \d+', l): connected += 1
    m = re.search(r'sent (\d+) bytes \+ header to elfldr in (\d+) ms', l)
    if m and int(m[1]) > 1_000_000: sends.append(int(m[2]))
    m = re.search(r'^\[wc\] [\d.]+ s: .*tiles uploaded (\d+) \(\d+ draws, ([\d.]+) MB, ([\d.]+) ms, ([\d.]+) ms per upload', l)
    if m:
        n, mb, ms = int(m[1]), float(m[2]), float(m[3])
        if mb >= 20 and ms > 0: thr.append(mb / ms)
        if n >= 10 and mb / n < 0.15: small.append(float(m[4]))
    m = re.search(r'\[render\] wc tiles: record [\d.]+ ms/s \([\d.]+ Mpx/s\), raster ([\d.]+) ms/s wall \(([\d.]+) Mpx/s', l)
    if m and float(m[2]) >= 0.5: rast.append(float(m[1]) / float(m[2]))
    if 'quiesced:' in l: quiesce = l.split('quiesced:')[1].strip().replace(' ', '_')
    if 'wind-down: the WebProcess ended' in l: winddown = 'noreload'
    if 'terminated (reason 2); reloading' in l: winddown = 'RELOADED'
    if 'not starting another' in l: brake = 'BRAKE'
med = lambda v, f='%.2f': (f % st.median(v)) if v else '-'
print(f'pid={pid} launched={launched} connected={connected} send_ms={med(sends, "%.0f")} sends={",".join(map(str, sends))} '
      f'upload_MB_per_ms={med(thr)} small_tile_ms={med(small, "%.3f")} raster_ms_per_Mpx={med(rast)} n_raster={len(rast)} '
      f'winddown={winddown or "-"} quiesce={quiesce or "-"} {brake}')
