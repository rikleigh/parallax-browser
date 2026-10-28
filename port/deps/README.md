# WPE WebKit dependencies for PS5 payloads

Static libraries for WPE WebKit (Milestone 1: browsing, no media), cross-built
with ps5-payload-sdk into **`toolchain/wpe-sysroot`**. Package list, versions
and state: [STATUS.md](STATUS.md).

```sh
port/deps/build.sh               # every step, in order (4.5 min from scratch on the M4 Max)
port/deps/build.sh --list        # step names
port/deps/build.sh glib libsoup  # single steps
KEEP=1 port/deps/build.sh glib   # keep toolchain/wpe-deps/{src,build}/glib for debugging
port/deps/smoke/build.sh         # console smoke test payload
port/deps/smoke/upload-runtime.sh && port/deps/smoke/run.sh
```

Each step downloads a pinned tarball into `toolchain/wpe-deps/dl` (sha256
checked), extracts it into a new empty `toolchain/wpe-deps/src/<pkg>`,
applies `patches/<pkg>/*.patch`, builds with `-O2 -g0 -fPIC
-ffunction-sections -fdata-sections`, installs into the sysroot and deletes
the source and build trees. Host tools: Homebrew `ninja nasm bison glib`
(for `glib-mkenums` etc.), `gperf`, Python 3; meson 1.7.2 is installed into
`toolchain/wpe-deps/venv` by the `tools` step. `tools/mkpatch.sh` turns edits
in a kept source tree into a patch.

## Layout

```
toolchain/wpe-sysroot/
  include/  lib/*.a  lib/pkgconfig/*.pc  lib/cmake/  lib/gio/modules/*.a
  share/pkgconfig/  share/fontconfig/conf.avail/ (upstream snippets, reference)
  share/ps5webkit-runtime/   console runtime tree, mirrors /data/ps5webkit
```

The `.pc` files use `prefix=${pcfiledir}/../..`, so the sysroot is
relocatable and **`PKG_CONFIG_SYSROOT_DIR` must be empty**. CMake config
files in `lib/cmake` contain absolute paths.

## Using the sysroot

### CMake (WebKit)

```sh
cmake -G Ninja -S WebKit -B build \
  -DCMAKE_TOOLCHAIN_FILE=$REPO/port/deps/ps5-wpe.cmake \
  -DPORT=WPE ...
```

`ps5-wpe.cmake` includes the SDK's `prospero.cmake` (FreeBSD/x86_64,
`CMAKE_SYSROOT` = SDK target, find mode ONLY) and adds:

- `CMAKE_FIND_ROOT_PATH += toolchain/wpe-sysroot`, so `find_library`,
  `find_path` and `find_package` see `lib/`, `include/`, `lib/cmake/`;
- `PKG_CONFIG_EXECUTABLE = port/deps/pkg-config-ps5`, which sets
  `PKG_CONFIG_LIBDIR=<sysroot>/lib/pkgconfig:<sysroot>/share/pkgconfig`,
  `PKG_CONFIG_PATH=` and `PKG_CONFIG_SYSROOT_DIR=` and always passes
  `--static` (every library is an archive, so `Libs.private` is needed);
- `PS5_WPE_SYSROOT`, `PS5_LIBCXX19_DIR` variables.

Without the toolchain file, the equivalent environment is:

```sh
export PKG_CONFIG=$REPO/port/deps/pkg-config-ps5   # or:
export PKG_CONFIG_LIBDIR=$REPO/toolchain/wpe-sysroot/lib/pkgconfig:$REPO/toolchain/wpe-sysroot/share/pkgconfig
export PKG_CONFIG_PATH= PKG_CONFIG_SYSROOT_DIR=
cmake ... -DCMAKE_FIND_ROOT_PATH=$REPO/toolchain/wpe-sysroot
```

The toolchain file does not change compile flags. C++ that must use libc++ 19
(WebKit) adds `-nostdinc++ -isystem $PS5_LIBCXX19_DIR/include/c++/v1` and links
`-nostdlib++ libc++.a libc++abi.a libunwind.a` from `$PS5_LIBCXX19_DIR/lib`,
as `spikes/04-jsc/build.sh` does. C++ deps here (harfbuzz) were compiled
against libc++ 19 headers. WebKit also needs `-DLD_PACK_RELATIVE_RELOCS=OFF`
(the SDK loader ignores DT_RELR).

### Meson

