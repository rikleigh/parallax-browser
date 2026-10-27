# Spike 4: JavaScriptCore `jsc` for PS5 (ps5-payload-sdk, JIT off)

**Result: `jsc` links as a PS5 payload ELF.** It is the JSCOnly port, built statically, with the asm LLInt and no JIT. bmalloc/libpas is used and ICU 76.1 is statically linked. No link stubs were needed. The only shim is `memmem`. Nothing has run on a console yet: everything below the line "device" is still unverified.

| artifact | size (stripped) | notes |
|---|---|---|
| `build/jsc-ps5/bin/jsc.elf` | 49.3 MB | full ICU data (31.9 MB of `.rodata`) |
| `build/jsc-ps5/bin/jsc-slim.elf` | 35.5 MB | slim ICU data (18.1 MB), see below |
| `build/jsc-ps5-wasm/bin/jsc.elf` | 50.8 MB | same with `ENABLE_WEBASSEMBLY=ON` (IPInt only) |
| `build/jsc-ps5-wasm/bin/jsc-slim.elf` | 37.0 MB | |

The code is 13.7 MB of `.text` (JSC + WTF + libpas + ICU code + libc++). The ELF is a static PIE. Its NEEDED entries are `libkernel_web.sprx`, `libSceLibcInternal.sprx` and `libSceNet.sprx`, all resolved by the SDK's crt/rtld at load time. It imports 183 symbols, and every one is present in the SDK's stubs.

## Configuration

* WebKit `15486a01` (2026-10-27) with `-DPORT=JSCOnly -DENABLE_STATIC_JSC=ON`.
* `ENABLE_JIT=OFF ENABLE_DFG_JIT=OFF ENABLE_FTL_JIT=OFF ENABLE_C_LOOP=OFF` gives the **asm LLInt**, not the CLoop. YARR JIT is off as a consequence and the assembler is compiled but never used to emit code. The CLoop fallback was not needed; it is available with `C_LOOP=ON build.sh jsc-ps5`.
* `ENABLE_WEBASSEMBLY=OFF` in the main build. A `WASM=ON` variant also builds and links (`BUILD_SUFFIX=-wasm`), with BBQ/OMG off, so wasm runs in **IPInt**, the in-place interpreter. The 1253 `ipint_*` symbols are present. System JSC on macOS with `--useJIT=0` runs `js/wasm.js` correctly, so JIT-less wasm is a supported JSC mode.
* `ENABLE_SAMPLING_PROFILER=OFF ENABLE_REMOTE_INSPECTOR=OFF ENABLE_RESOURCE_USAGE=OFF`, `DEVELOPER_MODE=OFF`, Release.
* **Allocator: bmalloc + libpas** (`USE_SYSTEM_MALLOC=OFF`). Enabling it took one line in `BPlatform.h`, because libpas already has FreeBSD code paths (`_umtx_op` futex, `MADV_FREE` and so on). Gigacage is off, which upstream does for every non-Darwin/Linux OS. `SYSTEM_MALLOC=ON` remains a fallback.
* Toolchain: SDK `prospero-clang` (Homebrew clang 18.1.8, `x86_64-sie-ps5`) using `prospero-cmake`'s toolchain file. Two private additions are in `toolchain/`:
  * **libc++/libc++abi/libunwind 19.1.7** in `toolchain/libcxx19/install`. The SDK ships libc++ 18, which has no `std::atomic_ref`, and `WTF/Atomics.h` needs it. This copy is built with the same flags as the SDK's `libcxx.sh` and used through `-nostdinc++ -isystem …` and full archive paths. The SDK sysroot is left unchanged.
  * **ICU 76.1** in `toolchain/icu/install-ps5`.
* Extra flags are `-ffunction-sections -fdata-sections`, `-Wl,--gc-sections` and `-include ps5shim.h`.

### Platform identity: FreeBSD, not PLATFORM(PLAYSTATION)

For `x86_64-sie-ps5`, clang predefines `__FreeBSD__=9`, `__SCE__`, `__PROSPERO__`, `__ELF__` and `__unix__`. Two consequences follow:

* `wchar_t` is 16-bit (`__clang_wide_literal_encoding__ "UTF-16"`, `__WCHAR_MAX 0xffff` under `__SCE__` in the SDK headers).
* The SDK's `machine/param.h` has `PAGE_SHIFT 14`, i.e. 16 KiB pages.

