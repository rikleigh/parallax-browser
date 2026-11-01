#!/usr/bin/env python3
"""Symbolize the main-thread watchdog's backtraces (apps/common/watchdog.c).

  watchdog-sym.py [--elf ps5wk-child] [--no-inlines] [log]     (log: stdin if omitted)

Prints the [watchdog ...] lines of a title/harness log, each "bt" line
followed by its frames: hex offsets in the unstripped ps5wk-child
(toolchain/wpe-webkit/titles/debug/<build>/ps5wk-child for a packaged title;
default: the newest there), "j<tier>" JIT code, "x<hex>" other modules.
Frames after the first are return addresses: looked up at offset - 1.
"""
import argparse, glob, os, re, subprocess, sys

SYMBOLIZER = '/opt/homebrew/opt/llvm@18/bin/llvm-symbolizer'
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))))
TIERS = {'j0': 'JIT code (thunk/IC/regexp)', 'j1': 'JIT code (Baseline)', 'j2': 'JIT code (DFG)', 'j3': 'JIT code (FTL)', 'j': 'JIT code'}


def newest_elf():
    builds = sorted(glob.glob(os.path.join(ROOT, 'toolchain/wpe-webkit/titles/debug/*/ps5wk-child')), key=os.path.getmtime)
    return builds[-1] if builds else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--elf', default=None)
    ap.add_argument('--no-inlines', dest='inlines', action='store_false', help='outermost function per frame only')
    ap.add_argument('log', nargs='?')
    a = ap.parse_args()
    elf = a.elf or newest_elf()
    lines = [l.rstrip('\n') for l in (open(a.log, errors='replace') if a.log else sys.stdin) if '[watchdog ' in l]
    stacks = []
    offsets = set()
    for l in lines:
        m = re.search(r'\] bt (.*?):((?: \S+)+)$', l)
        frames = m.group(2).split() if m and not m.group(2).strip().startswith(('same as', 'no answer', 'thread gone')) else []
        stacks.append(frames)
        for i, f in enumerate(frames):
            if re.fullmatch(r'[0-9a-f]+', f):
                offsets.add(int(f, 16) - (1 if i else 0))
    names = {}
    if offsets and elf:
        order = sorted(offsets)
        out = subprocess.run([SYMBOLIZER, '--obj=' + elf, '--demangle', '--functions=linkage', '--inlines' if a.inlines else '--no-inlines'],
                             input='\n'.join('0x%x' % o for o in order), capture_output=True, text=True).stdout
        blocks = out.strip('\n').split('\n\n')
        for o, block in zip(order, blocks):
            parts = block.split('\n')
            entries = []
            for k in range(0, len(parts) - 1, 2):
                fn = re.sub(r'\(.*', '', parts[k]).strip() or parts[k]
                loc = re.sub(r'^.*?/toolchain/(wpe-webkit/)?', '', parts[k + 1])
                entries.append('%s  %s' % (fn, loc))
            names[o] = entries
    for l, frames in zip(lines, stacks):
        print(l)
        for i, f in enumerate(frames):
            if re.fullmatch(r'[0-9a-f]+', f):
                for k, e in enumerate(names.get(int(f, 16) - (1 if i else 0), ['?'])):
                    print('      %s%-9s %s' % ('' if k == 0 else '  ', f if k == 0 else '', e))
            elif f in TIERS:
                print('      %-9s %s' % (f, TIERS[f]))
            else:
                print('      %-9s %s' % (f, 'other module (libkernel/libc)' if f.startswith('x') else f))


if __name__ == '__main__':
    main()