```sh
meson setup build --cross-file $REPO/port/deps/ps5-cross.ini
```

Paths in it are relative to the file (`@DIRNAME@`); it sets the SDK
compilers, `-O2 -g0 -fPIC`, libc++ 19 for C++, `pkg-config-ps5`, and
`pkg_config_libdir`.

## What OptionsWPE (WebKit 15486a0178) needs, and what is here

Unconditional `find_package(... REQUIRED)`: GLib 2.70 (GioUnix, Thread,
Module), HarfBuzz 2.7.4 with ICU, ICU 70.1, JPEG, **Epoxy 1.5.4**, Soup3,
**XkbCommon 0.4.0**, LibXml2 2.9.13, PNG, SQLite3, Threads, WebP (demux),
ZLIB, Freetype 2.9. All are in the sysroot.

Option-dependent, with the settings we suggest for Milestone 1:

| WebKit option | default | needs | here |
| --- | --- | --- | --- |
| `USE_OPENSSL` (private) | OFF | OFF: libgcrypt 1.7 + libtasn1; ON: OpenSSL 3 | **use `-DUSE_OPENSSL=ON`**: OpenSSL 3.5.2 is here (glib-networking needs it anyway); libgcrypt/libtasn1 not built |
| `USE_WOFF2` | ON | libwoff2dec, unless FreeType has brotli | FreeType is built with brotli, so `WOFF2Checks` turns `USE_WOFF2` off and FreeType decodes WOFF2 |
| `ENABLE_XSLT` | ON | libxslt 1.1.13 | built |
| `USE_LCMS` | ON | lcms2 | built |
| `USE_AVIF` | ON | libavif 0.9 | **not built**: `-DUSE_AVIF=OFF` |
| `USE_JPEGXL` | ON | libjxl 0.7 | **not built**: `-DUSE_JPEGXL=OFF` |
| `ENABLE_WPE_LEGACY_API` | ON | libwpe | not built: `-DENABLE_WPE_LEGACY_API=OFF` (WPEPlatform only) |
| `ENABLE_WPE_PLATFORM_DRM` / `_WAYLAND` | ON | libinput, udev / wayland | `OFF` |
| `USE_GBM`, `USE_LIBDRM` | ON | gbm, libdrm | `OFF` (also forces `ENABLE_GPU_PROCESS` off) |
| `USE_LIBHYPHEN` | ON | libhyphen + dictionaries | `OFF` |
| `USE_LIBBACKTRACE` | ON | libbacktrace | `OFF` |
| `USE_ATK` | ON | atk, atk-bridge | `OFF` |
| `ENABLE_SPEECH_SYNTHESIS` | ON | flite or spiel | `OFF` |
| `ENABLE_SPELLCHECK` | ON | enchant | `OFF` |
| `ENABLE_JOURNALD_LOG` | ON | libsystemd | `OFF` |
| `USE_SYSPROF_CAPTURE` | ON | libsysprof-capture | `OFF` |
| `ENABLE_INTROSPECTION`, `ENABLE_DOCUMENTATION` | ON | gobject-introspection, gi-docgen | `OFF` |
| `ENABLE_VIDEO`, `ENABLE_WEB_AUDIO`, `ENABLE_MEDIA_STREAM`, `ENABLE_WEB_CODECS` | ON | GStreamer | `OFF` for Milestone 1 |
| `ENABLE_WEBDRIVER`, `ENABLE_BUBBLEWRAP_SANDBOX` | ON | – | `OFF` (no use on the console) |

## Linking a payload

`pkg-config-ps5 --libs <modules>` gives a complete static link line. Then
add libc++ 19 (harfbuzz, ICU and WebKit are C++). The smoke test links like
this (`smoke/build.sh`):

```sh
prospero-clang -o smoke smoke.o -Wl,--gc-sections \
  $(port/deps/pkg-config-ps5 --libs libsoup-3.0 gioopenssl gio-unix-2.0 fontconfig freetype2 harfbuzz openssl xkbcommon) \
  -nostdlib++ $LIBCXX/lib/libc++.a $LIBCXX/lib/libc++abi.a $LIBCXX/lib/libunwind.a
```

Things a consumer must know:

