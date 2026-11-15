#!/usr/bin/env python3
"""Summarize apps/common/profiler.c output (<tag>-<pid>.prof files).

  prof.py [--elf ps5wk-child] [--folded out.folded] [--from MS] [--to MS] [--top N] file.prof...

Symbolizes with llvm-symbolizer (inlined frames expanded) and prints, per
process: CPU per thread; for the busiest threads where the time goes by area
(the innermost frame that names one: JavaScript by tier, style, layout,
paint, text, images, network, malloc, locks, kernel...), and for WebKit's
main thread also by phase (the outermost frame that names one: rendering
update, JS task, HTML parsing, ...); the hottest functions; and a timeline.
--folded writes "process;thread;root;...;leaf count" lines (flame graphs,
speedscope). --jitdump jit-*.dump (JSC_useJITDump=true,
JSC_jitDumpDirectory=/data/ps5webkit/prof) names JIT frames by function
("JIT: wasm OMG | <function>"); --code-dir DIR also writes the machine code
of the hottest of them there (<n>.bin plus a .s from llvm-objdump). --from/--to keep only windows in that range (ms since the
profiler started in that process).
"""
import argparse, collections, json, os, re, subprocess, sys

SYMBOLIZER = '/opt/homebrew/opt/llvm@18/bin/llvm-symbolizer'
NM = '/opt/homebrew/opt/llvm@18/bin/llvm-nm'

TIERS = {'j0': 'JIT: thunks/ICs/regexp', 'j1': 'JIT: Baseline', 'j2': 'JIT: DFG', 'j3': 'JIT: FTL', 'j4': 'JIT: wasm BBQ', 'j5': 'JIT: wasm OMG', 'j6': 'JIT: wasm OMG (OSR entry)', 'j7': 'JIT: wasm BBQ, function over 100 KB', 'j8': 'JIT: wasm OMG, function over 100 KB', 'j': 'JIT: unknown tier'}

