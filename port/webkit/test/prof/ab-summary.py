#!/usr/bin/env python3
"""A/B summary of prof-run.sh runs: one line per run, then per (page, tag)
medians.

  ab-summary.py [--elf ps5wk-child] <prof-run output dir>...

Per run (<name>.log plus the <name>/ profiles): the last load's finish time
(ps5wk-shot) and its PAGE load/FCP (page.js); from the WebProcess profile
(prof.py): main-thread CPU, process CPU, compiles per tier and the JIT
threshold multiplier; from the WebProcess's [mem] lines (EXTRA_ENV
PS5WK_MEM_LOG=5): peak JIT code, JIT committed, pool size, peak libpas
committed and peak rss; from the NetworkProcess profile: its main-thread CPU
and the share of it in libsoup's HSTS code (stacks naming "hsts").
Runs are grouped by the name without its -cold/-warm part (TAG=... in
prof-run.sh names a configuration).
"""
import argparse, collections, os, re, statistics, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))


def prof(files, elf):
    with tempfile.NamedTemporaryFile(suffix='.folded') as folded:
        out = subprocess.run([sys.executable, os.path.join(HERE, 'prof.py'), '--elf', elf, '--threads', '1', '--folded', folded.name] + files,
                             capture_output=True, text=True).stdout
        stacks = open(folded.name).read().splitlines()
    return out, stacks


def run_summary(d, name, elf):
    r = {'name': name}
    log = open(os.path.join(d, name + '.log'), errors='replace').read()
    fin = re.findall(r'^load finished at ([\d.]+) s', log, re.M)
    r['finished'] = float(fin[-1]) if fin else None
    r['loads'] = len(fin)
    page = re.findall(r'PAGE ttfb=\d+ \S+ dcl=(\d+) load=(\d+) fp=\S+ fcp=(\d+|-)', log)
    if page:
        r['dcl'], r['load'] = int(page[-1][0]), int(page[-1][1])
        r['fcp'] = int(page[-1][2]) if page[-1][2] != '-' else None
    mems = [l for l in log.splitlines() if '[mem] web' in l and ' at ' in l]
    def peak(rx):
        v = [float(m.group(1)) for l in mems for m in [re.search(rx, l)] if m]
        return max(v) if v else None
    r['jit_mb'] = peak(r'JIT code ([\d.]+) MB')
    r['jit_committed'] = peak(r'committed ([\d.]+) of [\d.]+ MB\)')
    r['pool'] = peak(r'committed [\d.]+ of ([\d.]+) MB\)')
    r['pas_committed'] = peak(r'libpas pages [\d.]+ MB \([\d.]+ given back, ([\d.]+) committed\)')
    r['rss_peak'] = peak(r'peak rss ([\d.]+) MB')
    r['mult_mem'] = peak(r'thresholds x([\d.]+)')
    pdir = os.path.join(d, name)
    files = sorted(os.listdir(pdir)) if os.path.isdir(pdir) else []
    web = [os.path.join(pdir, f) for f in files if f.startswith('web') and f.endswith('.prof')]
    net = [os.path.join(pdir, f) for f in files if f.startswith('network') and f.endswith('.prof')]
    if web:
        out, _ = prof(web, elf)
        m = re.search(r'^\s+main \(WebKit\)\s+(\d+) ms CPU', out, re.M)
        r['main_cpu'] = int(m.group(1)) / 1000 if m else None
        m = re.search(r'^process CPU (\d+) ms', out, re.M)
        r['proc_cpu'] = int(m.group(1)) / 1000 if m else None
        m = re.search(r'compiles fail/Baseline/DFG/FTL/cancel (\d+)/(\d+)/(\d+)/(\d+)/(\d+).*threshold multiplier ([\d.]+)', out)
        if m:
            r['failed'], r['baseline'], r['dfg'], r['ftl'] = (int(m.group(i)) for i in range(1, 5))
            r['mult'] = float(m.group(6))
        jit = [int(x) for x in re.findall(r'JIT_Worklist_Helper_Thread\s+(\d+) ms CPU', out)]
        r['jit_cpu'] = sum(jit) / 1000 if jit else None
    if net:
        out, stacks = prof(net, elf)
        main = hsts = 0.0
        for s in stacks:
            frames, _, n = s.rpartition(' ')
            parts = frames.split(';')
            if len(parts) > 1 and parts[1].startswith('main'):
                main += float(n)
                if 'hsts' in frames:
                    hsts += float(n)
        mode = re.search(r'mode (\w+)', out)
        r['net_mode'] = mode.group(1) if mode else '?'
        r['net_main'] = main / 1e6  # folded counts are microseconds
        r['hsts'] = hsts / 1e6
        r['hsts_share'] = hsts / main if main else None
    return r


COLS = [('finished', '%.1f'), ('load', '%d'), ('fcp', '%s'), ('main_cpu', '%.1f'), ('proc_cpu', '%.1f'), ('jit_cpu', '%.1f'),
        ('baseline', '%d'), ('dfg', '%d'), ('ftl', '%d'), ('mult', '%.2f'), ('jit_mb', '%.1f'), ('jit_committed', '%.1f'),
        ('pool', '%.0f'), ('pas_committed', '%.0f'), ('rss_peak', '%.0f'), ('net_main', '%.2f'), ('hsts', '%.2f'), ('hsts_share', '%.3f')]


def fmt(v, f):
    if v is None:
        return '-'
    try:
        return f % v
    except TypeError:
        return str(v)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dirs', nargs='+')
    ap.add_argument('--elf', default=os.path.join(HERE, '../../../../toolchain/wpe-webkit/build/bin/ps5wk-child'))
    args = ap.parse_args()
    runs = []
    for d in args.dirs:
        for f in sorted(os.listdir(d)):
            if f.endswith('.log') and os.path.isdir(os.path.join(d, f[:-4])):
                runs.append(run_summary(d, f[:-4], args.elf))
    print('run\t' + '\t'.join(c for c, _ in COLS))
    groups = collections.OrderedDict()
    for r in runs:
        print(r['name'] + '\t' + '\t'.join(fmt(r.get(c), f) for c, f in COLS))
        key = re.sub(r'-(cold|warm)|-r\d+$', '', r['name'])
        groups.setdefault(key, []).append(r)
    print('\nmedians (n runs)')
    print('group\tn\t' + '\t'.join(c for c, _ in COLS))
    for key, rs in groups.items():
        cells = []
        for c, f in COLS:
            v = [r[c] for r in rs if isinstance(r.get(c), (int, float))]
            cells.append(fmt(statistics.median(v), f if f != '%s' else '%.0f') if v else '-')
        print('%s\t%d\t%s' % (key, len(rs), '\t'.join(cells)))


if __name__ == '__main__':
    main()
