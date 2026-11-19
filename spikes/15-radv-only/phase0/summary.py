#!/usr/bin/env python3
"""Phase 0 summary: one row per case log from run.sh.

  summary.py <dir>            the table, then each case's RESULT summaries
Per case, from the title's stats lines:
  present/s   [present] 5 s windows: the best window's frames/s and the
              median over windows with more than 5 frames/s
  render ms   [present] render avg, median of those windows
  scene ms    [wc] scene per paint, paint ms: median and max (per-second lines)
  draws       draws per scene paint, median
  upload ms   [wc] tiles uploaded: ms per upload, weighted by uploads
  convert ms  [present] copy+convert avg, median of windows with video frames
  direct MB   [mem] direct memory in use, highest
  flex MB     [mem] title flexible free, lowest
"""
import glob, os, re, statistics, sys

PRESENT = re.compile(r'\[present\] (\d+) frames \(([\d.]+)/s\), render ([\d.]+) ms avg.*?(\d+) video frames.*?copy\+convert ([\d.]+) ms avg')
SCENE = re.compile(r'\[wc\] scene per paint: [\d.]+ ms prepare, ([\d.]+) ms paint;.*?draws (\d+)')
TILES = re.compile(r'\[wc\] [\d.]+ s: \d+ updates.*?tiles uploaded (\d+) \(\d+ draws, [\d.]+ MB, ([\d.]+) ms')
MEM = re.compile(r'^\[mem\] [^:]*: flexible ([\d.]+) MB free; direct memory ([\d.]+) MB in use')
RESULT = re.compile(r'RESULT (\S+) (summary|n=\d+|load|vr-selftest\S*) (.*)')
VR = re.compile(r'\[vr\] .*')
WGLVIDEO = re.compile(r'\[webgl-video\] ([\d.]+ fps.*)')


def med(xs):
    return statistics.median(xs) if xs else None


def f(x, fmt='{:.2f}'):
    return '-' if x is None else fmt.format(x)


def case(path):
    pres, scene, draws, mem_d, mem_f, results, vr = [], [], [], [], [], [], []
    up_n = up_ms = 0
    for line in open(path, errors='replace'):
        m = PRESENT.search(line)
        if m:
            pres.append((float(m.group(2)), float(m.group(3)), int(m.group(4)), float(m.group(5))))
            continue
        m = SCENE.search(line)
        if m:
            scene.append(float(m.group(1)))
            draws.append(int(m.group(2)))
            continue
        m = TILES.search(line)
        if m:
            up_n += int(m.group(1))
            up_ms += float(m.group(2))
            continue
        m = MEM.search(line)
        if m:
            mem_f.append(float(m.group(1)))
            mem_d.append(float(m.group(2)))
            continue
        m = RESULT.search(line)
        if m:
            results.append(m.group(0)[:200])
            continue
        m = WGLVIDEO.search(line)
        if m:
            results.append('webgl-video ' + m.group(1)[:160])
            continue
        if VR.search(line) and ('render' in line or 'frames' in line):
            vr.append(line.strip()[:220])
    busy = [p for p in pres if p[0] > 5]
    video = [p[3] for p in pres if p[2] > 0]
    return {
        'fps_best': max((p[0] for p in pres), default=None),
        'fps_med': med([p[0] for p in busy]),
        'render': med([p[1] for p in busy]),
        'scene_med': med(scene), 'scene_max': max(scene, default=None),
        'draws': med(draws),
        'upload': up_ms / up_n if up_n else None,
        'convert': med(video),
        'direct': max(mem_d, default=None), 'flex': min(mem_f, default=None),
        'results': results, 'vr': vr,
    }


def main():
    d = sys.argv[1]
    logs = sorted(glob.glob(os.path.join(d, '*.log')), key=os.path.getmtime)
    rows = [(os.path.basename(p)[:-4], case(p)) for p in logs]
    print('| case | present/s best (median) | render ms | scene paint ms med (max) | draws/paint | upload ms each | convert ms | direct MB max | flexible MB min |')
    print('| --- | --- | --- | --- | --- | --- | --- | --- | --- |')
    for name, c in rows:
        print(f"| {name} | {f(c['fps_best'], '{:.1f}')} ({f(c['fps_med'], '{:.1f}')}) | {f(c['render'])} | "
              f"{f(c['scene_med'])} ({f(c['scene_max'])}) | {f(c['draws'], '{:.0f}')} | {f(c['upload'], '{:.3f}')} | "
              f"{f(c['convert'])} | {f(c['direct'], '{:.0f}')} | {f(c['flex'], '{:.0f}')} |")
    print()
    for name, c in rows:
        keep = [r for r in c['results'] if ' summary ' in r or ' n=' in r or 'vr-selftest' in r]
        keep += [r for r in c['results'] if r.startswith('webgl-video ')][-2:]
        if keep or c['vr']:
            print(f'{name}:')
            for r in dict.fromkeys(keep):
                print('   ' + r)
            for v in c['vr'][-3:]:
                print('   ' + v)


if __name__ == '__main__':
    main()