# Innermost frame that matches decides the area (checked leaf to root).
AREAS = [
    ('JS: JIT code', r'^JIT: '),
    ('JS: LLInt', r'^llint_|^op_|^vmEntryTo|^wasm_|LLInt::|^jsc_llint|llintPCRange'),
    ('JS: GC', r'JSC::(Heap|SlotVisitor|MarkedBlock|MarkedSpace|BlockDirectory|LocalAllocator|Subspace|GCThread|MarkingConstraint|IsoCellSet|PreciseAllocation|ConservativeRoots|MachineThreads|Heap::)|JSC::.*::visitChildren|JSC::GC'),
    ('JS: parse/bytecode', r'JSC::(Parser|Lexer|ASTBuilder|BytecodeGenerator|UnlinkedCodeBlock|CodeCache|generateUnlinked|parse|SourceProvider|ScopeNode|.*Node::emitBytecode)|recursivelyGenerateUnlinkedCodeBlock'),
    ('JS: JIT compiler', r'JSC::(DFG|FTL|B3|Air|JIT::|JITPlan|BaselineJIT|LinkBuffer|MacroAssembler|AssemblerBuffer|X86Assembler|JITWorklist|CCallHelpers)'),
    ('JS: regexp', r'JSC::Yarr|Yarr::'),
    ('JS: runtime', r'JSC::|WTF::String|WTF::StringImpl|WTF::AtomString|WTF::HashTable|WTF::dtoa|double_conversion'),
    ('DOM bindings', r'WebCore::JS[A-Z]|WebCore::.*Binding|IDL'),
    ('style', r'WebCore::Style::|WebCore::CSS|WebCore::Selector|WebCore::ElementRuleCollector|WebCore::RuleSet|WebCore::RenderStyle|WebCore::MatchedDeclarations|StyleBuilder|WebCore::Animation|KeyframeEffect|WebCore::WebAnimation|AnimationTimeline|BlendingKeyframe'),
    ('layout', r'WebCore::(Layout|Render(?!Layer.*paint)|Legacy|Inline|LineLayout|LocalFrameView::layout|LayoutContext|FlexLayout|GridLayout|TableLayout|BoxGeometry|LineBuilder|TextBreak)|LayoutIntegration'),
    ('images', r'ImageDecoder|ImageSource|BitmapImage|PNGImage|JPEGImage|WEBPImage|GIFImage|AVIFImage|png_|jpeg_|jpeg|WebP|VP8|libavif|dav1d|jinit|decode_mcu|ycc_|^Sk.*Codec|SkCodec|SkJpeg|SkPng|SkWebp'),
    ('text/fonts', r'hb_|harfbuzz|FT_|^tt_|cff_|ft_|Fc[A-Z]|WebCore::(Font|Glyph|ComplexText|TextRun|WidthIterator|TextPainter|FontCascade|SkiaHarfBuzz)|SkFont|SkScalerContext|SkStrike|SkGlyph|icu_|ubrk_|ucol_|u_'),
    ('paint: record', r'WebCore::(GraphicsContext|RenderLayer::paint|.*::paint|LocalFrameView::paint|PaintInfo|BorderPainter|BackgroundPainter|DisplayList)|SkPictureRecord|SkRecorder|SkRecord::|SkBBH|SkRTree|NonCompositedFrameRenderer::.*record'),
    ('paint: raster', r'^Sk|skia|^S32|^SkRaster|SkBlit|SkScan|SkDraw|SkPaint|SkPath|SkCanvas|SkPicture|SkOpts|hsw::|sse41::|ssse3::|skcms|SkRasterPipeline|portable::|NonCompositedFrameRenderer|ps5ParallelFor|PS5RasterJob'),
    ('media', r'gst_|Gst|WebCore::.*Media|SourceBuffer|MediaSource|ps5vdec|ps5video|orc_|audio_|video_'),
    ('network/TLS', r'soup_|Soup|SSL_|ssl3?_|tls_|ossl_|EVP_|BN_|bn_|ec_|ECDSA|RSA_|sha256|SHA|aes|AES|gcm|chacha|poly1305|nghttp2|g_tls|WebKit::Network|NetworkCache|NetworkLoad|NetworkDataTask|NetworkResource|inflate|Brotli|brotli|zstd|ZSTD'),
    ('IPC', r'IPC::|WebKit::.*MessageReceiver|Connection::(send|dispatch|read|process)|sendmsg|recvmsg'),
    ('HTML/DOM', r'WebCore::(HTML|Document|Element|Node|ContainerNode|Text|Event|MutationObserver|Range|TreeScope|ShadowRoot|SVG|XML|Attr|NamedNodeMap|ChildList|QualifiedName)'),
    ('malloc', r'pas_|bmalloc|ps5malloc|__wrap_malloc|__wrap_free|__wrap_realloc|__wrap_calloc|WTF::fastMalloc|WTF::fastFree|fastMalloc|operator new|operator delete|TZone|IsoHeap'),
    ('locks', r'ps5locks|__wrap_pthread|WTF::Lock|WTF::ParkingLot|WTF::Condition|LockAlgorithm|g_mutex|g_cond|g_rec_mutex'),
    ('glib main loop', r'g_main_|g_poll|g_source|g_idle|g_timeout|WTF::RunLoop|g_wakeup'),
    ('libc/kernel', r'^\(other module\)'),
]

