# Spike 10: a direct-memory heap in a PS5 title (R8)

**Question:** a registered title has only ~440 MB of flexible (CPU) memory
but gigabytes of direct memory. The browser's title also has to host the
hardware video decoder, whose CPU part is flexible memory (223 MB for 8K
HEVC at depth 4, Spike 9). Can the title's own malloc heap live in direct
memory, so the decoder gets the flexible memory?

**Answer (measured on the console, FW 13.60, 2026-10-28):** yes.
- A 16 GiB reservation at 0x1000000000 is granted. 64 × 64 MB of main
  direct memory (type 12) mapped CPU read-write over it commits 4 GB in
  19 ms, and every page reads back.
- **Flexible memory doesn't move** (440.0 MB before, at 4 GB committed and
  after). The same holds for the heap at 1.8 GB mapped and for 64 thread
  stacks in direct memory.
- dlmalloc over that page source, with malloc and its family `--wrap`ped,
  serves C, libc++ `new`/`delete` and glib's `g_malloc`. It passed an
  8-thread stress test with 1.55 GB live at peak, and it is 2.5× faster than
  libc's malloc on one thread and ~500× faster on eight.
- **With 1.5 GB live in the direct heap, the 8K HEVC decoder at depth 4 runs
  at 92.3 fps** (Spike 9: 93.4). Flexible memory goes 440 → 215 MB with the
  decoder open, and the heap is untouched.
- **The binary's size comes out of flexible memory, 1:1, at load time.** A
  build padded with 100 MB of text and rodata starts with 340 MB instead of
  440 MB. The browser's eboot (114 MB of segments today) will start near
  330 MB.
- **libc's thread stacks are flexible memory, and they stay taken after the
  threads end.** 64 threads with 2 MB libc stacks took 128 MB, and the
  128 MB was still gone after they were joined.

## What was built

| File | What |
| --- | --- |
| `dheap.c`, `dmem.h` | The heap. Page source: one 16 GiB range reserved at the first allocation (hint 0x2000000000). A segment is main direct memory (type 12) mapped CPU RW at a fixed address in the range, first fit, with a 64 KiB hole after each one so every unmap dlmalloc asks for is exactly one segment. Allocator: dlmalloc mspaces, 8 arenas (round robin per thread, through a pthread key), `FOOTERS` so a free from any thread finds the block's arena. A pointer is the heap's iff it lies in the range; anything else goes back to libc (`__real_*`) |
| `dlmalloc/malloc.c` | dlmalloc 2.8.6, Doug Lea, **MIT-0** (2023 re-license of the CC0 release), unmodified, fetched from gee.cs.oswego.edu (`dlmalloc/SOURCE` has the sha256) |
| `title_dmem.c` | The tests (below). `-DPAD` builds the padded variant |
| `cxx_test.cpp`, `glib_test.c` | C++ and glib allocations checked with `dheap_owns()` |
| `pad.S` | 50 MB of text (each 4 KiB page: 4095 NOPs and a RET) + 50 MB of random rodata, `.incbin`'d from `build/` |
| `build-title.sh` | Builds both titles (PPSA99015). Packaging follows `spikes/02-gl-egl/ps5-opengl/package-title.sh` without ps5-opengl: its `app_heap.c` would claim the malloc wraps. It reuses Spike 9's `vdec2.c`/`au.c` from `spikes/09-hw-decode` (compiled, not copied) |
| `run.sh [--pad]` | Uploads and launches through `tools/launch.sh --install`; the log goes to `tools/logserver.sh` |

Link flags for the heap: `--wrap=` for `malloc`, `calloc`, `realloc`,
`free`, `posix_memalign`, `aligned_alloc`, `memalign` and
`malloc_usable_size`. glib also needs `libps5compat`'s wraps
(`readlink eaccess faccessat link symlink ppoll`) and a no-op
`__lsan_ignore_object`, a weak reference the title converter refuses to
leave unresolved.

## Results

Full log: `evidence/2026-10-28/10-title-dmem.log`. All lines are from run 3
(final test order) except `binary-size`, which comes from the padded build.

