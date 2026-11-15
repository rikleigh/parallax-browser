#!/usr/bin/env python3
"""Frame-rate benchmark for a WebGPU/WebGL canvas page (e.g. the PS2 emulator)
from the browser title's per-second log lines, without touching the browser.

The title's TCP log (tools/logserver.sh -> logs/title-*.log) has, every second:
  [wc] 1.0 s: N updates (...); tiles uploaded K (D draws, M MB, ...)
A presented 1920x1080 canvas frame is one 7.9 MB upload, so K is the page's
own frame rate (the compositor's N stays ~60). Every 5 s the WebProcess adds
  [ps5wk-child P] [cpu] web4: C cores (user U, sys S) over 5.0 s

Seconds whose canvas rate is under --fight-below (default 57) and over 3 are
"fight" seconds (menus and round intros run at 60).

  tools/fps-bench.py --label baseline            # live: wait for the fight, 60 fight seconds
  tools/fps-bench.py --label x --seconds 90
  tools/fps-bench.py --file logs/title-....log --last 600   # offline: last 600 s of a log
Live runs write logs/bench-<label>-<time>.csv and append a line to
logs/bench-summary.txt.
"""
import argparse, glob, os, re, statistics, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WC = re.compile(r'^\[wc\] [\d.]+ s: (\d+) updates \(apply.*?tiles uploaded (\d+) \((\d+) draws, ([\d.]+) MB')
WEBCPU = re.compile(r'\[cpu\] web\d+: ([\d.]+) cores \(user ([\d.]+), sys ([\d.]+)\)')
TITLECPU = re.compile(r'\[cpu\] title: main thread busy (\d+) %.*?process CPU ([\d.]+) cores')
CANVAS_MB = 4.0  # an upload this big is a whole-canvas frame, not a tile


def newest_log():
    logs = sorted(glob.glob(os.path.join(ROOT, 'logs', 'title-*.log')), key=os.path.getmtime)
    if not logs:
        sys.exit('no logs/title-*.log (is tools/logserver.sh running?)')
    return logs[-1]


class Second:
    __slots__ = ('t', 'updates', 'fps', 'mb_each', 'web_cores', 'title_cores')

    def __init__(self, t, updates, uploads, mb):
        self.t, self.updates = t, updates
        self.mb_each = mb / uploads if uploads else 0.0
        self.fps = uploads if self.mb_each >= CANVAS_MB else 0
        self.web_cores = None
        self.title_cores = None


def parse_lines(lines, out, state, now=None):
    for line in lines:
        m = WC.match(line)
        if m:
            s = Second(now if now is not None else len(out), int(m.group(1)), int(m.group(2)), float(m.group(4)))
            s.web_cores, s.title_cores = state.pop('web', None), state.pop('title', None)
            out.append(s)
            continue
        m = WEBCPU.search(line)
        if m:
            state['web'] = float(m.group(1))
            state['webLast'] = float(m.group(1))
            continue
        m = TITLECPU.search(line)
        if m:
            state['title'] = float(m.group(2))


def pct(v, p):
    v = sorted(v)
    if not v:
        return float('nan')
    k = (len(v) - 1) * p
    lo, hi = int(k), min(int(k) + 1, len(v) - 1)
    return v[lo] + (v[hi] - v[lo]) * (k - lo)


def summarize(secs, below):
    fight = [s for s in secs if 3 < s.fps < below]
    if not fight:
        return None
    f = [s.fps for s in fight]
    cores = [s.web_cores for s in fight if s.web_cores is not None]
    return {
        'fight_s': len(f), 'all_s': len(secs),
        'mean': statistics.fmean(f), 'median': statistics.median(f),
        'p10': pct(f, 0.10), 'p90': pct(f, 0.90), 'min': min(f), 'max': max(f),
        'web_cores': statistics.fmean(cores) if cores else float('nan'),
    }