WebKit's own PlayStation port (`OptionsPlayStation.cmake`, `PLATFORM(PLAYSTATION)`) targets Sony's official SDK, with private libraries such as `memory-extra` and `libtestwrappers`. bmalloc and libpas auto-select that platform from `__SCE__`. This build instead defines `PS5_PAYLOAD_SDK=1` from `OptionsJSCOnly.cmake` whenever the SDK toolchain file sets `PS5`. That keeps the PlayStation platform off, so WTF, JSC and libpas take their **OS(FREEBSD)** paths. The open SDK's userland is FreeBSD-like (FreeBSD headers, libkernel pthreads, `_umtx_op`, `sysctl`), so these paths fit far better. `__SCE__` must **not** be undefined, because SDK headers key `wchar_t` and `PAGE_SIZE` off it.

## Running it on the console (device runs are for the user)

ICU data is linked into the binary, so the payload is a single self-contained file.

`run-on-ps5.sh` has two modes:

* **Default `stdin` mode.** The script streams `jsc.elf` followed by the JS file to elfldr on port 9021. Current `ps5-payload-dev/elfldr` reads exactly `e_shoff + e_shnum*64` bytes of ELF; for these ELFs that equals the file size, which I checked. It then `dup2`s the socket onto the payload's stdin, stdout and stderr. `PS5Main.cpp` therefore reads the remaining bytes, the script, from stdin until EOF and runs `jsc -e <source>`. Output comes back on the same socket. If the first line of a script is `//jsc: --opt=… …`, those words become extra jsc options; `JSC_OPTS=…` adds that line for you. I loopback-tested the framing (ELF prefix byte-identical, then the script).
* **`JSC_MODE=uri`.** You first copy `jsc.elf` and `js/*.js` to `/data/jsc` yourself, for example via an FTP payload. The script then sends `file:///data/jsc/jsc.elf?args=…`, and current elfldr passes these as real argv. `/data/jsc/args.txt`, if present, is another argv source.

`PS5Main.cpp` also does the following:

* It makes stdout line-buffered.
* It logs `pagesize` and the `pthread_attr_get_np` stack bounds of the main thread and the jsc thread to stderr.
* It runs `jscShellMain` on a **16 MiB pthread**, not the payload's main thread, so that WTF::StackBounds sees a thread with a known stack.

```sh
spikes/04-jsc/run-on-ps5.sh                         # hello.js, correctness.js, bench.js
JSC_VARIANT=slim spikes/04-jsc/run-on-ps5.sh        # 35 MB ELF
JSC_VARIANT=wasm spikes/04-jsc/run-on-ps5.sh        # + js/wasm.js
JSC_OPTS="--useConcurrentGC=0 --numberOfGCMarkers=1" spikes/04-jsc/run-on-ps5.sh js/bench.js   # "safe mode"
```

Logs go to `spikes/04-jsc/results/<timestamp>-<host>.log`. A `PS5_HOST` or `PS5_PORT` given on the command line overrides `env.local.sh`.

**Old elfldr caveat:** `ref/elfldr`, the old john-tornblom one, reads until EOF and caps payloads at 16 MiB. With that loader neither stdin mode nor these ELF sizes work. Even without ICU data the code is about 17 MB. Use the current elfldr from ps5-payload-dev, or a `file://` launch.

## JS test files (`js/`)

* `hello.js`: first light.
* `correctness.js`: 30 quick checks covering numbers and strtod, strings and unicode, RegExp (unicode, named groups), typed arrays and endianness, classes, Proxy, Map/Set/WeakMap, generators, deep recursion and the stack-overflow RangeError, JSON, Date, BigInt, Intl via ICU (NumberFormat, DateTimeFormat, Collator, Turkish casing) and GC churn. It prints `CORRECTNESS: PASS n/n`.
* `bench.js`: a self-contained small benchmark with five kernels: richards-like scheduler, deltablue-like propagation, numeric (mandelbrot + sieve), strings/regexp/JSON, and allocation/GC. It prints ms per kernel and `BENCH TOTAL`.
* `wasm.js`: a hand-assembled wasm module (add + sum loop). It prints `WASM: PASS`, or `unavailable` when wasm is off.

## macOS interpreter baseline (optional step, done)

