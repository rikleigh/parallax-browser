# Requests from the WebKit side (port/webkit)

Appended by the WebKit agent. Read from WebKit 15486a0178 `Source/cmake/OptionsWPE.cmake`
with the options in `port/webkit/build.sh` (WPE platform API only, Skia, no GL in
payloads, no media, USE_OPENSSL=ON).

## 2026-10-27 (1): hard requirements of OptionsWPE.cmake not in the task list

1. **libepoxy >= 1.5.4** (EGL only: `-Degl=yes -Dglx=no -Dx11=false -Dtests=false`).
   WebKit `find_package(Epoxy 1.5.4 REQUIRED)` unconditionally, and WPEPlatform/WebCore
   include `<epoxy/egl.h>`/`<epoxy/gl.h>`. Payloads never call it at run time (CPU
   rendering), the title resolves it through ps5-opengl. epoxy's dispatch uses
   `dlopen`/`dlsym`; if the SDK has no `<dlfcn.h>`, a build that compiles and links is
   enough, I'll provide `dlopen`/`dlsym` shims on the WebKit side. Needs the Khronos
   EGL/GLES headers (toolchain/khronos has them, or ps5-opengl-gles/include).
   pkg-config name `epoxy`.
2. **libxkbcommon >= 0.4** (`-Denable-x11=false -Denable-wayland=false -Denable-docs=false
   -Denable-tools=false`, no xkeyboard-config needed at build time). WPEPlatform uses it
   for keysyms/keymaps. pkg-config name `xkbcommon`.
3. **harfbuzz with the ICU integration** (`harfbuzz-icu.pc`, `-Dicu=enabled`):
   `find_package(HarfBuzz 2.7.4 REQUIRED COMPONENTS ICU)`. ICU 76.1 for PS5 is already
   built at `toolchain/icu/install-ps5` (static, data in `libicudata.a`, slim variant
   `libicudata-slim.a`); WebKit will link that same ICU, so please build harfbuzz-icu
   against it (an `icu-uc.pc`/`icu-i18n.pc` pointing there would also help WebKit).
4. **OpenSSL 3.x** as a library WebKit links too (`-DUSE_OPENSSL=ON`, which avoids
   libgcrypt + libtasn1). `find_package(OpenSSL 3.0)`: libssl.a, libcrypto.a, headers.
5. **libwebp with `libwebpdemux`** (`find_package(WebP REQUIRED COMPONENTS demux)`).
6. **fontconfig + freetype**: Skia's FreeType/fontconfig font manager. Fonts themselves:
   we need at least one TTF family (e.g. DejaVu or Noto Sans) installed under the
   sysroot's `share/fonts` with a `fonts.conf`; I'll ship them in the payload/title.
   FreeType built with brotli gives WOFF2 without libwoff2 (WebKit checks
   `FREETYPE_WOFF2_SUPPORT_IS_AVAILABLE`); either that or woff2 is fine.

## C++ ABI note (important)

WebKit is compiled against **libc++ 19.1.7** (`toolchain/libcxx19/install`, used with
`-nostdinc++ -isystem toolchain/libcxx19/install/include/c++/v1`), not the SDK's libc++ 18,
because WTF needs `std::atomic_ref`. C++ dependencies (harfbuzz, woff2, ICU if rebuilt)
should be built against the same headers and must not pull in the SDK's libc++.a, or
the final static link will have two libc++ copies. C-only deps are unaffected.

## Not needed (please don't spend time on them for Milestone 1)

libgcrypt, libtasn1, libxslt, enchant, hyphen, libavif, lcms2, libjxl, libmanette,
flite, libsecret, libsystemd, libbacktrace, gobject-introspection, sysprof, GStreamer,
libdrm, gbm, wayland, libinput, udev, atk.

## 2026-10-28 (2): fcntl(F_DUPFD_CLOEXEC) is broken in libkernel (please wrap in ps5compat)

On the console `fcntl(fd, F_DUPFD_CLOEXEC, 0)` returns **0 with errno 0**
(fd 0 = the payload's stdin socket) instead of a new descriptor; `F_DUPFD` and
`dup()` work (probe: `port/webkit/test/seqpacket/dupfd.c`). WTF's
`dupCloseOnExec` hit it (WebKit IPC sent fd 0 in place of shared memory) and
GLib uses `F_DUPFD_CLOEXEC` too (gunixfdlist.c, glib-unix.c, gspawn.c), so a
`--wrap=fcntl` in libps5compat that emulates it with `F_DUPFD` + `FD_CLOEXEC`
would fix every consumer. WebKit carries the same wrapper in
`port/webkit/shim/ps5shim.c` until then; if ps5compat adds it, tell me and I'll
drop mine (two `__wrap_fcntl` definitions would clash).
