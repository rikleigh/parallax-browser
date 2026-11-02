#!/usr/bin/env python3
"""Long-feed cost over time from a title log (COMPOSITING-PLAN.md, "Long feeds").

  feed-summary.py <title log> [--from LINE] [--buckets 500,1000,2000]

One row per WebProcess [render] window (5 s): feed items (feedlayers.html's
FEED lines, if any), composited layers and scene paint ms (the title's [wc]
lines), WebProcess updates/s and ms per update (avg, p95, the style/layout/
script part), the layer tree flush (ms, layers walked and culled), title
scroll updates applied (ms), compositing updates (ms), DOM wheel events sent
and their round trip, moving frames/s and clamped steps ([ascroll]).
With --buckets, averages of the rows whose item count is closest to each
bucket (within 25 %).
"""
import argparse, re, sys

ap = argparse.ArgumentParser()
ap.add_argument('log')
ap.add_argument('--from', dest='start', type=int, default=0, help='first line (1-based)')
ap.add_argument('--buckets', default='')
args = ap.parse_args()

lines = open(args.log, errors='replace').read().splitlines()[max(0, args.start - 1):]

def num(pattern, text, cast=float):
    m = re.search(pattern, text)
    return cast(m.group(1)) if m else None

rows = []
state = dict(items=None, layers=None, scene=[], moved=[], clamped=0, cull=[], culledLayers=[])
pending = {}
for line in lines:
    if 'CONSOLE LOG FEED ' in line:
        v = num(r'items (\d+)', line, int)
        if v is not None:
            state['items'] = v
    elif re.match(r'^\[wc\] [\d.]+ s: \d+ updates \(', line):
        state['layers'] = num(r'(\d+) layers', line, int)
        p = num(r'\d+ paints \(([\d.]+) ms avg', line)
        if p is not None and num(r'(\d+) paints', line, int):
            state['scene'].append(p)
    elif line.startswith('[wc] scene per paint:'):
        v = num(r'culled ([\d.]+) subtrees, ([\d.]+) layers', line)
        c = re.search(r'culled ([\d.]+) subtrees, ([\d.]+) layers', line)
        if c:
            state['culledLayers'].append(float(c.group(2)))
    elif re.match(r'^\[ascroll\] [\d.]+ s: \d+ frames moved', line):
        secs = num(r'^\[ascroll\] ([\d.]+) s', line)
        moved = num(r': (\d+) frames moved', line, int)
        pad = num(r'(\d+) pad wheel events', line, int)
        if pad and pad >= 20 and secs:
            state['moved'].append(moved / secs)
    elif re.match(r'^\[ascroll\] [\d.]+ s: wheel events', line):
        c = num(r'clamped at the painted edge (\d+)', line, int)
        if c:
            state['clamped'] += c
    elif '[ascroll] DOM wheel events:' in line:
        pending['dom'] = num(r'(\d+) sent', line, int)
        pending['domRT'] = num(r'round trip ([\d.]+) ms avg', line)
    elif '[render] wc flush:' in line:
        pending['flush'] = num(r'flushes, ([\d.]+) ms avg', line)
        pending['walked'] = num(r'([\d.]+) walked', line)
        pending['skipped'] = num(r'([\d.]+) skipped', line)
    elif '[render] wc scroll:' in line:
        pending['scrollApply'] = num(r'applying them ([\d.]+) ms avg', line)
    elif '[render] compositing:' in line:
        pending['comp'] = num(r'([\d.]+) ms avg', line)
        pending['compN'] = num(r'(\d+) updates in', line, int)
    elif '[render] wc:' in line:
        row = dict(
            ups=num(r'\(([\d.]+)/s\)', line), avg=num(r'([\d.]+) ms average', line), p95=num(r'p95 ([\d.]+)', line),
            update=num(r'update ([\d.]+) avg', line), items=state['items'], layers=state['layers'],
            scene=sum(state['scene']) / len(state['scene']) if state['scene'] else None,
            moved=sum(state['moved']) / len(state['moved']) if state['moved'] else None,
            clamped=state['clamped'],
            culled=sum(state['culledLayers']) / len(state['culledLayers']) if state['culledLayers'] else None)
        rows.append(row)
        pending = {}
        rows[-1]['_pending'] = pending  # filled by the lines that follow
        state['scene'], state['moved'], state['clamped'], state['culledLayers'] = [], [], 0, []

def f(v, fmt='%.1f'):
    return '' if v is None else fmt % v

print('| items | layers | WP upd/s | ms avg | p95 | update | flush ms | walked | culled | scroll apply | compositing ms (n) | DOM wheel n, rt | scene ms | scene culled | moving fps | clamped |')
print('| ' + ' | '.join(['---'] * 16) + ' |')
for r in rows:
    p = r['_pending']
    print('| ' + ' | '.join([
        f(r['items'], '%d'), f(r['layers'], '%d'), f(r['ups']), f(r['avg']), f(r['p95']), f(r['update']),
        f(p.get('flush'), '%.2f'), f(p.get('walked'), '%.0f'), f(p.get('skipped'), '%.0f'), f(p.get('scrollApply'), '%.2f'),
        (f(p.get('comp')) + (' (%d)' % p['compN'] if p.get('compN') else '')) if p.get('comp') is not None else '',
        ('%d, %.0f' % (p['dom'], p['domRT'] or 0)) if p.get('dom') else '',
        f(r['scene']), f(r['culled'], '%.0f'), f(r['moved']), str(r['clamped'] or '')]) + ' |')

if args.buckets:
    print()
    print('| bucket | windows | layers | WP upd/s | ms avg | p95 | scene ms | moving fps |')
    print('| ' + ' | '.join(['---'] * 8) + ' |')
    for b in [int(x) for x in args.buckets.split(',')]:
        sel = [r for r in rows if r['items'] and abs(r['items'] - b) <= 0.25 * b]
        if not sel:
            continue
        def avg(k):
            v = [r[k] for r in sel if r[k] is not None]
            return sum(v) / len(v) if v else None
        print('| %d | %d | %s | %s | %s | %s | %s | %s |' % (b, len(sel), f(avg('layers'), '%.0f'), f(avg('ups')), f(avg('avg')), f(avg('p95')), f(avg('scene')), f(avg('moved'))))
