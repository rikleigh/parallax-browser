#!/usr/bin/env python3
"""Summarize the title's heap snapshots (apps/title/heapprof.c).

  tools/heapprof.py heap-<pid>-005.txt                 what holds memory now
  tools/heapprof.py --base heap-<pid>-002.txt heap-<pid>-005.txt   what grew since
  tools/heapprof.py --fetch [--pid N]                  copy the console's snapshots to logs/heap/ first

Symbolizes with prof.py's llvm-symbolizer helpers against the title's ELF
(toolchain/wpe-webkit/titles/ps5browser-symbols.elf by default). Prints the
estimated bytes per allocation site (the first frame outside the allocators
and their wrappers), per library, and the biggest stacks.
"""
import argparse, collections, glob, os, re, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'port/webkit/test/prof'))
import prof  # noqa: E402

# Frames that only pass an allocation along: the site is the first frame past them.
ALLOCATOR = re.compile(
    r'^(\?\?|0x[0-9a-f]+|__wrap_|note_alloc|onAlloc|ps5wk_backtrace|operator new|operator delete|malloc|calloc|realloc|free|'
    r'posix_memalign|aligned_alloc|memalign|std::__1::__libcpp_allocate|std::__1::allocator|std::__1::__allocation|'
    r'std::__1::vector.*::(__vallocate|__append|__push_back_slow_path|__emplace_back_slow_path|reserve|resize|__construct_at_end|__init_with_size|vector)|'
    r'std::__1::__split_buffer|std::__1::basic_string.*::(__init|__grow_by|reserve|append|basic_string)|std::__1::__hash_table.*::(__rehash|__construct_node|__emplace_unique)|'
    r'std::__1::__tree|std::__1::make_unique|std::__1::make_shared|std::__1::__shared_ptr_emplace|std::__1::unique_ptr|'
    r'g_malloc|g_malloc0|g_realloc|g_try_malloc|g_try_malloc0|g_try_realloc|g_slice_|g_new|g_memdup|g_strdup|g_strndup|g_byte_array|g_array_|g_ptr_array_|'
    r'gst_buffer_new|gst_memory_new|_sysmem_|gst_allocator_|sk_malloc|sk_calloc|sk_realloc|SkMallocPixelRef|'
    r'vk_alloc|vk_zalloc|vk_object_alloc|vk_object_zalloc|vk_default_alloc|vk_multialloc|ralloc_|rzalloc_|linear_|'
    r'dawn::native::.*Allocat|absl::|WTF::fastMalloc|WTF::fastZeroedMalloc|WTF::fastRealloc|WTF::SystemMalloc)')

LIBS = [
    ('Dawn (WebGPU)', r'dawn::|wgpu|^tint::|Dawn'),
    ('RADV/Mesa (Vulkan driver)', r'^radv_|^ac_|^aco::|^nir_|^vk_|^ralloc|^util_|^si_|^amdgpu|^ws_|^radeon_|^_mesa|^glsl'),
    ('ANGLE (WebGL)', r'^rx::|^gl::|^egl::|angle::|^sh::|^GL_|^EGL_'),
    ('WebKit WebGPU', r'WebGPU|WebKit::Remote.*GPU|WebKit::WebGPU|WebCore::WebGPU|WebKit::RemoteGPU'),
    ('Skia', r'^Sk|^sk_|skgpu|skia'),
    ('GStreamer', r'^gst_|Gst|^orc_'),
    ('glib/soup/IO', r'^g_|^soup_|^Soup'),
    ('WebKit IPC', r'IPC::'),
    ('WebCore/WebKit', r'WebCore::|WebKit::|WTF::|JSC::'),
    ('libc++/other', r'.'),
]


def parse(path):
    header, stacks = {}, []
    with open(path) as f:
        for line in f:
            if line.startswith('#'):
                for field in line.split()[2:]:
                    k, _, v = field.partition('=')
                    header[k] = v
            elif line.startswith('S '):
                parts = line.split()
                stacks.append((int(parts[1]), int(parts[2]), tuple(parts[3:])))
    return header, stacks


def site_of(frames):
    for f in frames:
        if not ALLOCATOR.search(f) and f != '(other module)':
            return f
    return frames[0] if frames else '??'


def lib_of(frames):
    for f in frames:
        if ALLOCATOR.search(f) or f == '(other module)':
            continue
        for name, rx in LIBS:
            if re.search(rx, f):
                return name
    return 'unknown'