# Outermost frame that matches decides the phase (main thread; root to leaf).
PHASES = [
    ('rendering update: paint', r'NonCompositedFrameRenderer::(display|paint|render|ps5)|DrawingArea.*::(display|paint)|WebCore::LocalFrameView::paint|WebPage::paint'),
    ('rendering update: style/layout/rAF', r'WebCore::Page::updateRendering|WebCore::Page::finalizeRenderingUpdate|DrawingArea.*updateRendering|WebPage::updateRendering|WebCore::ScriptedAnimationController|WebCore::Page::doAfterUpdateRendering'),
    ('style/layout (outside an update)', r'WebCore::Document::(updateStyleIfNeeded|updateLayout|resolveStyle)|LocalFrameView::layout|LayoutContext::layout'),
    ('HTML parsing', r'WebCore::HTMLDocumentParser|HTMLTreeBuilder|HTMLTokenizer|HTMLConstructionSite'),
    ('JS: timers/events/microtasks', r'WebCore::(DOMTimer|ScheduledAction|EventTarget::fireEventListeners|EventDispatcher|MicrotaskQueue|JSExecState|ScriptController::evaluate|ScriptElement::executeClassicScript|ScriptElement::execute|PendingScript|ScriptRunner|WorkerOrWorklet|JSEventListener|IdleCallback|MessagePort)|JSC::(JSPromise|microtask)|JSC::Interpreter::execute|JSC::call|JSC::profiledCall'),
    ('resource loading', r'WebCore::(SubresourceLoader|ResourceLoader|CachedResource|DocumentLoader|FrameLoader|CachedImage|CachedScript|CachedCSS)|WebKit::WebResourceLoader|WebKit::WebLoaderStrategy'),
    ('image decoding', r'ImageDecoder|ImageSource'),
    ('GC (main)', r'JSC::Heap::|JSC::GCActivityCallback|OpportunisticTaskScheduler'),
    ('IPC dispatch', r'IPC::Connection::dispatch|WebKit::WebProcess::didReceive|WebKit::WebPage::didReceive'),
    ('media', r'gst_|WebCore::.*Media'),
]
# Samples taken while the thread was blocked (it used the CPU earlier in the tick).
BLOCKED = re.compile(r'ParkingLot::park|Condition::wait|ThreadCondition::timedWait|g_cond_wait|g_poll|g_main_context_poll|cond_wait|nanosleep|sem_wait|BinarySemaphore::wait|g_async_queue_pop|kevent|poll\b|__wrap_pthread_cond_(timed)?wait|futex_wait')


def parse(path, lo, hi):
    header, windows, threads, stacks = {}, [], {}, collections.Counter()
    current = 0
    keep = True
    for line in open(path, errors='replace'):
        if line.startswith('#'):
            fields = line.split()
            if len(fields) > 2 and fields[1] == 'ps5prof':
                header['version'] = fields[2]
            header.update(dict(kv.split('=', 1) for kv in fields[3:] if '=' in kv))
            continue
        parts = line.split()
        if not parts:
            continue
        if parts[0] == 'W':
            current = float(parts[1])
            keep = (lo is None or current >= lo) and (hi is None or current - float(header.get('window_ms', 1000)) <= hi)
            v3 = header.get('version') == '3'
            windows.append({'ms': current, 'cpu': float(parts[2]), 'samples': int(parts[3]), 'dropped': int(parts[4]),
                            'signals': int(parts[5]) if v3 else 0, 'idle_signals': int(parts[6]) if v3 else 0,
                            'jit': [int(x) for x in parts[(8 if v3 else 6):]], 'keep': keep, 'threads': {}})
        elif parts[0] == 'T':
            idx, gen, cpu, samples = int(parts[1]), int(parts[2]), float(parts[3]), int(parts[4])
            name = ' '.join(parts[5:])
            key = (idx, gen)
            t = threads.setdefault(key, {'name': name, 'cpu': [], 'samples': 0})
            if name != '-':
                t['name'] = name
            t['cpu'].append((current, cpu))
            if windows:
                windows[-1]['threads'][key] = cpu
        elif parts[0] == 'S' and keep:
            idx, count = int(parts[1]), int(parts[2])
            if header.get('version') == '3':  # weighted: microseconds of the thread's CPU
                stacks[(idx, tuple(parts[4:]))] += int(parts[3]) / 1000.0
            else:
                stacks[(idx, tuple(parts[3:]))] += count * float(header.get('interval_us', 1000)) / 1000.0
    return header, windows, threads, stacks


INLINES = False
JIT_MAP = []  # sorted (start, end, name, code bytes) from --jitdump
JIT_STARTS = []


def load_jitdump(paths):
    """JSC's PerfLog files (perf's jitdump format, version 1): code load records."""
    import bisect, struct
    loads = {}
    for path in paths:
        data = open(path, 'rb').read()
        if len(data) < 40 or struct.unpack_from('<I', data, 0)[0] != 0x4a695444:
            print('!! %s: not a jitdump file' % path)
            continue
        off = struct.unpack_from('<I', data, 8)[0]
        while off + 16 <= len(data):
            kind, size, ts = struct.unpack_from('<IIQ', data, off)
            if size < 16 or off + size > len(data):
                break
            if kind == 0:  # JITCodeLoad
                pid, tid, vma, addr, codesize, index = struct.unpack_from('<IIQQQQ', data, off + 16)
                name_end = data.index(b'\0', off + 56)
                name = data[off + 56:name_end].decode('utf-8', 'replace')
                code = data[name_end + 1:name_end + 1 + codesize]
                prev = loads.get(addr)
                if not prev or prev[0] <= ts:  # code is freed and reused: the newest load wins
                    loads[addr] = (ts, addr + codesize, name, code)
            off += size
    JIT_MAP[:] = sorted((a, v[1], v[2], v[3]) for a, v in loads.items())
    JIT_STARTS[:] = [m[0] for m in JIT_MAP]
    print('jitdump: %d code loads from %d file(s)' % (len(JIT_MAP), len(paths)))