```
RESULT 10-title-dmem start PASS flexible available 440.0 MB at main (440.0 MB at the first constructor), configured 448.0 MB (rc 0x0); direct memory 11712 MB, largest free block 11708 MB (11710 MB at the first constructor); heap mapped 0.0 MB before main
RESULT 10-title-dmem reserve PASS 16 GiB at 1000000000 (FIXED|NO_OVERWRITE, 16 KiB): rc 0; a second reservation inside it: rc 0x8002000c; flexible 440.0 MB after
RESULT 10-title-dmem commit PASS prot 0x03 map rc 0x0, 0xf2 map rc 0x0 -> using 0x3; 64 x 64 MB = 4096 MB committed in 19 ms (stopped at the 4 GB cap), mprotect rc 0x0; every 4 KiB written and read back: 0 bad; fresh pages non-zero: 0 of 262144; flexible 440.0 MB before, 440.0 MB with 4096 MB committed; largest free direct block 11708 -> 7612 MB
RESULT 10-title-dmem decommit PASS 64 chunks: munmap rc 0x0, release rc 0x0 in 1 ms; the VA was free again after munmap for 64 of 64 (re-reserved); recommit at the same address rc 0x0, 4096 of 4096 pages non-zero before writing (old data?), 0 bad after; partial: unmap+release the last 16 MB of a 64 MB commit rc 0x0 / 0x0, first 48 MB still fine (0 bad), release of the rest rc 0x0; largest free direct block 11708 MB
RESULT 10-title-dmem flexible-unchanged PASS flexible 440.00 MB before, 440.00 MB with 4096 MB of direct memory committed (+0 KB), 440.00 MB after decommit
RESULT 10-title-dmem cxx PASS 7 of 7 C++ allocations in the direct heap (new[], make_unique, vector growth, string, make_shared, unordered_map node, aligned new of alignas(256): aligned yes); values correct
RESULT 10-title-dmem glib PASS 6 of 6 glib allocations in the direct heap (glib 2.88.4: g_malloc, g_malloc0 1 MB, g_realloc, g_strdup_printf, GHashTable, GString); values correct
RESULT 10-title-dmem stress PASS 8 threads, 16 B-4 MB: 325817 ops in 464 ms (ramp to 1.5 GB in 41 ms): 91352 allocs (9133 posix_memalign, 9237 calloc), 4357 reallocs, 91352 frees (80477 cross-thread); peak live 1551 MB, heap mapped peak 1811 MB, 0 bad, 0 failed/not in heap, 0 libc fallbacks; 4719 blocks left at the end (1538 MB live) freed by main; heap mapped 32 MB after free+trim (16 segments); flexible 440.0 MB before, min 440.0 MB during, 440.0 MB after
RESULT 10-title-dmem decoder PASS 1536 MB live in the direct heap (4884 blocks, 4884 in the heap) + the 141 MB clip: 8K HEVC depth 4 up in 75 ms, 929 AUs -> 929 pictures in 10063 ms = 92.3 fps, 0 errors (first 0x00000000), 0 flagged; decoder asked 223 MB flexible; flexible 440.0 MB at start, 440.0 MB with the heap, 440.0 MB with the clip, 215.0 MB decoder open, 215.0 MB after 10 s, 438.0 MB after close; heap blocks intact: 0 bad
RESULT 10-title-dmem stacks PASS 64 threads on 2 MB direct-memory stacks (pthread_attr_setstack, 64 KiB guard), 1 MB of each touched, all alive at once: OK, every local inside its own stack (create rc 0x0); flexible 438.0 MB before, 438.0 MB with them alive, 438.0 MB after. Same with libc stacks of 2 MB: OK (rc 0x0), flexible 310.0 MB alive (128.0 MB used), 310.0 MB after
RESULT 10-title-dmem throughput PASS malloc+free pairs, 16 B-64 KB, 1024 live per thread: direct heap 6.94 M/s vs libc 2.75 M/s on 1 thread (400k); 12.86 vs 0.22 M/s on 2 threads (2 x 100k); 21.08 vs 0.04 M/s on 8 threads (8 x 50k); flexible 310.0 -> 310.0 MB (libc's heap grew by 0.0 MB)
RESULT 10-title-dmem jit PASS map with RWX rc 0x80020016, with RX rc 0x80020016; RW map + mprotect RX rc 0x0, call -> 43 (want 43); mprotect RW rc 0x0, patch, RX rc 0x0, call -> 101 (want 101); mprotect RWX rc 0x0
RESULT 10-title-dmem binary-size PASS padded build: 50.0 MB text pad + 50.0 MB rodata pad (12800 text pages run, rodata sum 0x18d6e4); flexible at first constructor 340.0 MB, at main 340.0 MB, after running the text pad 340.0 MB, after the rodata pad 340.0 MB (12 ms)
```