- **TLS is a static GIO module.** Link `gioopenssl` (pkg-config module,
  `lib/gio/modules/libgioopenssl.a`) and call, once, before any TLS use:
  ```c
  extern void g_io_openssl_load(GIOModule *module);
  g_io_openssl_load(NULL);   /* registers GTlsBackendOpenssl */
  ```
  Nothing is loaded from `GIO_MODULE_DIR` (payloads have no dlopen for our
  code). `gioenvironmentproxy` (proxy from `http_proxy`/`https_proxy`) is
  also built: `g_io_environmentproxy_load(NULL)` if wanted. GLib's own
  modules (local file monitor, etc.) are built into libgio already.
- **CA bundle**: OpenSSL is configured with
  `OPENSSLDIR=/data/ps5webkit/etc/ssl`, so glib-networking's default
  database (`X509_STORE_set_default_paths`) reads
  **`/data/ps5webkit/etc/ssl/cert.pem`** (Mozilla bundle from curl.se,
  2026-10-20). `SSL_CERT_FILE` overrides the path.
- **Fonts**: libfontconfig's compiled-in default config is
  **`/data/ps5webkit/etc/fonts/fonts.conf`** (`FONTCONFIG_FILE` overrides).
  It lists `<dir prefix="relative">../../fonts</dir>` (=
  `/data/ps5webkit/fonts`) and the cache `/data/ps5webkit/cache/fontconfig`.
- **XKB data**: libxkbcommon's default include path is
  **`/data/ps5webkit/share/xkb`** (xkeyboard-config 2.43; `XKB_CONFIG_ROOT`
  overrides). WPEPlatform's `wpe_keymap_xkb_new()` compiles evdev/pc105/us
  from it.