def jit_name(address):
    import bisect
    i = bisect.bisect_right(JIT_STARTS, address) - 1
    if i >= 0 and JIT_MAP[i][0] <= address < JIT_MAP[i][1]:
        return re.sub(r'^JSC-', '', JIT_MAP[i][2])
    return None


def symbolize(elf, offsets):
    """offset (int, already -1 for return addresses) -> list of names, innermost first (inlines expanded)."""
    result = {}
    if not offsets:
        return result
    offsets = sorted(offsets)
    proc = subprocess.run([SYMBOLIZER, '--obj=' + elf, '--output-style=JSON', '--demangle', '--inlines' if INLINES else '--no-inlines', '--functions=linkage'],
                          input='\n'.join('0x%x' % o for o in offsets), capture_output=True, text=True)
    for line in proc.stdout.splitlines():
        try:
            entry = json.loads(line)
        except ValueError:
            continue
        address = int(entry.get('Address', '0x0'), 16)
        names = [s.get('FunctionName') or '??' for s in entry.get('Symbol', [])]
        result[address] = [shorten(n) for n in names] or ['??']
    return result


def shorten(name):
    # Drop argument lists and template arguments for readability.
    out, depth = [], 0
    for c in name:
        if c in '<(':
            if depth == 0:
                out.append('<>' if c == '<' else '()')
            depth += 1
        elif c in '>)':
            depth = max(0, depth - 1)
        elif depth == 0:
            out.append(c)
    s = ''.join(out).replace('()', '').replace('<>', '')
    s = re.sub(r'^(\S+\s+)+(?=\S*::)', '', s) if ' ' in s and '::' in s else s
    return s.strip()[:120]


def frames_of(stack, sym, adjust, llint=(0, 0)):
    """Innermost-first list of function names for one sampled stack."""
    out = []
    for i, pc in enumerate(stack):
        if pc.startswith('j'):
            tier, _, addr = pc.partition(':')
            label = TIERS.get(tier, 'JIT: unknown tier')
            if JIT_MAP and addr:
                name = jit_name(int(addr, 16) - (1 if i else 0))
                label += ' | ' + (name or '(not in the jitdump)')
            out.append(label)
        elif pc.startswith('x'):
            if not out or out[-1] != '(other module)':
                out.append('(other module)')
        else:
            off = int(pc, 16) + adjust - (1 if i else 0)
            if llint[0] <= int(pc, 16) < llint[1]:
                out.append('llint_ (LLInt)')
            else:
                out.extend(sym.get(off, ['0x%x' % off]))
    return out


def classify(frames, table, outermost=False):
    seq = reversed(frames) if outermost else frames
    for f in seq:
        if f == '(other module)' and not outermost:
            continue  # libc/libkernel leaves count for their caller
        for name, rx in table:
            if rx.search(f):
                return name
    return None


IDLE_CALLER = re.compile(r'g_main_context_(iterate|poll|prepare|check)|g_poll|WTF::RunLoop::run|g_main_loop_run|runGLibMainLoop')


def blocked(frames):
    if not frames or frames[0] != '(other module)':
        return False
    if len(frames) > 1 and IDLE_CALLER.search(frames[1]):
        return True
    return any(BLOCKED.search(f) for f in frames[:8])