### What the numbers say

**1. Start.** 440 MB of flexible memory is available at the first
constructor and at main. `sceKernelConfiguredFlexibleMemorySize` reports
448 MB. Spike 9's title started at 419 MB; it also linked ps5-opengl,
whose code and data are about 20 MB. Direct memory is 11712 MB.

**2. Reserve, commit, decommit.**
- `sceKernelReserveVirtualRange(&a, 16 GiB, 0x10|0x80, 16 KiB)` at
  0x1000000000 is granted at the hint. A second reservation inside it is
  refused with 0x8002000c (EBUSY), so NO_OVERWRITE protects the range.
- **Both protections work:** 0x03 (CPU RW) and 0xf2 (CPU+GPU RW) map with
  rc 0. The 0xf2 fallback was never needed. `sceKernelAllocateMainDirectMemory`
  and the ranged `sceKernelAllocateDirectMemory` both work.
- The 4 GB cap was reached with nothing refused. 7.6 GB of direct memory
  was still free in one block, so far more than 4 GB can be committed.
- **`sceKernelMunmap` frees the address range** (64 of 64 times the range
  could be reserved again right after). This contradicts prospero-win's
  note that the range stays reserved. A page source must reserve the hole
  again after each unmap, or another mapping can land in it.
- **Released direct memory comes back dirty.** A recommit got the old
  pages back (4096 of 4096 non-zero). Pages are zero only the first time
  the pool hands them out. The heap builds dlmalloc with `MMAP_CLEARS 0`,
  so `calloc` clears.
- **Partial release works.** Unmapping and releasing the last 16 MB of a
  64 MB commit (`phys + 48 MB`) returns 0, and the first 48 MB stays
  intact. A page allocator can decommit any 16 KiB-aligned part of a
  commit.

**3. JIT.**
- Mapping with execute permission is refused: RWX and RX both return
  0x80020016 (EINVAL).
- Mapped RW and then `sceKernelMprotect` to RX (0x05) works: the copied
  `x + 42` returns 43.
- Back to RW, patched, and RX again returns 101.
- `mprotect` to RWX (0x07) also returns 0. Whether code in an RWX page
  runs was not tested.
- **Title text is execute-only.** The first padded run read its own
  `.text` and died with `SYSTEM_XO_VIOLATION` (0xa0020328, kernel log in
  the evidence). A JIT can't read or checksum code in the ELF, only in
  its own pages.

**4. Heap.**
- The heap is in place before `main` (2 MB mapped by then).
- libc++ and glib land in it with no source changes, because `--wrap`
  reaches into static archives.
- Stress test: 8 threads, 326k operations, 1551 MB live at peak, 1811 MB
  mapped, 80k cross-thread frees. 0 corrupt blocks, 0 libc fallbacks.
  After the free and trim, 32 MB stays mapped, the arenas' first segments.
- **libc's malloc collapses under contention:** 2.75 M pairs/s on one
  thread, 0.22 on two, 0.04 on eight, against 21 M/s for the direct heap.
  Moving the heap also fixes a performance problem.
