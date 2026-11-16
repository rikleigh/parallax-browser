#!/usr/bin/env python3
"""Sum up the browser title's stall samples (apps/title/ps5browser.c).

  stall-sym.py [--elf ps5browser.elf] [--top N] [--stalls] [--frames N] stalls-title-<pid>.txt

The title samples its main thread's stack every 20 ms while the main loop is
stuck, and writes each stall over 250 ms to
/data/ps5webkit/prof/stalls-title-<pid>.txt:

  # stall <n> <ms> ms, <samples> samples every 20 ms, main thread CPU <ms> ms (<busy|waiting>), at <s> s
  S <count> <frames>      (hex offsets in the unstripped ELF, j<tier> JIT code, x<hex> other modules)

Prints, over all the stalls, the functions the samples were in: "self" (the
innermost frame) and "total" (anywhere on the stack, once per sample), with
the share of all samples. --stalls adds each stall with its most common
innermost functions; --frames N prints the N heaviest stacks symbolized.
The ELF defaults to the newest toolchain/wpe-webkit/titles/debug/<build>/ps5browser.elf
(take the one of the build that wrote the file: the log names it).
"""
import argparse, collections, glob, os, re, subprocess

SYMBOLIZER = '/opt/homebrew/opt/llvm@18/bin/llvm-symbolizer'
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))))
TIERS = {'j0': 'JIT code (thunk/IC/regexp)', 'j1': 'JIT code (Baseline)', 'j2': 'JIT code (DFG)', 'j3': 'JIT code (FTL)', 'j': 'JIT code'}


def newest_elf():
    builds = sorted(glob.glob(os.path.join(ROOT, 'toolchain/wpe-webkit/titles/debug/*/ps5browser.elf')), key=os.path.getmtime)
    return builds[-1] if builds else None


def read(path):
    stalls = []
    for line in open(path, errors='replace'):
        line = line.rstrip('\n')
        if line.startswith('# stall '):
            stalls.append({'head': line[2:], 'stacks': []})
        elif line.startswith('S ') and stalls:
            parts = line.split()
            stalls[-1]['stacks'].append((int(parts[1]), parts[2:]))
    return stalls


def symbolize(elf, offsets):
    names = {}
    if not offsets or not elf:
        return names
    order = sorted(offsets)
    out = subprocess.run([SYMBOLIZER, '--obj=' + elf, '--demangle', '--functions=linkage', '--no-inlines'],
                         input='\n'.join('0x%x' % o for o in order), capture_output=True, text=True).stdout
    for o, block in zip(order, out.strip('\n').split('\n\n')):
        parts = block.split('\n')
        fn = re.sub(r'\(.*', '', parts[0]).strip() or parts[0]
        names[o] = fn
    return names


def frame_name(frame, index, names):
    if re.fullmatch(r'[0-9a-f]+', frame):
        return names.get(int(frame, 16) - (1 if index else 0), '?+' + frame)
    if frame in TIERS:
        return TIERS[frame]
    if frame.startswith('x'):
        return 'other module (libkernel/libc)'
    return frame


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--elf', default=None)
    ap.add_argument('--top', type=int, default=40)
    ap.add_argument('--stalls', action='store_true')
    ap.add_argument('--frames', type=int, default=0)
    ap.add_argument('file')
    a = ap.parse_args()
    elf = a.elf or newest_elf()
    stalls = read(a.file)
    offsets = set()
    for st in stalls:
        for _, frames in st['stacks']:
            for i, f in enumerate(frames):
                if re.fullmatch(r'[0-9a-f]+', f):
                    offsets.add(int(f, 16) - (1 if i else 0))
    names = symbolize(elf, offsets)

    total = collections.Counter()
    self_ = collections.Counter()
    samples = 0
    heaviest = collections.Counter()
    for st in stalls:
        leaf = collections.Counter()
        for count, frames in st['stacks']:
            named = [frame_name(f, i, names) for i, f in enumerate(frames)]
            samples += count
            if named:
                self_[named[0]] += count
                leaf[named[0]] += count
            for n in set(named):
                total[n] += count
            heaviest[tuple(named)] += count
        st['leaf'] = leaf
    print('ELF: %s' % elf)
    print('%d stalls, %d samples' % (len(stalls), samples))
    busy = sum(1 for st in stalls if '(busy)' in st['head'])
    print('main thread busy (on the CPU most of the stall) in %d of them, waiting in %d' % (busy, len(stalls) - busy))
    print('\nself (innermost frame):')
    for n, c in self_.most_common(a.top):
        print('  %5.1f%%  %5d  %s' % (100.0 * c / samples, c, n))
    print('\ntotal (anywhere on the stack):')
    for n, c in total.most_common(a.top):
        print('  %5.1f%%  %5d  %s' % (100.0 * c / samples, c, n))
    if a.stalls:
        print('\nstalls:')
        for st in stalls:
            print('  ' + st['head'])
            for n, c in st['leaf'].most_common(3):
                print('      %4d  %s' % (c, n))
    for stack, c in heaviest.most_common(a.frames):
        print('\n%d samples:' % c)
        for n in stack:
            print('    ' + n)


if __name__ == '__main__':
    main()