The same JIT-off asm-LLInt configuration was built natively: `build.sh jsc-mac`, giving `build/jsc-mac/bin/jsc` (arm64, 24 MB). It runs on an Apple M4 Max under heavy background load from other builds, so the numbers are noisy:

| run | richards | deltablue | numeric | strings | alloc | **total** |
|---|---|---|---|---|---|---|
| this build, JIT off (typical of 6 runs) | 168 | 171 | 322 | 116 | 296 | **~1.05–1.10 s** (range 1.04–1.45) |
| system JSC (macOS 15) `--useJIT=0` | | | | | | 1.17–1.44 s |
| system JSC, JIT on (reference only) | 46 | 22 | 39 | 83 | 69 | 0.19–0.26 s |

`correctness.js` passes 30/30 on the macOS build. `wasm.js` sum loop takes 125 ms with system JSC `--useJIT=0` against 7 ms with the JIT.

Expect the PS5's Zen 2 (about 3.5 GHz, x86_64 LLInt) to be roughly 1.5–3× slower than the M4 per interpreter step. That puts this bench somewhere around 2–3 s on the console. **This estimate is unverified.**

## Problems hit and fixes (running log)

| # | problem | fix |
|---|---|---|
| 1 | Top-level `CMakeLists.txt` includes `WebKitXcodeSDK` on any Darwin host and pins the compilers to Xcode clang | Skip it when a toolchain file is in use (patch 0001) |
| 2 | Configure took about 8 minutes: `DETERMINE_GCC_SYSTEM_INCLUDE_DIRS` runs `cc -v -E -x c dummy`, `prospero-clang` appends `crt1.o`, and so it "preprocesses" a binary, giving 235 MB of stderr fed into a CMake regex | Skip the probe when `CMAKE_CROSSCOMPILING` (0001). Configure now takes about 2 s on re-runs |
| 3 | JSCOnly forces `ENABLE_API_TESTS=ON` on non-Windows, which needs ThirdParty/gtest | OFF when cross-compiling, and honour `-DENABLE_API_TESTS=OFF` (0001) |
| 4 | Sparse checkout lacks `Tools/Scripts/hmaptool` (header maps) and `Source/WebGPU` (header copy) | `build.sh src` extracts these unmodified upstream files with `git archive` from ref/WebKit |
| 5 | `Source/ThirdParty/unifdef` missing | `-DUSE_SYSTEM_UNIFDEF=ON` (`/usr/bin/unifdef`) |
| 6 | ICU not found because `CMAKE_FIND_ROOT_PATH_MODE_*=ONLY` confines searches to the sysroot | Pass `ICU_INCLUDE_DIR` and `ICU_{UC,I18N,DATA}_LIBRARY_RELEASE` explicitly |
| 7 | libc++ 18 (SDK) has no `std::atomic_ref`, used by WTF/Atomics.h | Private libc++ 19.1.7 build (`build.sh libcxx`) |
| 8 | clang 18 rejects `[[clang::suppress]]` on a function (`Seconds.h` `operator<=>`) | Use the `…_ON_FUNCTION` macro variant, which is empty before clang 19 (patch 0004) |
| 9 | `__SCE__` makes bmalloc/libpas select PLAYSTATION (`memory-extra.h`, `clock_gettime_np`, `pthread_setname_np`…) | Detection is gated on `!PS5_PAYLOAD_SDK`; libpas is enabled for FreeBSD x86_64 when `PS5_PAYLOAD_SDK` is set (0002) |
| 10 | libpas has no FreeBSD `pas_get_current_monotonic_time_nanoseconds` (link error) | `clock_gettime(CLOCK_MONOTONIC_FAST)` (0003) |
| 11 | `sys/sysinfo.h` missing (WTF RAMSize/AvailableMemory, OS(FREEBSD) path) | `shim/include/sys/sysinfo.h`: `sysinfo()` via `sysctlbyname("hw.physmem")`, falling back to 4 GiB |
| 12 | The SDK's old `<sys/sysctl.h>` uses `u_int` without including `<sys/types.h>` | `shim/include/ps5shim.h` (force-included) includes `<sys/types.h>` |
| 13 | `pthread_getname_np` is exported by libkernel but not declared | Prototype added in `ps5shim.h` (used only in libpas debug logging) |
| 14 | `MemoryPressureHandlerUnix.cpp` includes `<malloc.h>` unconditionally; the SDK's version is a hard `#error` | Include it only `#if HAVE(MALLOC_TRIM)` (0003) |
| 15 | `JSCBytecodeCacheVersion.cpp` uses `ElfW()` and `dl_iterate_phdr`, both missing, and the payload isn't known to the system rtld anyway | Use the existing `__TIMESTAMP__` fallback (0003) |
| 16 | `memmem` missing from SDK libc and libSceLibcInternal (link error) | `shim/ps5shim.c` |
| 17 | **Device-only, found by review:** WTF `CeilingOnPageSize` is 4 KiB for x86_64/FreeBSD, but PS5 pages are 16 KiB, so the `RELEASE_ASSERT` in `WTF::pageSize()` would fire at startup | 16 KiB when `PS5_PAYLOAD_SDK` (0002). This also aligns `g_wtfConfig` and `MarkedBlock` to 16 KiB |
| 18 | elfldr passes no argv for raw uploads | `shell/ps5/PS5Main.cpp`: argv, then `/data/jsc/args.txt`, then the script on stdin; `jsc.cpp`'s `main` is renamed to `jscShellMain` under `__PROSPERO__` (0005) |
| 19 | (macOS baseline only) JSCOnly on a Mac host had bit-rotted: `memoryFootprint(mach_port_t)` was declared only for `PLATFORM(COCOA)`, and `OSLogPrintStream.mm` needs ARC | `#if OS(DARWIN)` (patch 0090, not needed for PS5) and `-DCMAKE_CXX_FLAGS=-fobjc-arc` |