- The SDK's `__thread` is emulated TLS: `__emutls_get_address` calls
  `malloc`. The first build kept the thread's arena in a `__thread` and
  recursed until the stack overflowed (SIGSEGV, kernel log in the
  evidence). The heap uses a pthread key instead.

**5. Decoder next to the heap.**
- 1.5 GB live in 4884 heap blocks, plus the 141 MB clip read into a heap
  buffer, leaves flexible memory at 440 MB.
- The decoder at depth 4 takes its 223 MB (215 MB left) and decodes 8K
  HEVC at **92.3 fps**, with 0 errors, 0 flagged pictures and every heap
  block intact.
- Run 2 ran the libc-stack test first. That test had already taken 128 MB
  of flexible memory, so the decoder started at 312 MB and left 87 MB:
  same speed, 92.1 fps (evidence).

**6. Binary size.**
- The padded build's segments are 99.3 MB larger: 50.3 MB of text and
  50 MB of rodata, against 0.35 MB and 1.0 MB in the normal build.
- It starts with **100 MB less flexible memory** (340 vs 440 MB), already
  at the first constructor.
- Running every text page and reading every rodata page changes nothing.
- So the loader charges the whole image to flexible memory up front, text
  and rodata alike.
- The browser's title ELF today (`toolchain/wpe-webkit/title-work/llvm-pie.elf`)
  has 73.4 MB of text, 24.9 MB of rodata, 10.4 MB of RW data and 4.9 MB
  of bss: about 112 MB more than this spike, so it should start near
  **328 MB**.

**7. Thread stacks.**
- 64 threads on 2 MB direct-memory stacks (`pthread_attr_setstack`, with
  an unmapped 64 KiB guard below each one) all ran at once. Each touched
  1 MB of its stack, and every thread's locals were inside its own stack.
  Flexible memory was unchanged.
- The same 64 threads with libc's 2 MB stacks took 128 MB of flexible
  memory and **didn't give it back after `pthread_join`**. libkernel
  appears to cache the stacks of finished threads, as FreeBSD's libthr
  does. Flexible memory stayed at 310 MB to the end of the run.

## Recommendation for the browser title (PPSA99020)

The flexible budget, measured:

| Item | MB |
| --- | --- |
| Title start | 440 |
| Browser image (text + rodata + data + bss) | −112 |
| 8K HEVC decoder, depth 4 / depth 2 | −223 / −139 |
| **Left, depth 4 / depth 2** | **~105 / ~190** |

Everything else has to stay out of flexible memory:

1. **malloc family → direct heap.** Replace ps5-opengl's `app_heap.c` in
   `port/webkit/apps/title/package.sh`. It is compiled there separately,
   and `libPS5OpenGL.a` doesn't reference it. Use a heap like `dheap.c`,
   linked with `--wrap=malloc,calloc,realloc,free,posix_memalign,aligned_alloc,memalign,malloc_usable_size`.
   Also wrap `getline`/`getdelim` (and `reallocf`/`reallocarray`) if
   anything in WebKit or its dependencies uses them: libc's own versions
   would `realloc` a heap pointer with libc's allocator.
   - Route `free`/`realloc` by address, so libc.prx's own allocations
     (`strdup`, `asprintf`, stdio buffers) go back to libc.
   - Don't use `__thread` in the allocator (emulated TLS calls `malloc`).
