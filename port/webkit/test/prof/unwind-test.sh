#!/usr/bin/env bash
# Checks apps/common/ps5unwind.c on the Mac: the unwind rule it computes for
# the first address of every row of N random FDEs (and the last byte of the
# row) against llvm-dwarfdump --eh-frame's row table.
#   port/webkit/test/prof/unwind-test.sh [elf] [N=3000]
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../../.." && pwd)"
ELF="${1:-$ROOT/toolchain/wpe-webkit/build/bin/ps5wk-child}"
N="${2:-3000}"
T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
cc -O1 -g -Wall -fsanitize=address,undefined -o "$T/unwind-test" "$HERE/unwind-test.c" "$ROOT/port/webkit/apps/common/ps5unwind.c"
/opt/homebrew/opt/llvm@18/bin/llvm-dwarfdump --eh-frame "$ELF" > "$T/dump.txt"
python3 -I - "$T/dump.txt" "$N" "$T/expect.txt" "$T/queries.txt" <<'PY'
import sys, re, random
dump, n, expect_path, query_path = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4]
fdes = []
cur = None
fde_re = re.compile(r'^[0-9a-f]+ [0-9a-f]+ [0-9a-f]+ FDE cie=[0-9a-f]+ pc=([0-9a-f]+)\.\.\.([0-9a-f]+)')
row_re = re.compile(r'^\s+0x([0-9a-f]+): CFA=([^:]+)(?::\s*(.*))?$')
for line in open(dump):
    m = fde_re.match(line)
    if m:
        cur = [int(m.group(1), 16), int(m.group(2), 16), []]
        fdes.append(cur)
        continue
    if cur is not None:
        m = row_re.match(line)
        if m:
            cur[2].append((int(m.group(1), 16), m.group(2), m.group(3) or ''))
random.seed(1)
pick = random.sample(fdes, min(n, len(fdes)))
# Always include FDEs with unusual rules.
pick += [f for f in fdes if any(not re.match(r'R[SB]P[+-]\d+$', r[1]) for r in f[2])][:50]
exp, qs = [], []
for begin, end, rows in pick:
    for i, (addr, cfa, regs) in enumerate(rows):
        nxt = rows[i + 1][0] if i + 1 < len(rows) else end
        m = re.match(r'(RSP|RBP)([+-]\d+)$', cfa)
        rbp = re.search(r'RBP=\[CFA([+-]\d+)\]', regs)
        ra = re.search(r'RIP=\[CFA([+-]\d+)\]', regs)
        unsupported = not m or not ra or re.search(r'RBP=(?!\[CFA)', regs)
        for pc in sorted({addr, nxt - 1}):
            if pc < addr: continue
            qs.append('%x' % pc)
            if unsupported:
                exp.append('%x fail' % pc)
            else:
                exp.append('%x %s %d %s %d' % (pc, m.group(1), int(m.group(2)), rbp.group(1).lstrip('+') if rbp else '-', int(ra.group(1))))
open(expect_path, 'w').write('\n'.join(exp) + '\n')
open(query_path, 'w').write('\n'.join(qs) + '\n')
print('%d FDEs in the dump, %d picked, %d queries' % (len(fdes), len(pick), len(qs)))
PY
"$T/unwind-test" "$ELF" < "$T/queries.txt" | sed -E 's/ fail -?[0-9]+$/ fail/' > "$T/got.txt"
if diff "$T/expect.txt" "$T/got.txt" > "$T/diff.txt"; then
    echo "PASS: $(wc -l < "$T/got.txt") rules match llvm-dwarfdump"
else
    echo "FAIL: $(grep -c '^<' "$T/diff.txt") mismatches"; head -20 "$T/diff.txt"; exit 1
fi
