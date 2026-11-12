#!/usr/bin/env python3
"""Feed scroll benchmark summary up to the point the feed stopped growing.
Per run: distance, held seconds, moving fps, stalled share, and engine costs per 1000 px of feed."""
import re, sys
def run(log):
    lines = open(log, errors='replace').read().splitlines()
    secs = []  # (line index, moved, pad, cant, step, clamp, secs)
    for i, line in enumerate(lines):
        m = re.match(r'\[ascroll\] ([\d.]+) s: wheel events in the title (\d+), .*?clamped at the painted edge (\d+)', line)
        if m:
            cur = dict(i=i, secs=float(m[1]), clamp=int(m[3]), cant=int(re.search(r'root could not take (\d+)', line)[1]), step=float(re.search(r'per-frame step ([\d.]+) px avg', line)[1]), moved=0, pad=0)
            secs.append(cur); continue
        m = re.match(r'\[ascroll\] ([\d.]+) s: (\d+) frames moved the page.*?; (\d+) pad wheel events', line)
        if m and secs: secs[-1]['moved'] = int(m[2]); secs[-1]['pad'] = int(m[3])
    held = [s for s in secs if s['secs'] <= 1.5 and s['pad'] / s['secs'] >= 90]
    moving = [s for s in held if s['moved'] > 0]
    if not moving: return None
    end = moving[-1]['i']; start = held[0]['i']
    held = [s for s in held if s['i'] <= end]
    px = sum(s['moved'] * s['step'] for s in held)
    fps = sum(s['moved'] / s['secs'] for s in held) / len(held)
    stalled = sum(1 for s in held if s['cant'] >= 0.5 * s['pad']) / len(held)
    smooth = sum(1 for s in held if s['moved'] / s['secs'] >= 50) / len(held)
    rec = mpx = cpu = upd = updms = comp = compms = trav = 0.0
    lateu = []; latefps = []
    layers = 0
    for i in range(start, end + 1):
        line = lines[i]
        m = re.search(r'\[render\] wc tiles: record ([\d.]+) ms/s \(([\d.]+) Mpx/s\)', line)
        if m: rec += float(m[1]) * 5; mpx += float(m[2]) * 5
        m = re.search(r'\[cpu\] web\d+: ([\d.]+) cores', line)
        if m: cpu += float(m[1]) * 5
        m = re.search(r'\[render\] wc: (\d+) updates in ([\d.]+) s \(([\d.]+)/s\), ([\d.]+) ms average', line)
        if m: upd += int(m[1]); updms += int(m[1]) * float(m[4])
        m = re.search(r'\[render\] compositing: (\d+) updates in [\d.]+ s, ([\d.]+) ms avg.*?\((\d+) with a requirements', line)
        if m: comp += int(m[1]); compms += int(m[1]) * float(m[2]); trav += int(m[3])
        m = re.match(r'\[wc\] [\d.]+ s: \d+ updates \(.*?(\d+) layers', line)
        if m: layers = max(layers, int(m[1]))
    k = px / 1000
    # late phase: the second half of the distance
    acc = 0; late = []
    for s in held:
        acc += s['moved'] * s['step']
        if acc > px / 2: late.append(s)
    lfps = sum(s['moved'] / s['secs'] for s in late) / max(1, len(late))
    return dict(px=px, held=len(held), fps=fps, stalled=stalled, smooth=smooth, lfps=lfps, rec=rec / k, mpx=mpx / k, cpu=cpu / k, updms=updms / k, upd=upd / k, compms=compms / k, trav=trav / k, layers=layers)
print('| run | feed px | held s | moving fps | 2nd-half fps | stalled | >=50 fps secs | per 1000 px: WP CPU s | update ms | paint record ms | Mpx | compositing ms (traversals) | max layers |')
print('|' + ' --- |' * 13)
for log in sys.argv[1:]:
    r = run(log)
    name = log.split('/')[-1].replace('.log', '')
    if not r: print('| %s | no movement |' % name); continue
    print('| %s | %.0f | %d | %.1f | %.1f | %.0f%% | %.0f%% | %.2f | %.0f | %.0f | %.1f | %.0f (%.1f) | %d |' % (name, r['px'], r['held'], r['fps'], r['lfps'], 100 * r['stalled'], 100 * r['smooth'], r['cpu'], r['updms'], r['rec'], r['mpx'], r['compms'], r['trav'], r['layers']))