def thread_label(name, frames_counter):
    if name and name != '-':
        return name
    roots = collections.Counter()
    for frames, c in frames_counter.items():
        joined = ' '.join(frames[-6:])
        if 'WebProcessMain' in joined or 'NetworkProcessMain' in joined or 'runWebKit' in joined:
            return 'main (WebKit)'
        roots[frames[-1] if frames else '?'] += c
    return 'unnamed (%s)' % (roots.most_common(1)[0][0][:40] if roots else '?')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('files', nargs='+')
    here = os.path.dirname(os.path.abspath(__file__))
    ap.add_argument('--elf', default=os.path.join(here, '../../../../toolchain/wpe-webkit/build/bin/ps5wk-child'))
    ap.add_argument('--folded')
    ap.add_argument('--top', type=int, default=15)
    ap.add_argument('--from', dest='lo', type=float)
    ap.add_argument('--to', dest='hi', type=float)
    ap.add_argument('--threads', type=int, default=4, help='detail for this many busiest threads per process')
    ap.add_argument('--inlines', action='store_true', help='expand inlined frames (bare names with -gline-tables-only)')
    ap.add_argument('--jitdump', nargs='+', default=[], help="JSC's jit-*.dump files: name JIT frames by function")
    ap.add_argument('--code-dir', help='write the hottest JIT functions\' machine code here (needs --jitdump)')
    args = ap.parse_args()
    global INLINES
    INLINES = args.inlines
    if args.jitdump:
        load_jitdump(args.jitdump)
    areas = [(n, re.compile(r)) for n, r in AREAS]
    phases = [(n, re.compile(r)) for n, r in PHASES]
    marker_link = None
    out = subprocess.run([NM, args.elf], capture_output=True, text=True).stdout
    m = re.search(r'^([0-9a-f]+) T ps5wk_profiler_start$', out, re.M)
    if m:
        marker_link = int(m.group(1), 16)
    folded = open(args.folded, 'w') if args.folded else None
    for path in args.files:
        header, windows, threads, stacks = parse(path, args.lo, args.hi)
        adjust = 0
        if marker_link is not None and 'marker' in header:
            adjust = marker_link - int(header['marker'], 16)
            if adjust:
                print('!! %s: marker differs from %s by %+#x: wrong ELF? (adjusting)' % (path, args.elf, adjust))
        offsets = set()
        for (_, stack) in stacks:
            for i, pc in enumerate(stack):
                if not pc.startswith(('j', 'x')):
                    offsets.add(int(pc, 16) + adjust - (1 if i else 0))
        sym = symbolize(args.elf, offsets)
        kept = [w for w in windows if w['keep']]
        wall = (kept[-1]['ms'] - kept[0]['ms'] + float(header.get('window_ms', 1000))) if kept else 0
        interval = float(header.get('interval_us', 1000)) / 1000.0
        total = sum(stacks.values())
        print('=' * 100)
        print('%s: %s pid %s, mode %s, %.0f ms of sampled CPU over %.1f s%s%s' % (
            os.path.basename(path), header.get('tag'), header.get('pid'), header.get('mode'), total, wall / 1000.0,
            ', %d dropped' % windows[-1]['dropped'] if windows and windows[-1]['dropped'] else '',
            ', %d signals (%d to idle threads)' % (windows[-1]['signals'], windows[-1]['idle_signals']) if windows and windows[-1].get('signals') else ''))
        if len(kept) >= 2:
            pcpu = kept[-1]['cpu'] - kept[0]['cpu']
            print('process CPU %.0f ms = %.2f cores over the window range' % (pcpu, pcpu / max(1.0, kept[-1]['ms'] - kept[0]['ms'])))
        if windows and windows[-1]['jit']:
            j = windows[-1]['jit'] + [0] * 16
            print('JIT: allocations other/Baseline/DFG/FTL %d/%d/%d/%d, %d refused; compiles fail/Baseline/DFG/FTL/cancel %d/%d/%d/%d/%d; '
                  'compile ms Baseline/DFG/FTL %d/%d/%d; threshold multiplier %.2f' % tuple(j[:13] + [j[13] / 1000.0]))
        llint = tuple(int(x, 16) for x in header.get('llint', '0-0').split('-'))
        # Per thread (values: milliseconds of CPU).
        per_thread = collections.defaultdict(collections.Counter)
        for (idx, stack), c in stacks.items():
            per_thread[idx][tuple(frames_of(stack, sym, adjust, llint))] += c
        names = {}
        for (idx, gen), t in threads.items():
            names[idx] = t['name']
        cpu_by_thread = collections.Counter()
        for (idx, gen), t in threads.items():
            vals = [(ms, cpu) for ms, cpu in t['cpu'] if cpu >= 0 and (args.lo is None or ms >= args.lo) and (args.hi is None or ms <= args.hi + 1000)]
            if len(vals) >= 1:
                first = [cpu for ms, cpu in t['cpu'] if cpu >= 0 and args.lo is not None and ms < args.lo]
                base = first[-1] if first else 0.0
                cpu_by_thread[idx] += max(0.0, vals[-1][1] - base)
        labels = {idx: thread_label(names.get(idx, '-'), per_thread.get(idx, {})) for idx in set(names) | set(per_thread)}
        print('-- threads (CPU: last value the thread read from its own clock; sampled: CPU ms the samples stand for)')
        rows = sorted(labels, key=lambda i: -max(cpu_by_thread.get(i, 0), sum(per_thread.get(i, {}).values())))
        for idx in rows[:20]:
            s = sum(per_thread.get(idx, {}).values())
            cpu = cpu_by_thread.get(idx, 0)
            if cpu < 5 and s < 5:
                continue
            print('  %-34s %8.0f ms CPU (%5.1f%% of a core)  %8.0f ms sampled' % (labels[idx][:34], cpu, 100.0 * cpu / wall if wall else 0, s))
        for idx in sorted(per_thread, key=lambda i: -sum(per_thread[i].values()))[:args.threads]:
            fc = per_thread[idx]
            n = sum(fc.values())
            if n < 20:
                continue
            print('-- thread %s: %.0f ms sampled' % (labels[idx], n))
            ac = collections.Counter()
            libc = 0
            for frames, c in fc.items():
                if frames and frames[0] == '(other module)':
                    libc += c
                ac['(sampled while blocked)' if blocked(frames) else (classify(frames, areas) or 'other')] += c
            print('   by area: ' + ', '.join('%s %.0f%%' % (a, 100.0 * c / n) for a, c in ac.most_common()))
            print('   leaf in libc/libkernel (counted for the caller above): %.0f%%' % (100.0 * libc / n))
            if labels[idx].startswith('main'):
                pc = collections.Counter()
                cross = collections.defaultdict(collections.Counter)
                for frames, c in fc.items():
                    ph = 'idle (sampled while blocked)' if blocked(frames) else (classify(frames, phases, outermost=True) or 'other')
                    pc[ph] += c
                    cross[ph][classify(frames, areas) or 'other'] += c
                print('   by phase:')
                for ph, c in pc.most_common():
                    detail = ', '.join('%s %.0f%%' % (a, 100.0 * k / c) for a, k in cross[ph].most_common(5))
                    print('     %5.1f%%  %-38s (%s)' % (100.0 * c / n, ph, detail))
            selfc, incl = collections.Counter(), collections.Counter()
            for frames, c in fc.items():
                if frames:
                    selfc[frames[0]] += c
                for f in set(frames):
                    incl[f] += c
            print('   self: ' + '; '.join('%.1f%% %s' % (100.0 * c / n, f) for f, c in selfc.most_common(args.top)))
            print('   total: ' + '; '.join('%.0f%% %s' % (100.0 * c / n, f) for f, c in incl.most_common(args.top + 10)
                                         if not re.search(r'start_thread|threadTrampoline|wtfThreadEntryPoint|runWebKit|ProcessMain|AuxiliaryProcessMain|RunLoop::run|g_main_loop_run|g_main_context|WTF::Detail::CallableWrapper|WTF::Function', f)))
        # JavaScript tiers over all threads.
        tiers = collections.Counter()
        for idx, fc in per_thread.items():
            for frames, c in fc.items():
                if not frames:
                    continue
                a = classify(frames[:1], areas)
                if a and a.startswith('JS'):
                    tiers[frames[0].split(' | ')[0] if frames[0].startswith('JIT') else a] += c
        if tiers:
            n = sum(tiers.values())
            print('-- JavaScript by leaf (all threads, %.0f ms): %s' % (n, ', '.join('%s %.0f%%' % (k, 100.0 * v / n) for k, v in tiers.most_common())))
        # JIT functions by self time (all threads), with --jitdump.
        if JIT_MAP and tiers:
            funcs = collections.Counter()
            for idx, fc in per_thread.items():
                for frames, c in fc.items():
                    if frames and ' | ' in frames[0]:
                        funcs[frames[0]] += c
            leaf_pcs = collections.Counter()  # absolute leaf address -> ms
            for (idx, stack), c in stacks.items():
                if stack and stack[0].startswith('j') and ':' in stack[0]:
                    leaf_pcs[int(stack[0].partition(':')[2], 16)] += c
            n = sum(tiers.values())
            print('-- JIT code by function, self time (%% of JavaScript leaf time; %d functions)' % len(funcs))
            cum = 0.0
            hottest = funcs.most_common(args.top * 2)
            for rank, (f, c) in enumerate(hottest, 1):
                cum += c
                print('  %2d %5.1f%% (cum %5.1f%%)  %s' % (rank, 100.0 * c / n, 100.0 * cum / n, f[:150]))
            if args.code_dir:
                os.makedirs(args.code_dir, exist_ok=True)
                byname = {m[2]: m for m in JIT_MAP}
                for rank, (f, c) in enumerate(hottest[:args.top], 1):
                    m = byname.get('JSC-' + f.split(' | ', 1)[1])
                    if not m:
                        continue
                    base = os.path.join(args.code_dir, '%02d' % rank)
                    open(base + '.bin', 'wb').write(m[3])
                    open(base + '.txt', 'w').write('%s\nstart %#x size %d\n' % (m[2], m[0], m[1] - m[0]))
                    tools = os.path.dirname(SYMBOLIZER)
                    subprocess.run([os.path.join(tools, 'llvm-objcopy'), '-I', 'binary', '-O', 'elf64-x86-64', '--rename-section=.data=.text,code',
                                    base + '.bin', base + '.o'], check=False)
                    listing = subprocess.run([os.path.join(tools, 'llvm-objdump'), '-d', '--no-show-raw-insn', base + '.o'],
                                             capture_output=True, text=True).stdout
                    # Per instruction: % of this function's self time (offsets from its start).
                    hits = collections.Counter({a - m[0]: v for a, v in leaf_pcs.items() if m[0] <= a < m[1]})
                    total_hits = sum(hits.values()) or 1
                    with open(base + '.s', 'w') as asm:
                        asm.write('# %s\n# start %#x, %d bytes; left column: %% of its self time at that instruction\n' % (m[2], m[0], m[1] - m[0]))
                        for line in listing.splitlines():
                            mm = re.match(r'\s*([0-9a-f]+):', line)
                            h = hits.get(int(mm.group(1), 16), 0) if mm else 0
                            asm.write(('%6.2f ' % (100.0 * h / total_hits) if h else '       ') + line + '\n')
                    # The hottest 64-byte stretches, for a quick look.
                    spans = collections.Counter()
                    for off, v in hits.items():
                        spans[off // 64 * 64] += v
                    open(base + '.txt', 'a').write('hottest 64-byte stretches (offset: %% of self time): %s\n' % ', '.join(
                        '%#x: %.1f%%' % (o, 100.0 * v / total_hits) for o, v in spans.most_common(12)))
                print('   machine code of the top %d in %s' % (args.top, args.code_dir))
        # Timeline: process cores and the busiest threads per window.
        if len(windows) > 1:
            top = [i for i in rows[:4]]
            print('-- timeline (cores per window: process | ' + ' | '.join(labels[i][:16] for i in top) + ')')
            prev = None
            line = []
            for w in windows:
                if prev is not None:
                    dt = w['ms'] - prev['ms']
                    vals = ['%.2f' % ((w['cpu'] - prev['cpu']) / dt)]
                    for i in top:
                        a = [v for (k, g), v in w['threads'].items() if k == i]
                        b = [v for (k, g), v in prev['threads'].items() if k == i]
                        vals.append('%.2f' % ((a[0] - b[0]) / dt) if a and b else ('  - ' if not a else '%.2f' % (a[0] / dt)))
                    line.append('%5.0fs %s' % (w['ms'] / 1000.0, '/'.join(vals)))
                prev = w
            for k in range(0, len(line), 4):
                print('   ' + '   '.join(line[k:k + 4]))
        if folded:
            for idx, fc in per_thread.items():
                for frames, c in fc.items():
                    folded.write('%s;%s;%s %d\n' % (header.get('tag', '?'), labels[idx].replace(';', ','),
                                                   ';'.join(f.replace(';', ',') for f in reversed(frames)), round(c * 1000)))
    if folded:
        folded.close()


if __name__ == '__main__':
    main()
