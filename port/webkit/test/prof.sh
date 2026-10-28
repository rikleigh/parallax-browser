#!/usr/bin/env bash
# Summarize samples from apps/common/profiler.c (PS5WK_PROFILE=1 in
# /data/ps5webkit/child.env): fetches /data/ps5webkit/prof/*.txt over FTP,
# symbolizes with the unstripped ps5wk-child, prints the hottest functions
# (self) and the hottest functions anywhere in the sampled stacks (total).
#   port/webkit/test/prof.sh [file-on-console] [--clear]
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
. "$ROOT/env.sh" >/dev/null
FTP="ftp://$PS5_HOST:${PS5_FTP_PORT:-2121}"
ELF="${ELF:-$ROOT/toolchain/wpe-webkit/build/bin/ps5wk-child}"
OUT="$ROOT/toolchain/wpe-webkit/prof"; mkdir -p "$OUT"
name="${1:-}"
[ -z "$name" ] || [ "$name" = --clear ] && name=$(curl -s "$FTP/data/ps5webkit/prof/" | awk "{print \$NF}" | grep "\.txt$" | sort -t- -k2 -n | tail -1)
curl -s "$FTP/data/ps5webkit/prof/$name" -o "$OUT/$name"
echo "== $name: $(wc -l < "$OUT/$name") samples (2 ms each)"
python3 - "$OUT/$name" "$ELF" <<'PY'
import sys, subprocess, collections
samples = [l.split() for l in open(sys.argv[1]) if l.strip()]
addrs = sorted({a for s in samples for a in s if a not in ('0', 'jit')})
sym = {'jit': '(JIT-compiled JavaScript)'}
if addrs:
    out = subprocess.run(['/opt/homebrew/opt/llvm@18/bin/llvm-addr2line', '-f', '-C', '-e', sys.argv[2]] + ['0x' + a for a in addrs],
                         capture_output=True, text=True).stdout.split('\n')
    for i, a in enumerate(addrs):
        f = out[2 * i] if 2 * i < len(out) else '?'
        sym[a] = f[:110]
selfc, total = collections.Counter(), collections.Counter()
for s in samples:
    selfc[sym.get(s[0], '(outside the ELF: libkernel/libc)')] += 1
    for f in {sym.get(a, '?') for a in s if a != '0'}:
        total[f] += 1
n = len(samples) or 1
print('-- self')
for f, c in selfc.most_common(25): print('%5.1f%%  %s' % (100.0 * c / n, f))
print('-- total (function anywhere in the scanned stack)')
for f, c in total.most_common(40): print('%5.1f%%  %s' % (100.0 * c / n, f))
# Where the thread's time goes: each sample by its innermost frame that
# names a known area (JavaScript, style, layout, paint, ...).
import re
areas = [('JavaScript (JIT code)', r'^\(JIT'), ('JavaScript (JSC, LLInt, GC)', r'JSC::|llint_|op_|Yarr|Inspector|pas_|bmalloc'),
         ('DOM bindings', r'WebCore::JS[A-Z]|IDL|WebCore::Bindings'),
         ('style', r'WebCore::Style::|CSS|Selector'), ('layout', r'WebCore::(Layout|Render|Legacy|Inline|LineLayout)|LayoutIntegration|BoxGeometry'),
         ('paint', r'Sk[A-Z]|skia|GraphicsContext|paint|Paint|Skia|ImageBuffer|DisplayList'),
         ('text/fonts', r'harfbuzz|hb_|FT_|Font|Glyph|ComplexText'),
         ('media', r'gst|Media|SourceBuffer'), ('IPC/run loop', r'IPC::|RunLoop|g_main|WTF::'),]
cat = collections.Counter()
for smp in samples:
    names = [sym.get(a, '') for a in smp if a != '0']
    hit = 'other'
    for f in names:
        found = next((a for a, rx in areas if re.search(rx, f)), None)
        if found:
            hit = found
            break
    cat[hit] += 1
print('-- by area (innermost frame that names one)')
for a, c in cat.most_common(): print('%5.1f%%  %s' % (100.0 * c / n, a))
PY
if [ "${2:-}" = --clear ] || [ "${1:-}" = --clear ]; then curl -s "$FTP/" -Q "DELE /data/ps5webkit/prof/$name" >/dev/null || true; fi; exit 0