**Stubs:** none. The link was clean once item 10 was patched and item 16 shimmed. Every libc, pthread, signal and mmap symbol JSC needs (`pthread_attr_get_np`, `pthread_attr_getstack`, `sigaction`, `pthread_kill`, `sem_*`, `_umtx_op`, `madvise`, `sysctlbyname`, `clock_gettime`, …) is in the SDK's libkernel_web or libSceLibcInternal stubs. Missing from those stubs, and not needed: `uname`, `pthread_setname_np`, `pthread_getattr_np`, `malloc_good_size`, `__cxa_thread_atexit_impl`.

## Patches (`patches/`, git diff against 15486a01; apply with `git apply`)

1. `0001-cmake-cross-compile-from-macOS-and-PS5-define.patch`: Xcode SDK skip, include-dir probe skip, API-test default, `PS5_PAYLOAD_SDK` define.
2. `0002-ps5-payload-sdk-is-FreeBSD-not-PLATFORM-PLAYSTATION.patch`: bmalloc/libpas platform detection, libpas on for PS5, 16 KiB page ceiling.
3. `0003-freebsd-portability-fixes.patch`: libpas FreeBSD monotonic clock, `malloc.h` guard, bytecode-cache-version fallback.
4. `0004-clang18-suppress-attribute-on-function.patch`.
5. `0005-jsc-shell-ps5-entry-point.patch`: `jscShellMain` rename, `PS5Main.cpp`, shell CMake.
6. `0090-macos-jsconly-baseline-only.patch`: only for the macOS baseline build.

The patch set is 14 files, +205/-13 lines. All six patches pass `git apply --check` against `ref/WebKit`, and **ref/WebKit was never modified**. The build uses a private copy at `spikes/04-jsc/build/WebKit`, a git repo with the upstream baseline committed and the patches applied as working-tree changes. `build.sh src` recreates it with `FRESH_SRC=1`.

## ICU route

* ICU 76.1 is cross-built with autotools: a host build provides the tools (`--with-cross-build`), and the target is `--host=x86_64-unknown-freebsd9` with `CC=prospero-clang`. The build is static (`--disable-shared --with-data-packaging=static --disable-dyload`). It built without changes.
* **Full data** (`libicudata.a`, 31.9 MB) is linked by default.
* **Slim data** (`build.sh icu-data-slim`, 18.1 MB, 1234 items removed) uses `icupkg --ignore-deps -r` to drop:
  * all charset converters (`*.cnv`)
  * transliterators
  * CJK/Khmer/Lao/Burmese/Thai word-break dictionaries
  * `unames.icu`
  * `lang/` and `region/` display names

  It is then re-emitted with `genccode -a gcc` as a static archive. Its cost is `Intl.DisplayNames`, dictionary-based `Intl.Segmenter` word breaking for CJK/SE-Asian text, and legacy charset conversion; JSC itself does no charset conversion.
