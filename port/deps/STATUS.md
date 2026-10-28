# PS5 WPE dependency sysroot: status

Sysroot: `toolchain/wpe-sysroot` (static `.a` only, relocatable `.pc` files).
Built by `port/deps/build.sh`; how to consume it: `port/deps/README.md`.
Status: **built** = cross-compiles and installs; **smoke-tested** = linked
into `port/deps/smoke` and exercised on the console (13.60), all 18 checks PASS
(`smoke/results/20261028-002207-all-pass.log`). A clean rebuild of the whole
sysroot takes 4m22s.

Suggested WebKit flags: `-DUSE_OPENSSL=ON -DUSE_AVIF=OFF -DUSE_JPEGXL=OFF
-DENABLE_WPE_LEGACY_API=OFF -DUSE_GBM=OFF -DUSE_LIBDRM=OFF
-DENABLE_WPE_PLATFORM_DRM=OFF -DENABLE_WPE_PLATFORM_WAYLAND=OFF
-DUSE_LIBHYPHEN=OFF -DUSE_LIBBACKTRACE=OFF -DUSE_ATK=OFF
-DENABLE_SPEECH_SYNTHESIS=OFF -DENABLE_SPELLCHECK=OFF -DENABLE_JOURNALD_LOG=OFF
-DUSE_SYSPROF_CAPTURE=OFF -DENABLE_INTROSPECTION=OFF -DENABLE_DOCUMENTATION=OFF`
(+ media off for M1). Details in README.md.

| package | version | status | notes |
| --- | --- | --- | --- |
| ps5compat | 1.0 | smoke-tested | ours: memmem, getline, resolver/fstab stubs, fixes for libkernel calls returning errno as a positive value (readlink, eaccess, faccessat, link, symlink, ppoll; `-Wl,--wrap=...` from the .pc files); empty libm.a/librt.a |
| zlib | 1.3.1 | smoke-tested | |
| brotli | 1.1.0 | smoke-tested | (libsoup, freetype) |
| libpng | 1.6.47 | smoke-tested | linked via freetype |
| libjpeg-turbo | 3.1.0 | built | SIMD (nasm) on |
| libwebp | 1.5.0 | built | incl. demux, mux, sharpyuv |
| expat | 2.7.1 | smoke-tested | (fontconfig) |
| libiconv | 1.18 | smoke-tested | GNU; SDK libc has no iconv |
| proxy-libintl | 0.4 | smoke-tested | gettext stubs (no translations) |
| libxml2 | 2.14.6 | built | iconv on, no zlib/lzma/ICU/HTTP |
| libxslt | 1.1.43 | built | + libexslt, no crypto |
| sqlite3 | 3.50.4 | smoke-tested | amalgamation, no load_extension (libsoup) |
| libpsl | 0.21.5 | smoke-tested | builtin list, runtime=no |
| freetype | 2.13.3 | smoke-tested | zlib, png, brotli (=> WOFF2 via FreeType; WebKit skips libwoff2) |
| harfbuzz | 11.2.1 | smoke-tested | freetype + ICU (harfbuzz-icu), no glib |
| fontconfig | 2.16.0 | smoke-tested | default config /data/ps5webkit/etc/fonts/fonts.conf |
| ICU | 76.1 | built | copied from toolchain/icu/install-ps5 (+ libicudata-slim.a) |
| pcre2 | 10.45 | smoke-tested | 8-bit, no JIT |
| libffi | 3.4.8 | smoke-tested | patch: no ms_abi |
| glib | 2.88.4 | smoke-tested | gio, gio-unix, gobject, gmodule, gthread; 2 patches (ppoll off!) |
| OpenSSL | 3.5.2 | smoke-tested | static, no-dso/no-module; OPENSSLDIR=/data/ps5webkit/etc/ssl |
| nghttp2 | 1.65.0 | smoke-tested | HTTP/2 to example.com and youtube.com |
| libsoup | 3.8.1 | smoke-tested | HTTP/2, brotli, psl, sqlite |
| glib-networking | 2.90.1 | smoke-tested | OpenSSL backend, static: link `gioopenssl`, call `g_io_openssl_load(NULL)` |
| libepoxy | 1.5.10 | built | EGL only (required by OptionsWPE; unused by CPU painting) |
| libxkbcommon | 1.7.0 | smoke-tested | XKB root /data/ps5webkit/share/xkb; evdev/pc105/us keymap compiles in 7 ms |
| xkeyboard-config | 2.43 | smoke-tested | data in runtime tree share/xkb (/data/ps5webkit/share/xkb) |
| lcms2 | 2.17 | built | MIT core only |
| CA bundle | 2026-10-20 | smoke-tested | /data/ps5webkit/etc/ssl/cert.pem |
| Noto fonts + fonts.conf | notofonts @781896b | smoke-tested | /data/ps5webkit/fonts; + Arabic, Hebrew, Devanagari, Thai, Bengali, Tamil, Armenian, Georgian, Ethiopic, Symbols, Symbols 2, Math; OFL texts beside them |
| Noto Color Emoji | noto-emoji v2026-09-24-unicode18_0 | built | CBDT (FreeType + libpng), 10.7 MB, OFL-1.1 |
| MIME globs2 | ours | console-tested | /data/ps5webkit/share/mime/globs2 (hand-written; no shared-mime-info); children set XDG_DATA_DIRS=/data/ps5webkit/share |
| libgcrypt/libtasn1 | – | skipped | use -DUSE_OPENSSL=ON |
| libavif, libjxl, woff2, libwpe | – | skipped | see README |
| **Milestone 2** | | | `build.sh m2` (2m34s clean); gst smoke 9/9 PASS (`smoke/results/*-gst-all-pass.log`) |
| dav1d | 1.5.1 | smoke-tested | AV1 via avdec_libdav1d |
| FFmpeg | 7.1.1 | smoke-tested | decoders only: aac, mp3, opus, vorbis, flac, h264, hevc, vp8, vp9, libdav1d; avfilter, swresample; LGPL |
| GStreamer core | 1.28.8 | smoke-tested | static, `gstreamer-static-full` (gst_init registers static plugins), registry off |
| gst-plugins-base | 1.28.8 | smoke-tested | app, playback (decodebin3/parsebin/playbin3), typefind, converters; **no GL** |
| gst-plugins-good | 1.28.8 | smoke-tested | isomp4, matroska, audioparsers, adaptivedemux2 (hlsdemux2/dashdemux2), id3demux |
| gst-plugins-bad | 1.28.8 | smoke-tested | videoparsersbad, mpegtsdemux (+libgstmpegts), codecparsers |
| gst-libav | 1.28.8 | smoke-tested | patch: keep avdec_libdav1d, fix dropped frames without DR |
| gstreamer-full | 1.28.8 | smoke-tested | `lib/libgstreamer-full-1.0.a` = all gst libs + 25 plugins + FFmpeg/dav1d + `gst_init_static_plugins()`; WebKit: `-DUSE_GSTREAMER_FULL=ON -DUSE_GSTREAMER_GL=OFF` |
