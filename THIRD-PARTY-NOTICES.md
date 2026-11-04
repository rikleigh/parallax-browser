# Third-party notices

Parallax Browser is built from, links against or ships the following
projects. Each keeps its own licence. The builds download them from their
upstream sources; only patches are kept in this repository.

## In the title (`PPSA99020`)

| Component | Use | Licence |
| --- | --- | --- |
| [WebKit](https://webkit.org/) (WPE port) | Browser engine; patches in `port/webkit/patches/` | LGPL-2.1 / BSD-2-Clause |
| [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk) | Toolchain, libc, system library stubs | GPL-3.0 |
| [ps5-opengl](https://github.com/mihawk-99/ps5-opengl) (Mesa) | OpenGL / OpenGL ES and EGL on the PS5 GPU; patches in `spikes/02-gl-egl/ps5-opengl/patches/` | GPL-3.0-or-later; Mesa under MIT |
| [PS5VR](https://github.com/kavera001-dev/PS5VR) | Title packaging tools, the clean-room `libc.prx` by BlackBearReloaded, and the PS VR2 export-table technique (`tools/gen_export_table.py`) | GPL-3.0 |
| [uBlock Origin Lite](https://github.com/uBlockOrigin/uBOL-home) | Ad-block rule lists, converted by `tools/ubol-convert.js` | GPL-3.0 |
| [LLVM libc++ / libc++abi / libunwind](https://libcxx.llvm.org/) | C++ runtime | Apache-2.0 WITH LLVM-exception |
| [ICU](https://icu.unicode.org/) | Unicode and internationalisation | Unicode License v3 |
| [GLib](https://gitlab.gnome.org/GNOME/glib), [libsoup](https://gitlab.gnome.org/GNOME/libsoup), [GStreamer](https://gstreamer.freedesktop.org/) | Event loop, networking, media pipeline | LGPL-2.1-or-later |
| [FFmpeg](https://ffmpeg.org/), [dav1d](https://code.videolan.org/videolan/dav1d) | Software video and audio decoding | LGPL-2.1-or-later / BSD-2-Clause |
| [OpenSSL](https://www.openssl.org/) | TLS | Apache-2.0 |
| [HarfBuzz](https://harfbuzz.github.io/), [FreeType](https://freetype.org/), [Fontconfig](https://www.fontconfig.org/) | Text shaping and font rendering | MIT / FTL / MIT |
| [libxml2](https://gitlab.gnome.org/GNOME/libxml2), [libxslt](https://gitlab.gnome.org/GNOME/libxslt), [SQLite](https://sqlite.org/), [libpsl](https://github.com/rockdaboot/libpsl), [nghttp2](https://nghttp2.org/) | Parsing, storage, networking | MIT / public domain / MIT / MIT |
| [zlib](https://zlib.net/), [brotli](https://github.com/google/brotli), [libpng](http://www.libpng.org/), [libjpeg-turbo](https://libjpeg-turbo.org/), [libwebp](https://developers.google.com/speed/webp), [expat](https://libexpat.github.io/), [PCRE2](https://github.com/PCRE2Project/pcre2), [libffi](https://sourceware.org/libffi/), [libiconv](https://www.gnu.org/software/libiconv/) | Compression, images, XML, regex, FFI, charsets | zlib / MIT / libpng / BSD-3-Clause / BSD-3-Clause / MIT / BSD-3-Clause / MIT / LGPL-2.1 |

The full list with versions is in `port/deps/build.sh` and
[port/deps/STATUS.md](port/deps/STATUS.md).

## In the spikes

- `spikes/10-title-dmem/dlmalloc/`: Doug Lea's malloc, released to the
  public domain (CC0).