def fmt(sm):
    if not sm:
        return 'no fight seconds yet'
    return ('fight {fight_s} s of {all_s}: mean {mean:.1f} fps, median {median:.0f}, p10 {p10:.0f}, p90 {p90:.0f}, '
            'min {min} max {max}; WebProcess {web_cores:.2f} cores').format(**sm)


def offline(path, last, below):
    secs, state = [], {}
    with open(path, 'r', errors='replace') as fh:
        parse_lines(fh, secs, state)
    if last:
        secs = secs[-last:]
    row = ''
    for i, s in enumerate(secs):
        row += '%3d%s ' % (s.fps, '(%.2f)' % s.web_cores if s.web_cores is not None else '')
        if (i + 1) % 20 == 0:
            print(row); row = ''
    if row:
        print(row)
    print(fmt(summarize(secs, below)))


def live(args):
    path = args.file or newest_log()
    print('==> reading %s (Ctrl-C to stop early)' % path, file=sys.stderr)
    fh = open(path, 'r', errors='replace')
    fh.seek(0, os.SEEK_END)
    secs, state, buf = [], {}, ''
    t0 = time.time()
    label = re.sub(r'[^A-Za-z0-9_.-]', '_', args.label or 'run')
    stamp = time.strftime('%Y%m%d-%H%M%S')
    csv_path = os.path.join(ROOT, 'logs', 'bench-%s-%s.csv' % (label, stamp))
    last_print = 0
    idle_since = time.time()
    try:
        while True:
            chunk = fh.read()
            if not chunk:
                if time.time() - idle_since > 15 and not secs:
                    print('    (no [wc] lines for 15 s: is the browser running and logging here?)', file=sys.stderr)
                    idle_since = time.time()
                newest = newest_log() if not args.file else path
                if newest != path:  # logserver restarted: follow the new file
                    path, fh = newest, open(newest, 'r', errors='replace')
                time.sleep(0.25)
                continue
            idle_since = time.time()
            buf += chunk
            lines = buf.split('\n')
            buf = lines.pop()
            n0 = len(secs)
            parse_lines(lines, secs, state, now=round(time.time() - t0, 1))
            for s in secs[n0:]:
                tag = 'fight' if 3 < s.fps < args.fight_below else ('60' if s.fps else '-')
                cores = ' web %.2f cores' % s.web_cores if s.web_cores is not None else ''
                print('%6.0fs  canvas %2d fps  (%s)%s' % (s.t, s.fps, tag, cores), flush=True)
            fight_n = sum(1 for s in secs if 3 < s.fps < args.fight_below)
            if fight_n and fight_n // 15 != last_print:
                last_print = fight_n // 15
                print('    -- ' + fmt(summarize(secs, args.fight_below)), flush=True)
            if args.seconds and fight_n >= args.seconds:
                break
    except KeyboardInterrupt:
        pass
    sm = summarize(secs, args.fight_below)
    with open(csv_path, 'w') as out:
        out.write('t,canvas_fps,updates,mb_per_upload,web_cores,title_cores\n')
        for s in secs:
            out.write('%s,%d,%d,%.2f,%s,%s\n' % (s.t, s.fps, s.updates, s.mb_each,
                      '' if s.web_cores is None else s.web_cores, '' if s.title_cores is None else s.title_cores))
    line = '%s %-20s %s' % (stamp, label, fmt(sm))
    with open(os.path.join(ROOT, 'logs', 'bench-summary.txt'), 'a') as out:
        out.write(line + '\n')
    print('\n' + line)
    print('csv: ' + csv_path)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--label')
    ap.add_argument('--seconds', type=int, default=60, help='stop after this many fight seconds (0: until Ctrl-C)')
    ap.add_argument('--fight-below', type=int, default=57)
    ap.add_argument('--file', help='log file (default: newest logs/title-*.log)')
    ap.add_argument('--last', type=int, help='offline: summarize the last N seconds of --file and exit')
    a = ap.parse_args()
    if a.last:
        offline(a.file or newest_log(), a.last, a.fight_below)
    else:
        live(a)


if __name__ == '__main__':
    main()