- **libps5compat** (our shim, `compat/`): memmem, getline/getdelim, resolver
  stubs (`res_query` etc. fail with HOST_NOT_FOUND; only GResolver SRV/MX
  lookups use them), empty fstab, and fixes for libkernel calls that return
  errno values (below), linked with
  `-Wl,--wrap=readlink,--wrap=eaccess,--wrap=faccessat,--wrap=link,--wrap=symlink,--wrap=ppoll`.
  `ps5compat.pc`, and `Libs.private` of `glib-2.0.pc`, `gio-2.0.pc`,
  `fontconfig.pc` and `xkbcommon.pc`, carry `-lps5compat` plus those flags.
  **WebKit should link with them too** (`pkg-config-ps5 --libs ps5compat`):
  WTF/libc++ `std::filesystem` call readlink, and the wrap only fixes
  objects linked with the flag. Each function is
  its own object, so a second memmem (WebKit's shim) does not clash.
  Empty `libm.a`/`librt.a` exist so `-lm`/`-lrt` resolve (libm lives in
  libSceLibcInternal).
- **ICU**: `libicudata.a` (full, 31.9 MB) is what `icu-uc.pc` links;
  `libicudata-slim.a` (18 MB) is a drop-in alternative.
- **libepoxy** dlopens `libEGL.so.1`/`libGL` on first GL call; a
  CPU-painted WebProcess never makes one. The UIProcess (title) gets GL from
  ps5-opengl, not from this epoxy.
- **No translations**: gettext is proxy-libintl (stubs).

## PS5 libc findings (13.60, from the smoke test)

- **Some libkernel calls return the errno value as a positive result**
  instead of -1 with errno set (`smoke/libcprobe.c` checks 28 calls):
  `readlink` (regular file → 22, missing → 2), `eaccess` and `faccessat`
  (→ 78 ENOSYS, even for existing paths), `link` and `symlink` (→ 45
  EOPNOTSUPP), `ppoll` (→ 3, no revents). `access`, `stat`, `lstat`, `open`,
  `mkdir`, `rmdir`, `unlink`, `rename`, `chmod`, `utimes`, `truncate`,
  `chdir`, `close`, `dup2`, `fcntl`, `fsync`, `statfs` and `poll` behave
  normally. The pattern looks like syscalls the payload sandbox doesn't
  allow; expect more of them and check return values when something odd
  happens.
  - glib: `ppoll` is disabled at configure time on PS5 (patch). With it,
    GMainContext sources on sockets never fired in worker threads, so every
    glib-networking TLS handshake timed out.
  - readlink: wrapped in libps5compat (lstat first). fontconfig failed to
    load its config without it.
  - eaccess/faccessat: wrapped, falling back to `access()`. libxkbcommon
    rejected its XKB directory without it.
  - link/symlink: wrapped to return -1/errno. Hard and symbolic links fail
    on /data (EOPNOTSUPP).
- `<malloc.h>` is a hard `#error` in the SDK headers (as on FreeBSD).
- No `F_KINFO`, no procfs: `g_unix_fd_query_path()` returns
  `G_FILE_ERROR_NOSYS`.
- Missing from every SDK library: iconv, memmem, getline, BIND resolver
  (`res_*`, `dn_*`), fstab functions, `getauxval`, `eventfd`.
- `getaddrinfo`, TCP sockets, `poll`, pthreads, `realpath` and OpenSSL's
  default entropy source (`RAND_bytes`) work in payloads.

## Console runtime tree

`share/ps5webkit-runtime` is copied to **`/data/ps5webkit/`** by
`smoke/upload-runtime.sh` (FTP, port 2121):

```
/data/ps5webkit/etc/ssl/cert.pem            Mozilla CA bundle
/data/ps5webkit/etc/fonts/fonts.conf        from runtime/etc/fonts/fonts.conf
/data/ps5webkit/fonts/                      Noto Sans/Serif (4 styles), Sans Mono (2), Sans CJK (Regular, Bold TTC)
/data/ps5webkit/cache/fontconfig/           fontconfig cache (written on first FcInit)
/data/ps5webkit/share/xkb/                  xkeyboard-config data
```

About 49 MB. Fonts are fetched by the `fonts` step (not committed).

## Patches

| patch | why |
| --- | --- |
| `libffi/0001-ps5-no-ms_abi.patch` | clang rejects `__attribute__((ms_abi))` for x86_64-sie-ps5; the Win64 ABIs are rejected at `ffi_prep_cif` instead |
| `glib/0001-ps5-configure-memalign-ppoll.patch` | don't use `memalign` (needs the `#error` `<malloc.h>`); don't use `ppoll` on PS5 (broken in libkernel, see above) |
| `glib/0002-unix-fd-query-path-no-F_KINFO.patch` | SDK headers lack `F_KINFO`; fall back to "not supported" |

## Licenses

| package | license |
| --- | --- |
| ps5compat (ours, `compat/`) | same as this repo |
| zlib | Zlib |
| brotli | MIT |
| libpng | libpng-2.0 (PNG Reference Library License v2) |
| libjpeg-turbo | IJG + BSD-3-Clause (+ Zlib for SIMD) |
| libwebp | BSD-3-Clause |
| expat | MIT |
| libiconv | LGPL-2.1-or-later |
| proxy-libintl | LGPL-2.1-or-later |
| libxml2 | MIT |
| libxslt | MIT |
| SQLite | Public domain |
| libpsl | MIT (built-in list: MPL-2.0 data) |
| FreeType | FTL or GPL-2.0-or-later (we use FTL) |
| HarfBuzz | MIT ("Old MIT") |
| fontconfig | HPND-style (MIT-like) |
| ICU | Unicode-3.0 |
| pcre2 | BSD-3-Clause |
| libffi | MIT |
| GLib | LGPL-2.1-or-later |
| OpenSSL | Apache-2.0 |
| nghttp2 | MIT |
| libsoup | LGPL-2.0-or-later |
| glib-networking | LGPL-2.1-or-later |
| libepoxy | MIT |
| libxkbcommon | MIT |
| xkeyboard-config (data) | MIT/X11-style |
| lcms2 | MIT (the GPL-3 fastfloat/threaded plugins are not built) |
| Mozilla CA bundle | MPL-2.0 |
| Noto fonts | OFL-1.1 |

The LGPL libraries are linked statically into GPL-3 payloads (the SDK is
GPL-3), which LGPL allows; publishing the full source, as FEASIBILITY.md §6
plans, covers the relinking requirement.

## Skipped (and why)

- **libgcrypt, libgpg-error, libtasn1**: not needed with `-DUSE_OPENSSL=ON`
  (WebCrypto and PAL crypto then use OpenSSL 3).
- **woff2**: FreeType with brotli decodes WOFF2; WebKit then disables `USE_WOFF2`.
- **libavif + dav1d, libjxl**: optional (`USE_AVIF`, `USE_JPEGXL`); dav1d is
  in `toolchain/ffmpeg-sw` if AVIF is wanted later.
- **libwpe / wpebackend-***: only for `ENABLE_WPE_LEGACY_API`.
- **Noto Color Emoji**: the file's path in googlefonts/noto-emoji moved;
  add it to `FONTS` in build.sh when needed (~10 MB, CBDT).
- zstd for libsoup (optional content coding), gssapi/NTLM, libproxy.