2. **One page source for everything that mmaps anonymous memory.** That is
   libpas (`pas_page_malloc_try_map_pages`, `pas_page_malloc_commit`,
   `pas_page_malloc_decommit`, `pas_page_malloc_deallocate` in
   `pas_page_malloc.c`), bmalloc's `VMAllocate.h` (`vmAllocate`,
   `vmDeallocate`, `vmAllocatePhysicalPages`, `vmDeallocatePhysicalPages`)
   and WTF's `OSAllocatorPOSIX.cpp` (JSC's JIT pool and Gigacage reserves).
   Today all of them are `mmap(MAP_ANON)`, which is flexible memory. Or
   build with `USE_SYSTEM_MALLOC=ON`, so FastMalloc goes through the
   wrapped malloc. The exact calls (all rc 0 on 13.60):
   - **reserve:** `void *a = (void *)0x2000000000; sceKernelReserveVirtualRange(&a, 16ull << 30, 0x10 /*FIXED*/ | 0x80 /*NO_OVERWRITE*/, 0x4000)`.
     If that fails, retry with flags 0, which lets the kernel choose.
   - **commit** `[p, p+len)` (16 KiB multiples; the heap uses 64 KiB):
     `sceKernelAllocateMainDirectMemory(len, 0x10000, 12, &phys)`, then
     `sceKernelMapDirectMemory(&p, len, 0x03, 0x10 /*FIXED*/, phys, 0x10000)`.
     Keep `p → phys`.
   - **decommit** any 16 KiB-aligned part of a commit:
     `sceKernelMunmap(p, len)`, then
     `sceKernelReserveVirtualRange(&p, len, 0x90, 0x4000)`
     (munmap frees the VA), then
     `sceKernelReleaseDirectMemory(phys + (p - commit_base), len)`.
   - **Recommitted pages are not zero.** libpas and bmalloc assume a fresh
     commit reads as zero, so the page source must `memset` recommitted
     pages, or report them as dirty. Only first-use pages are zero.
   - **JIT pages:** commit RW, write, `sceKernelMprotect(p, len, 0x05)`.
     Execute permission is refused at map time, but granted by
     `mprotect`.
3. **Thread stacks → direct memory.** Use `--wrap=pthread_create` (plus
   `pthread_join`/`pthread_detach` to free the stack). A thread created
   without its own stack gets a commit from the page source, with an
   unmapped guard below it, through `pthread_attr_setstack`. WebKit starts
   dozens of threads, and every libc stack is flexible memory that is
   never returned.
4. **Keep the image small**, since every MB of text, rodata and data is a
   MB of flexible memory: `--gc-sections`, no unused ports or features,
   and maybe `-Os` for cold code. Plan for **depth 2 (139 MB)** for 8K
   unless the image shrinks or other flexible users turn out to be small.
   The decoder's own speed at depth 4 is unaffected by a large heap.

## Licenses and prior art

- dlmalloc: MIT-0 (vendored, unmodified).
- Compiled from where they are, not copied into this folder: Spike 9's
  `vdec2.c` (derived from PS5VR, GPL-3.0-or-later) and the title runtime
  from `ref/` (PS5VR's `app_crt.cpp`, ps5-opengl's `runtime_shims.c`;
  GPL-3.0-or-later), as every spike title does.
- Read for prior art only, nothing copied (checkouts in gitignored `ref/`):
  - prospero-win (LGPL-2.1+): `docs/WINE_PS5_BUILD.md` "Direct memory"
    and `wine/ps5/pw_wine_dmem_ps5.c`.
  - mihawk-99/PS5_PayloadSDK (GPL-3.0-or-later): `platform/src/heap.c`
    and `platform/include/ps5platform/heap.h`. The requested revision
    95c08f2 isn't in the repository; HEAD 3e31e0b was read.
  - PS5_Vulkan (GPL-3.0-or-later): `tools/radv-link.sh`.
- Re-verified on 13.60 against that prior art:
  - ~440 MiB flexible: matches.
  - 16 GiB at 0x1000000000 granted: matches.
  - Type 12 and fixed map over a reservation: works.
  - Exec refused at map time, granted by mprotect: matches.
  - The 0xf2 fallback: not needed, since 0x03 works.
  - "The range stays reserved after munmap": **does not hold**; munmap
    frees the VA, as mihawk's heap assumes when it re-reserves.

## Reproduce

```sh
. ./env.sh
spikes/09-hw-decode/run.sh spikes/09-hw-decode/jobs/first.txt --no-launch   # only if the 8K clip isn't on the console yet
spikes/10-title-dmem/build-title.sh
spikes/10-title-dmem/run.sh          # ~25 s; log in tools/logserver.sh
spikes/10-title-dmem/run.sh --pad    # padded build, ~3 s
```