def fetch(pid):
    sys.path.insert(0, ROOT)
    env = {}
    out = subprocess.run(['bash', '-c', '. "%s/env.sh" >/dev/null; echo "$PS5_HOST ${PS5_FTP_PORT:-2121}"' % ROOT], capture_output=True, text=True).stdout.split()
    ftp = 'ftp://%s:%s/data/ps5webkit/prof/' % (out[0], out[1])
    # ftpsrv answers NLST with 502: the long listing's last field is the name.
    listing = [l.split()[-1] for l in subprocess.run(['curl', '-sS', '--max-time', '10', ftp], capture_output=True, text=True).stdout.splitlines() if l.strip()]
    names = sorted(n for n in listing if re.match(r'heap-\d+-\d+\.txt$', n) and (pid is None or n.startswith('heap-%d-' % pid)))
    dest = os.path.join(ROOT, 'logs', 'heap')
    os.makedirs(dest, exist_ok=True)
    for n in names:
        subprocess.run(['curl', '-sS', '--max-time', '30', '-o', os.path.join(dest, n), ftp + n], check=True)
    print('fetched %d snapshot(s) to %s' % (len(names), dest))
    return [os.path.join(dest, n) for n in names]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('files', nargs='*')
    ap.add_argument('--base', help='an earlier snapshot: show what grew since')
    ap.add_argument('--elf', default=os.path.join(ROOT, 'toolchain/wpe-webkit/titles/ps5browser-symbols.elf'))
    ap.add_argument('--top', type=int, default=25)
    ap.add_argument('--fetch', action='store_true')
    ap.add_argument('--pid', type=int)
    args = ap.parse_args()
    prof.INLINES = True
    files = args.files
    if args.fetch:
        got = fetch(args.pid)
        if not files and got:
            files = [got[-1]]
            if not args.base and len(got) > 1:
                args.base = got[0]
    if not files:
        sys.exit('no snapshot given')
    header, stacks = parse(files[-1])
    base = parse(args.base)[1] if args.base else []

    offsets = set()
    for _, _, pcs in stacks + base:
        for i, pc in enumerate(pcs):
            if not pc.startswith(('j', 'x')):
                offsets.add(int(pc, 16) - (1 if i else 0))
    adjust = 0
    nm = subprocess.run([prof.NM, args.elf], capture_output=True, text=True).stdout
    m = re.search(r'^([0-9a-f]+) T ps5wk_profiler_start$', nm, re.M)
    if m and header.get('marker', '0') not in ('0', ''):
        adjust = int(m.group(1), 16) - int(header['marker'], 16)
        if adjust:
            print('(offsets adjusted by %+#x to %s)' % (adjust, args.elf))
    offsets = {o + adjust for o in offsets}
    sym = prof.symbolize(args.elf, offsets)

    def tally(items, sign):
        sites, libs, full = collections.Counter(), collections.Counter(), collections.Counter()
        for nbytes, _, pcs in items:
            frames = prof.frames_of(pcs, sym, adjust)
            sites[site_of(frames)] += sign * nbytes
            libs[lib_of(frames)] += sign * nbytes
            full[' <- '.join(f for f in frames[:14])] += sign * nbytes
        return sites, libs, full

    sites, libs, full = tally(stacks, 1)
    if base:
        s2, l2, f2 = tally(base, -1)
        sites.update(s2)
        libs.update(l2)
        full.update(f2)
    mb = lambda b: b / 1048576.0
    print('%s: snapshot %s at %.0f s (%s): in use %.1f MB of %.1f MB mapped; samples %s live, %s taken, %s dropped; interval %s KB' % (
        os.path.basename(files[-1]), header.get('n'), int(header.get('ms', 0)) / 1000, header.get('why'),
        mb(int(header.get('in_use', 0))), mb(int(header.get('mapped', 0))), header.get('live'), header.get('sampled'),
        header.get('dropped'), int(header.get('interval', 0)) >> 10))
    if base:
        bh = parse(args.base)[0]
        print('compared with %s (snapshot %s at %.0f s, in use %.1f MB): growth in use %+.1f MB' % (
            os.path.basename(args.base), bh.get('n'), int(bh.get('ms', 0)) / 1000, mb(int(bh.get('in_use', 0))),
            mb(int(header.get('in_use', 0)) - int(bh.get('in_use', 0)))))
    print('\nBy library (estimated MB%s):' % (', change' if base else ''))
    for name, b in sorted(libs.items(), key=lambda kv: -abs(kv[1]))[:12]:
        print('  %9.1f  %s' % (mb(b), name))
    print('\nBy allocation site:')
    for name, b in sorted(sites.items(), key=lambda kv: -abs(kv[1]))[:args.top]:
        print('  %9.1f  %s' % (mb(b), name))
    print('\nBiggest stacks:')
    for name, b in sorted(full.items(), key=lambda kv: -abs(kv[1]))[:max(8, args.top // 2)]:
        print('  %9.1f  %s' % (mb(b), name))


if __name__ == '__main__':
    main()