* Other options: `--with-data-packaging=archive` plus `u_setDataDirectory("/data/jsc")` would load the data from a file and drop the ELF to about 17 MB. A proper `ICU_DATA_FILTER_FILE` build, with locale subsetting, needs the `icu4c-76_1-data.zip` sources.

## Open risks (only visible on the device)

1. **Memory model.** OSAllocatorPOSIX "reserves" with `mmap(PROT_READ|PROT_WRITE, MAP_ANON)` and relies on lazy commit. Examples are the structure heap (`tryReserveUncommittedAligned`, up to GBs, halving up to 8 times) and libpas reservations. If the PS5 kernel charges anonymous RW mappings against a small flexible-memory budget at mmap time, startup will fail or OOM. Sony's port uses `memory_extra::vss` (reserve, then commit) for this reason. Mitigations:
   * `--structureHeapSizeInKB=…` can be passed via `JSC_OPTS`.
   * The real fix is an OSAllocator and libpas page source on `sceKernelReserveVirtualRange` / `sceKernelMapFlexibleMemory` (libkernel), or `PROT_NONE` reserve with `mprotect` commit.

   This is the most likely first failure.
2. **Signals and thread suspension.** The conservative GC suspends threads with `pthread_kill(SIGUSR1)` plus a semaphore, and reads registers from the signal's `ucontext`. If that misbehaves in a payload process, try `JSC_OPTS="--useConcurrentGC=0 --numberOfGCMarkers=1"`.
3. **Stack bounds.** WTF uses `pthread_attr_get_np` + `pthread_attr_getstack`. PS5Main logs both for the main thread and the 16 MiB jsc thread. A zero or bogus result means immediate RangeErrors or crashes.
4. **`mprotect` of the config pages.** `WTF::Config::permanentlyFreeze` makes the 16 KiB `__DATA,__wtf_config` page read-only and RELEASE_ASSERTs on failure. The SDK linker script places it in a RW segment, which should be fine.
5. **Page size assumption.** 16 KiB is assumed from the SDK headers; PS5Main prints `sysconf(_SC_PAGESIZE)` to confirm. If the real value is 4 KiB, everything still works, just with 16 KiB blocks.
6. **Emulated TLS** (`-femulated-tls` from the SDK): `thread_local` goes through `__emutls_get_address`. It is functional but slower on hot paths such as libpas TLC lookup.
7. `sysctlbyname("hw.physmem")` may report total RAM rather than the process budget, so heap-growth heuristics may be optimistic.
8. elfldr version: see the old-elfldr caveat above.
9. The CPU estimate is speculative until run.

## Effort estimate to productionise (JSC part only)

* First device bring-up of this ELF, fixing what risks 1–3 throw up: **2–5 days**.
* A proper PS5 platform layer: OSAllocator and libpas page source on libkernel flexible memory, RAMSize/MemoryFootprint via the budget APIs, thread naming, crash logging to klog. Plus a clean upstream-style `PLATFORM(PS5_HOMEBREW)` or similar, instead of the ad-hoc `PS5_PAYLOAD_SDK` define. **1–2 weeks**.
* ICU data strategy (filter file plus external `.dat` on `/data`) and binary-size work (`-Os` for cold code, ICF): **2–3 days**.
* Keeping the patch set rebased on WebKit tip is ongoing. It is small today: 14 files and about 200 lines, most of it the PS5 entry point.
* Unknown, separate spike: JIT. elfldr maps the payload's text segment RWE, which hints that a jailbroken payload process *can* hold executable writable memory. If `mmap(PROT_EXEC)` or dual-mapping works for payloads, the baseline JIT and DFG might be enabled. The ratio above suggests 4× or more on this bench, and much more on long-running code. Untested.

## Reproduce

```sh
spikes/04-jsc/build.sh            # icu-host icu-ps5 icu-data-slim libcxx src jsc-ps5
WASM=ON BUILD_SUFFIX=-wasm spikes/04-jsc/build.sh jsc-ps5
spikes/04-jsc/build.sh jsc-mac bench-mac
```

Timing: on this machine, under load from other spikes, a from-scratch jsc-ps5 build took about 10 minutes, ICU about 3 and libc++ about 2. Disk use is about 1.3 GB in `spikes/04-jsc/build` (gitignored), about 0.8 GB for `toolchain/icu` and about 0.25 GB for `toolchain/libcxx19`.
