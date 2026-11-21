# Third-party notices

Parallax Browser is built from, links against or ships the following
projects. Each keeps its own licence. The builds download them from their
upstream sources; only patches are kept in this repository.

## In the title (`PPSA99207`)

| Component | Use | Licence |
| --- | --- | --- |
| [WebKit](https://webkit.org/) (WPE port) | Browser engine; patches in `port/webkit/patches/` | LGPL-2.1 / BSD-2-Clause |
| [ANGLE](https://chromium.googlesource.com/angle/angle) (in WebKit's `Source/ThirdParty/ANGLE`) | WebGL on top of OpenGL ES; in `GPU=radv` builds, all of the title's GL (compositor, UI, video, WebGL) on its Vulkan backend (patches in `port/webkit/patches-radv/`) | BSD-3-Clause |
| [VulkanMemoryAllocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator) | ANGLE's Vulkan memory (`GPU=radv` builds) | MIT |
| [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk) | Toolchain, libc, system library stubs | GPL-3.0 |
| [ps5-opengl](https://github.com/mihawk-99/ps5-opengl) (Mesa) | OpenGL / OpenGL ES and EGL on the PS5 GPU; patches in `spikes/02-gl-egl/ps5-opengl/patches/`. `GPU=gl` builds only (releases up to 0.2.3); `GPU=radv` builds, which the release is from 0.3.0, don't link it | GPL-3.0-or-later; Mesa under MIT |
| [Dawn](https://dawn.googlesource.com/dawn) with Tint, and in it [Abseil](https://abseil.io/), [SPIRV-Headers](https://github.com/KhronosGroup/SPIRV-Headers), [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) and [Vulkan-Utility-Libraries](https://github.com/KhronosGroup/Vulkan-Utility-Libraries) | WebGPU and WGSL on top of Vulkan (`WEBGPU=1` builds, which the release is); patches in `port/deps/patches/dawn/`, and for `GPU=radv` builds `port/deps/patches/dawn-radv/` too | BSD-3-Clause / Apache-2.0 / MIT / Apache-2.0 or MIT / Apache-2.0 |
| [RADV](https://docs.mesa3d.org/drivers/radv.html) from [PS5_Mesa](https://github.com/mpereiraesaa/PS5_Mesa) (Mesa 26.2 with a PS5 winsys by Mihawk), built with platform headers from [PS5_PayloadSDK](https://github.com/mihawk-99/PS5_PayloadSDK) | Vulkan on the PS5 GPU, under Dawn (`WEBGPU=1` builds) (`spikes/12-webgpu/p0b-radv/`), and under ANGLE as well in `GPU=radv` builds | MIT (Mesa); the SDK fork's headers GPL-3.0 |
| [PS5VR](https://github.com/kavera001-dev/PS5VR) | Title packaging tools, the clean-room `libc.prx` by BlackBearReloaded, and the PS VR2 export-table technique (`tools/gen_export_table.py`) | GPL-3.0 |
| [uBlock Origin Lite](https://github.com/uBlockOrigin/uBOL-home) | Ad-block rule lists, converted by `tools/ubol-convert.js` | GPL-3.0 |
| [LLVM libc++ / libc++abi / libunwind](https://libcxx.llvm.org/) | C++ runtime | Apache-2.0 WITH LLVM-exception |
| [ICU](https://icu.unicode.org/) | Unicode and internationalisation | Unicode License v3 |
| [GLib](https://gitlab.gnome.org/GNOME/glib), [libsoup](https://gitlab.gnome.org/GNOME/libsoup), [glib-networking](https://gitlab.gnome.org/GNOME/glib-networking), [GStreamer](https://gstreamer.freedesktop.org/) | Event loop, networking, TLS for GLib, media pipeline | LGPL-2.1-or-later (glib-networking with its OpenSSL exception) |
| [proxy-libintl](https://github.com/frida/proxy-libintl) | gettext stand-in for GLib | LGPL-2.0 |
| [FFmpeg](https://ffmpeg.org/), [dav1d](https://code.videolan.org/videolan/dav1d) | Software video and audio decoding | LGPL-2.1-or-later / BSD-2-Clause |
| [libogg](https://xiph.org/ogg/), [libvorbis](https://xiph.org/vorbis/), [Opus](https://opus-codec.org/) | Ogg, Vorbis and Opus audio | BSD-3-Clause |
| [OpenSSL](https://www.openssl.org/) | TLS | Apache-2.0 |
| [libepoxy](https://github.com/anholt/libepoxy), [libxkbcommon](https://xkbcommon.org/), [xkeyboard-config](https://gitlab.freedesktop.org/xkeyboard-config/xkeyboard-config), [Little CMS](https://www.littlecms.com/) | GL function loading, keyboard keymaps, colour management | MIT |
| [HarfBuzz](https://harfbuzz.github.io/), [FreeType](https://freetype.org/), [Fontconfig](https://www.fontconfig.org/) | Text shaping and font rendering | MIT / FTL / MIT |
| [libxml2](https://gitlab.gnome.org/GNOME/libxml2), [libxslt](https://gitlab.gnome.org/GNOME/libxslt), [SQLite](https://sqlite.org/), [libpsl](https://github.com/rockdaboot/libpsl), [nghttp2](https://nghttp2.org/) | Parsing, storage, networking | MIT / public domain / MIT / MIT |
| [libarchive](https://www.libarchive.org/), [bzip2](https://sourceware.org/bzip2/), [xz](https://tukaani.org/xz/) (liblzma), [zstd](https://facebook.github.io/zstd/) | Unpacking downloaded archives (ZIP, 7z, RAR, tar) in the download service | BSD-2-Clause / bzip2-1.0.6 / 0BSD / BSD-3-Clause (or GPL-2.0) |
| [zlib](https://zlib.net/), [brotli](https://github.com/google/brotli), [libpng](http://www.libpng.org/), [libjpeg-turbo](https://libjpeg-turbo.org/), [libwebp](https://developers.google.com/speed/webp), [expat](https://libexpat.github.io/), [PCRE2](https://github.com/PCRE2Project/pcre2), [libffi](https://sourceware.org/libffi/), [libiconv](https://www.gnu.org/software/libiconv/) | Compression, images, XML, regex, FFI, charsets | zlib / MIT / libpng / BSD-3-Clause / BSD-3-Clause / MIT / BSD-3-Clause / MIT / LGPL-2.1 |

## In the runtime files (`/data/ps5webkit`)

| Component | Use | Licence |
| --- | --- | --- |
| [Noto fonts](https://notofonts.github.io/) and [Noto Color Emoji](https://github.com/googlefonts/noto-emoji) | Fonts; the licence texts are beside them | OFL-1.1 |
| [Mozilla's CA certificates](https://curl.se/docs/caextract.html) (`etc/ssl/cert.pem`) | Trusted certificates for TLS | MPL-2.0 |

The full list with versions is in `port/deps/build.sh` and
[port/deps/STATUS.md](port/deps/STATUS.md).

## In the spikes

- `spikes/10-title-dmem/dlmalloc/`: Doug Lea's malloc, released to the
  public domain (CC0).
