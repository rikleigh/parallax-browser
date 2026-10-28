#!/usr/bin/env bash
# Cross-build the third-party libraries WPE WebKit needs for PS5 payloads
# (ps5-payload-sdk), as static libraries, into toolchain/wpe-sysroot.
#
#   port/deps/build.sh                 # every Milestone 1 step, in order
#   port/deps/build.sh glib libsoup    # just these steps
#   port/deps/build.sh --list          # list steps
#
# Each step fetches a pinned tarball (sha256-checked, kept in
# toolchain/wpe-deps/dl), extracts it into a new empty directory under
# toolchain/wpe-deps/src, applies port/deps/patches/<pkg>/*.patch, configures,
# builds and installs into the sysroot, then deletes its source and build trees.
#
# Env: JOBS (default ncpu), KEEP=1 (keep src/build trees after install).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
. "$ROOT/env.sh"

JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || nproc)}"
SDK="$PS5_PAYLOAD_SDK"
W="$ROOT/toolchain/wpe-deps"          # dl/ src/ build/ venv/ (gitignored)
SYSROOT="$ROOT/toolchain/wpe-sysroot"
LIBCXX="$ROOT/toolchain/libcxx19/install"
ICU_PS5="$ROOT/toolchain/icu/install-ps5"
PATCHES="$HERE/patches"
CROSS="$HERE/ps5-cross.ini"
TOOLCHAIN_CMAKE="$HERE/ps5-wpe.cmake"
PKGCONF="$HERE/pkg-config-ps5"
MESON_VER=1.7.2
# Console-side runtime root: fonts, fontconfig config, CA bundle (see README).
PS5_RUNTIME="/data/ps5webkit"

export PATH="$W/venv/bin:/opt/homebrew/opt/bison/bin:$SDK/bin:$PATH"

# libps5compat plus the --wrap flags for its libkernel fixes (compat/*.c).
PS5COMPAT_LIBS="-lps5compat -Wl,--wrap=readlink,--wrap=eaccess,--wrap=faccessat,--wrap=link,--wrap=symlink,--wrap=ppoll"

CFLAGS_COMMON="-O2 -g0 -fPIC -ffunction-sections -fdata-sections"
CXXFLAGS_COMMON="$CFLAGS_COMMON -nostdinc++ -isystem $LIBCXX/include/c++/v1"

log() { printf '\n=== %s\n' "$*"; }
die() { echo "error: $*" >&2; exit 1; }

# fetch <url> <sha256> [file]: download into dl/ once, always verify.
fetch() {
    local url=$1 sum=$2 f="$W/dl/${3:-$(basename "$1")}"
    mkdir -p "$(dirname "$f")"
    if [ ! -s "$f" ]; then
        echo "fetch $url"
        curl -sSfL --retry 3 -o "$f.part" "$url" && mv "$f.part" "$f"
    fi
    echo "$sum  $f" | shasum -a 256 -c - >/dev/null || die "sha256 mismatch: $f"
    FETCHED="$f"
}

# unpack <name> <archive>: fresh, empty src/<name>, archive's top dir stripped.
unpack() {
    local name=$1 arc=$2 d="$W/src/$1"
    rm -rf "$d"; mkdir -p "$d"
    case "$arc" in
        *.zip) local t="$W/src/.$name-zip"; rm -rf "$t"; mkdir -p "$t"
               unzip -q "$arc" -d "$t"; mv "$t"/*/* "$d"/; rm -rf "$t" ;;
        *) tar -xf "$arc" -C "$d" --strip-components=1 ;;
    esac
    if [ -d "$PATCHES/$name" ]; then
        for p in "$PATCHES/$name"/*.patch; do
            [ -f "$p" ] || continue
            echo "patch $name: $(basename "$p")"
            patch -s -d "$d" -p1 < "$p"
        done
    fi
    SRC="$d"
    BLD="$W/build/$name"
    rm -rf "$BLD"; mkdir -p "$BLD"
}

cleanup() { # <name>
    [ -n "${KEEP:-}" ] || rm -rf "$W/src/$1" "$W/build/$1"
}

meson_pkg() { # <name> [meson options...]
    local name=$1; shift
    meson setup "$BLD" "$SRC" --cross-file "$CROSS" \
        --prefix="$SYSROOT" --libdir=lib --buildtype=release \
        -Ddefault_library=static -Db_staticpic=true -Dstrip=false \
        --wrap-mode=nodownload "$@" >"$W/build/$name.configure.log" 2>&1 ||
        { tail -40 "$W/build/$name.configure.log"; die "meson setup $name failed"; }
    ninja -C "$BLD" -j"$JOBS" >"$W/build/$name.build.log" 2>&1 ||
        { grep -E 'error|FAILED' "$W/build/$name.build.log" | head -40; die "build $name failed"; }
    meson install -C "$BLD" --no-rebuild >/dev/null
}

cmake_pkg() { # <name> [cmake options...]
    local name=$1; shift
    cmake -G Ninja -S "$SRC" -B "$BLD" \
        -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_CMAKE" \
        -DCMAKE_INSTALL_PREFIX="$SYSROOT" -DCMAKE_INSTALL_LIBDIR=lib \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_FLAGS="$CFLAGS_COMMON -I$SYSROOT/include" \
        -DCMAKE_CXX_FLAGS="$CXXFLAGS_COMMON -I$SYSROOT/include" \
        -DCMAKE_C_FLAGS_RELEASE="" -DCMAKE_CXX_FLAGS_RELEASE="" \
        -DCMAKE_EXE_LINKER_FLAGS="-L$SYSROOT/lib" \
        -DBUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        "$@" >"$W/build/$name.configure.log" 2>&1 ||
        { tail -40 "$W/build/$name.configure.log"; die "cmake $name failed"; }
    ninja -C "$BLD" -j"$JOBS" >"$W/build/$name.build.log" 2>&1 ||
        { grep -E 'error|FAILED' "$W/build/$name.build.log" | head -40; die "build $name failed"; }
    ninja -C "$BLD" install >/dev/null
}

autotools_env() {
    export CC="$SDK/bin/prospero-clang" CXX="$SDK/bin/prospero-clang++"
    export AR="$SDK/bin/prospero-ar" RANLIB="$SDK/bin/prospero-ranlib"
    export NM="$SDK/bin/prospero-nm" STRIP="$SDK/bin/prospero-strip"
    export CFLAGS="$CFLAGS_COMMON" CXXFLAGS="$CXXFLAGS_COMMON"
    export CPPFLAGS="-I$SYSROOT/include" LDFLAGS="-L$SYSROOT/lib"
    export PKG_CONFIG="$PKGCONF"
}

autotools_pkg() { # <name> [configure options...]
    local name=$1; shift
    ( autotools_env
      cd "$BLD"
      "$SRC/configure" --host=x86_64-pc-freebsd9 \
          --build="$(uname -m | sed s/arm64/aarch64/)-apple-darwin" \
          --prefix="$SYSROOT" --libdir="$SYSROOT/lib" \
          --enable-static --disable-shared "$@" >"$W/build/$name.configure.log" 2>&1 ||
          { tail -40 "$W/build/$name.configure.log"; exit 1; }
      make -j"$JOBS" >"$W/build/$name.build.log" 2>&1 ||
          { grep -E 'error' "$W/build/$name.build.log" | head -40; exit 1; }
      make install >/dev/null 2>&1 ) || die "$name failed"
}

# Rewrite absolute sysroot paths in .pc files to ${pcfiledir}, drop libtool
# archives. Run after every install.
fix_sysroot() {
    local pc
    for pc in "$SYSROOT"/lib/pkgconfig/*.pc "$SYSROOT"/share/pkgconfig/*.pc; do
        [ -f "$pc" ] && [ ! -L "$pc" ] || continue
        if grep -q "$SYSROOT" "$pc"; then
            local rel='${pcfiledir}/../..'
            sed -i '' -e "s|^prefix=$SYSROOT\$|prefix=$rel|" -e "s|$SYSROOT|\${prefix}|g" "$pc"
        fi
    done
    rm -f "$SYSROOT"/lib/*.la
}

# Add a library to a .pc file's Libs.private (static link dependencies that
# the upstream .pc omits).
pc_add_private() { # <pc name> <flags>
    local pc="$SYSROOT/lib/pkgconfig/$1.pc"
    if grep -q '^Libs.private:' "$pc"; then
        grep -q -- " $2\$\| $2 " "$pc" || sed -i '' "s|^Libs.private:\(.*\)|Libs.private:\1 $2|" "$pc"
    else
        echo "Libs.private: $2" >> "$pc"
    fi
}

# ===================================================================== steps

step_tools() {
    log "host tools: meson $MESON_VER in toolchain/wpe-deps/venv"
    if ! "$W/venv/bin/meson" --version 2>/dev/null | grep -qx "$MESON_VER"; then
        python3 -m venv "$W/venv"
        "$W/venv/bin/pip" install -q "meson==$MESON_VER"
    fi
    command -v ninja >/dev/null || die "ninja missing (brew install ninja)"
    command -v nasm >/dev/null || die "nasm missing (brew install nasm)"
    command -v gperf >/dev/null || die "gperf missing"
    [ -x /opt/homebrew/opt/bison/bin/bison ] || die "bison 3 missing (brew install bison)"
    command -v glib-mkenums >/dev/null || die "host glib tools missing (brew install glib)"
    mkdir -p "$SYSROOT/lib/pkgconfig" "$SYSROOT/include" "$SYSROOT/share"
}

# libps5compat.a: memmem, getline/getdelim, resolver stubs (port/deps/compat).
step_compat() {
    log "ps5compat"
    local b="$W/build/compat"
    rm -rf "$b"; mkdir -p "$b"
    for c in "$HERE"/compat/*.c; do
        prospero-clang $CFLAGS_COMMON -Wall -c "$c" -o "$b/$(basename "$c" .c).o"
    done
    rm -f "$SYSROOT/lib/libps5compat.a"
    prospero-ar rcs "$SYSROOT/lib/libps5compat.a" "$b"/*.o
    install -m644 "$HERE/compat/ps5compat.h" "$SYSROOT/include/"
    cat > "$SYSROOT/lib/pkgconfig/ps5compat.pc" <<EOF
prefix=\${pcfiledir}/../..
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: ps5compat
Description: libc additions for PS5 payloads (memmem, getline, resolver stubs)
Version: 1.0
Libs: -L\${libdir} $PS5COMPAT_LIBS
Cflags: -I\${includedir}
EOF
    rm -rf "$b"
    # libm and librt are part of libSceLibcInternal/libkernel on the PS5, but
    # many build systems add -lm/-lrt. Empty archives keep those links working.
    for l in m rt; do
        rm -f "$SYSROOT/lib/lib$l.a"
        prospero-ar rcs "$SYSROOT/lib/lib$l.a"
    done
}

step_zlib() {
    log "zlib 1.3.1"
    fetch https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz \
        9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23
    unpack zlib "$FETCHED"
    cmake_pkg zlib -DZLIB_BUILD_EXAMPLES=OFF
    rm -f "$SYSROOT"/lib/libz.so* "$SYSROOT"/lib/libz.dylib
    # zlib's CMake installs libz.a and libzlibstatic? normalise to libz.a
    fix_sysroot; cleanup zlib
}

step_brotli() {
    log "brotli 1.1.0"
    fetch https://github.com/google/brotli/archive/refs/tags/v1.1.0.tar.gz \
        e720a6ca29428b803f4ad165371771f5398faba397edf6778837a18599ea13ff brotli-1.1.0.tar.gz
    unpack brotli "$FETCHED"
    cmake_pkg brotli -DBROTLI_DISABLE_TESTS=ON -DBROTLI_BUNDLED_MODE=OFF
    rm -f "$SYSROOT/bin/brotli"
    fix_sysroot; cleanup brotli
}

step_libpng() {
    log "libpng 1.6.47"
    fetch https://download.sourceforge.net/libpng/libpng-1.6.47.tar.xz \
        b213cb381fbb1175327bd708a77aab708a05adde7b471bc267bd15ac99893631
    unpack libpng "$FETCHED"
    cmake_pkg libpng -DPNG_SHARED=OFF -DPNG_STATIC=ON -DPNG_TESTS=OFF -DPNG_TOOLS=OFF \
        -DPNG_FRAMEWORK=OFF -DZLIB_ROOT="$SYSROOT"
    rm -rf "$SYSROOT/bin/png-fix-itxt" "$SYSROOT/bin/pngfix"
    fix_sysroot; cleanup libpng
}

step_libjpeg_turbo() {
    log "libjpeg-turbo 3.1.0"
    fetch https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/3.1.0/libjpeg-turbo-3.1.0.tar.gz \
        9564c72b1dfd1d6fe6274c5f95a8d989b59854575d4bbee44ade7bc17aa9bc93
    unpack libjpeg-turbo "$FETCHED"
    cmake_pkg libjpeg-turbo -DENABLE_SHARED=OFF -DENABLE_STATIC=ON -DWITH_TURBOJPEG=OFF \
        -DWITH_SIMD=ON -DCMAKE_ASM_NASM_COMPILER="$(command -v nasm)" -DWITH_TOOLS=OFF -DWITH_TESTS=OFF
    rm -f "$SYSROOT"/bin/{cjpeg,djpeg,jpegtran,rdjpgcom,wrjpgcom,tjbench}
    fix_sysroot; cleanup libjpeg-turbo
}

step_libwebp() {
    log "libwebp 1.5.0"
    fetch https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-1.5.0.tar.gz \
        7d6fab70cf844bf6769077bd5d7a74893f8ffd4dfb42861745750c63c2a5c92c
    unpack libwebp "$FETCHED"
    cmake_pkg libwebp -DWEBP_BUILD_ANIM_UTILS=OFF -DWEBP_BUILD_CWEBP=OFF -DWEBP_BUILD_DWEBP=OFF \
        -DWEBP_BUILD_GIF2WEBP=OFF -DWEBP_BUILD_IMG2WEBP=OFF -DWEBP_BUILD_VWEBP=OFF \
        -DWEBP_BUILD_WEBPINFO=OFF -DWEBP_BUILD_WEBPMUX=OFF -DWEBP_BUILD_EXTRAS=OFF \
        -DWEBP_BUILD_LIBWEBPMUX=ON -DWEBP_USE_THREAD=ON
    fix_sysroot; cleanup libwebp
}

step_expat() {
    log "expat 2.7.1"
    fetch https://github.com/libexpat/libexpat/releases/download/R_2_7_1/expat-2.7.1.tar.xz \
        354552544b8f99012e5062f7d570ec77f14b412a3ff5c7d8d0dae62c0d217c30
    unpack expat "$FETCHED"
    cmake_pkg expat -DEXPAT_BUILD_TOOLS=OFF -DEXPAT_BUILD_EXAMPLES=OFF -DEXPAT_BUILD_TESTS=OFF \
        -DEXPAT_BUILD_DOCS=OFF -DEXPAT_SHARED_LIBS=OFF
    fix_sysroot; cleanup expat
}

step_libiconv() {
    log "libiconv 1.18 (GNU; the SDK libc has no iconv)"
    fetch https://ftp.gnu.org/pub/gnu/libiconv/libiconv-1.18.tar.gz \
        3b08f5f4f9b4eb82f151a7040bfd6fe6c6fb922efe4b1659c66ea933276965e8
    unpack libiconv "$FETCHED"
    autotools_pkg libiconv --disable-nls --enable-extra-encodings
    rm -f "$SYSROOT/bin/iconv"
    fix_sysroot; cleanup libiconv
}

step_proxy_libintl() {
    log "proxy-libintl 0.4 (gettext stubs for glib/glib-networking; no translations)"
    fetch https://github.com/frida/proxy-libintl/archive/refs/tags/0.4.tar.gz \
        13ef3eea0a3bc0df55293be368dfbcff5a8dd5f4759280f28e030d1494a5dffb proxy-libintl-0.4.tar.gz
    unpack proxy-libintl "$FETCHED"
    meson_pkg proxy-libintl
    fix_sysroot; cleanup proxy-libintl
}

step_libxml2() {
    log "libxml2 2.14.6"
    fetch https://download.gnome.org/sources/libxml2/2.14/libxml2-2.14.6.tar.xz \
        7ce458a0affeb83f0b55f1f4f9e0e55735dbfc1a9de124ee86fb4a66b597203a
    unpack libxml2 "$FETCHED"
    cmake_pkg libxml2 -DLIBXML2_WITH_PYTHON=OFF -DLIBXML2_WITH_LZMA=OFF -DLIBXML2_WITH_ZLIB=OFF \
        -DLIBXML2_WITH_ICU=OFF -DLIBXML2_WITH_ICONV=ON -DLIBXML2_WITH_PROGRAMS=OFF \
        -DLIBXML2_WITH_TESTS=OFF -DLIBXML2_WITH_HTTP=OFF -DLIBXML2_WITH_MODULES=OFF \
        -DLIBXML2_WITH_DOCS=OFF -DIconv_INCLUDE_DIR="$SYSROOT/include" \
        -DIconv_LIBRARY="$SYSROOT/lib/libiconv.a"
    fix_sysroot; cleanup libxml2
}

step_libxslt() {
    log "libxslt 1.1.43"
    fetch https://download.gnome.org/sources/libxslt/1.1/libxslt-1.1.43.tar.xz \
        5a3d6b383ca5afc235b171118e90f5ff6aa27e9fea3303065231a6d403f0183a
    unpack libxslt "$FETCHED"
    cmake_pkg libxslt -DLIBXSLT_WITH_PYTHON=OFF -DLIBXSLT_WITH_PROGRAMS=OFF \
        -DLIBXSLT_WITH_TESTS=OFF -DLIBXSLT_WITH_CRYPTO=OFF -DLIBXSLT_WITH_MODULES=OFF \
        -DLIBXSLT_WITH_PROFILER=OFF -DLibXml2_DIR="$SYSROOT/lib/cmake/libxml2-2.14.6"
    fix_sysroot; cleanup libxslt
}

step_sqlite() {
    log "sqlite 3.50.4 (amalgamation)"
    fetch https://www.sqlite.org/2025/sqlite-amalgamation-3500400.zip \
        1d3049dd0f830a025a53105fc79fd2ab9431aea99e137809d064d8ee8356b032
    unpack sqlite "$FETCHED"
    # Options as distros build it for WebKit; no extension loading (no dlopen
    # on payloads), no Tcl.
    prospero-clang $CFLAGS_COMMON -DSQLITE_ENABLE_FTS3 -DSQLITE_ENABLE_FTS4 -DSQLITE_ENABLE_FTS5 \
        -DSQLITE_ENABLE_RTREE -DSQLITE_ENABLE_COLUMN_METADATA -DSQLITE_ENABLE_UNLOCK_NOTIFY \
        -DSQLITE_ENABLE_DBSTAT_VTAB -DSQLITE_SECURE_DELETE -DSQLITE_OMIT_LOAD_EXTENSION \
        -DSQLITE_THREADSAFE=1 -DSQLITE_DEFAULT_MEMSTATUS=0 -DHAVE_USLEEP=1 -DHAVE_LOCALTIME_R=1 \
        -c "$SRC/sqlite3.c" -o "$BLD/sqlite3.o"
    rm -f "$SYSROOT/lib/libsqlite3.a"
    prospero-ar rcs "$SYSROOT/lib/libsqlite3.a" "$BLD/sqlite3.o"
    install -m644 "$SRC/sqlite3.h" "$SRC/sqlite3ext.h" "$SYSROOT/include/"
    cat > "$SYSROOT/lib/pkgconfig/sqlite3.pc" <<'EOF'
prefix=${pcfiledir}/../..
libdir=${prefix}/lib
includedir=${prefix}/include

Name: SQLite
Description: SQL database engine
Version: 3.50.4
Libs: -L${libdir} -lsqlite3
Libs.private: -lpthread -lm
Cflags: -I${includedir}
EOF
    cleanup sqlite
}

step_libpsl() {
    log "libpsl 0.21.5 (builtin list, no IDNA runtime)"
    fetch https://github.com/rockdaboot/libpsl/releases/download/0.21.5/libpsl-0.21.5.tar.gz \
        1dcc9ceae8b128f3c0b3f654decd0e1e891afc6ff81098f227ef260449dae208
    unpack libpsl "$FETCHED"
    meson_pkg libpsl -Druntime=no -Dbuiltin=true -Dtests=false -Ddocs=false
    rm -f "$SYSROOT/bin/psl"
    fix_sysroot; cleanup libpsl
}

step_freetype() {
    log "freetype 2.13.3 (zlib, png, brotli -> WOFF2; no harfbuzz)"
    fetch https://download.savannah.gnu.org/releases/freetype/freetype-2.13.3.tar.xz \
        0550350666d427c74daeb85d5ac7bb353acba5f76956395995311a9c6f063289
    unpack freetype "$FETCHED"
    meson_pkg freetype -Dzlib=enabled -Dpng=enabled -Dbrotli=enabled -Dbzip2=disabled \
        -Dharfbuzz=disabled -Dmmap=enabled -Dtests=disabled
    fix_sysroot; cleanup freetype
}

step_harfbuzz() {
    log "harfbuzz 11.2.1 (freetype, ICU)"
    fetch https://github.com/harfbuzz/harfbuzz/releases/download/11.2.1/harfbuzz-11.2.1.tar.xz \
        093714c8548a285094685f0bdc999e202d666b59eeb3df2ff921ab68b8336a49
    unpack harfbuzz "$FETCHED"
    meson_pkg harfbuzz -Dfreetype=enabled -Dicu=enabled -Dglib=disabled -Dgobject=disabled \
        -Dcairo=disabled -Dchafa=disabled -Dgraphite2=disabled -Dtests=disabled \
        -Dintrospection=disabled -Ddocs=disabled -Dutilities=disabled -Dbenchmark=disabled \
        -Dicu_builtin=false -Dexperimental_api=false
    fix_sysroot; cleanup harfbuzz
}

step_fontconfig() {
    log "fontconfig 2.16.0 (expat; config under $PS5_RUNTIME/etc/fonts on the console)"
    fetch https://www.freedesktop.org/software/fontconfig/release/fontconfig-2.16.0.tar.xz \
        6a33dc555cc9ba8b10caf7695878ef134eeb36d0af366041f639b1da9b6ed220
    unpack fontconfig "$FETCHED"
    # The console paths are compiled in as defaults (FONTCONFIG_FILE/PATH
    # still override). Install via a staging DESTDIR so nothing lands in
    # /data on the Mac; only lib/include/pkgconfig move to the sysroot.
    meson setup "$BLD" "$SRC" --cross-file "$CROSS" \
        --prefix="$SYSROOT" --libdir=lib --buildtype=release \
        -Ddefault_library=static -Db_staticpic=true --wrap-mode=nodownload \
        -Ddoc=disabled -Dnls=disabled -Dtests=disabled -Dtools=disabled \
        -Dcache-build=disabled -Diconv=disabled -Dxml-backend=expat \
        -Dbaseconfig-dir="$PS5_RUNTIME/etc/fonts" -Dconfig-dir="$PS5_RUNTIME/etc/fonts/conf.d" \
        -Dtemplate-dir="$PS5_RUNTIME/share/fontconfig/conf.avail" \
        -Dcache-dir="$PS5_RUNTIME/cache/fontconfig" \
        -Ddefault-fonts-dirs="$PS5_RUNTIME/fonts" -Dadditional-fonts-dirs=no \
        >"$W/build/fontconfig.configure.log" 2>&1 ||
        { tail -40 "$W/build/fontconfig.configure.log"; die "meson setup fontconfig failed"; }
    ninja -C "$BLD" -j"$JOBS" >"$W/build/fontconfig.build.log" 2>&1 ||
        { grep -E 'error|FAILED' "$W/build/fontconfig.build.log" | head -40; die "build fontconfig failed"; }
    local stage="$W/build/fontconfig-stage"
    rm -rf "$stage"
    DESTDIR="$stage" meson install -C "$BLD" --no-rebuild >/dev/null
    rsync -a "$stage$SYSROOT/" "$SYSROOT/"
    # Upstream conf.avail snippets, for reference when writing our config.
    mkdir -p "$SYSROOT/share/fontconfig"
    rsync -a "$stage$PS5_RUNTIME/share/fontconfig/conf.avail/" \
        "$SYSROOT/share/fontconfig/conf.avail/"
    rm -rf "$stage"
    fix_sysroot
    # PS5 readlink() returns errno values (see compat/readlink.c).
    pc_add_private fontconfig "$PS5COMPAT_LIBS"
    cleanup fontconfig
}

step_icu() {
    log "ICU 76.1 (copied from toolchain/icu/install-ps5; built by spikes/04-jsc/build.sh)"
    [ -f "$ICU_PS5/lib/libicuuc.a" ] || die "missing $ICU_PS5 (run spikes/04-jsc/build.sh icu-host icu-ps5)"
    mkdir -p "$SYSROOT/include" "$SYSROOT/lib/pkgconfig"
    rsync -a --delete "$ICU_PS5/include/unicode/" "$SYSROOT/include/unicode/"
    cp "$ICU_PS5"/lib/libicu{uc,i18n,data}.a "$SYSROOT/lib/"
    [ -f "$ICU_PS5/lib/libicudata-slim.a" ] && cp "$ICU_PS5/lib/libicudata-slim.a" "$SYSROOT/lib/"
    for pc in icu-uc icu-i18n; do
        sed -e "s|^prefix *=.*|prefix=\${pcfiledir}/../..|" \
            -e 's|^exec_prefix *=.*|exec_prefix=${prefix}|' \
            -e 's|^libdir *=.*|libdir=${prefix}/lib|' \
            -e 's|^includedir *=.*|includedir=${prefix}/include|' \
            "$ICU_PS5/lib/pkgconfig/$pc.pc" > "$SYSROOT/lib/pkgconfig/$pc.pc"
    done
    # ICU is C++: its static users need libc++ (WebKit links libc++ 19 itself).
    # icu-uc.pc lists -licudata via Libs.private already when present.
}

step_pcre2() {
    log "pcre2 10.45 (8-bit, for glib)"
    fetch https://github.com/PCRE2Project/pcre2/releases/download/pcre2-10.45/pcre2-10.45.tar.bz2 \
        21547f3516120c75597e5b30a992e27a592a31950b5140e7b8bfde3f192033c4
    unpack pcre2 "$FETCHED"
    cmake_pkg pcre2 -DPCRE2_BUILD_PCRE2_8=ON -DPCRE2_BUILD_PCRE2_16=OFF -DPCRE2_BUILD_PCRE2_32=OFF \
        -DPCRE2_BUILD_PCRE2GREP=OFF -DPCRE2_BUILD_TESTS=OFF -DPCRE2_SUPPORT_JIT=OFF \
        -DPCRE2_SUPPORT_UNICODE=ON -DPCRE2_STATIC_PIC=ON
    fix_sysroot; cleanup pcre2
}

step_libffi() {
    log "libffi 3.4.8"
    fetch https://github.com/libffi/libffi/releases/download/v3.4.8/libffi-3.4.8.tar.gz \
        bc9842a18898bfacb0ed1252c4febcc7e78fa139fd27fdc7a3e30d9d9356119b
    unpack libffi "$FETCHED"
    autotools_pkg libffi --disable-docs --disable-multi-os-directory --disable-exec-static-tramp
    fix_sysroot; cleanup libffi
}

step_glib() {
    log "glib 2.88.4 (gio, gobject, gmodule, gthread; static; kqueue file monitor)"
    fetch https://download.gnome.org/sources/glib/2.88/glib-2.88.4.tar.xz \
        25fd169f3b2f91fe2391ad297883ce0fcf30c69fc789ffd1c9672d1332b1c5a8
    unpack glib "$FETCHED"
    meson_pkg glib -Dselinux=disabled -Dxattr=false -Dlibmount=disabled -Dman-pages=disabled \
        -Ddtrace=disabled -Dsystemtap=disabled -Dsysprof=disabled -Dtests=false \
        -Dinstalled_tests=false -Dnls=disabled -Dglib_debug=disabled -Dlibelf=disabled \
        -Dintrospection=disabled -Ddocumentation=false -Dbsymbolic_functions=false \
        -Dfile_monitor_backend=kqueue \
        -Dc_link_args="['-L$SYSROOT/lib', '-Wl,--gc-sections', '-lps5compat']"
    rm -f "$SYSROOT"/bin/{gio,gio-querymodules,glib-compile-schemas,glib-compile-resources,gresource,gsettings,gdbus,gapplication,gobject-query,gi-*}
    fix_sysroot
    # res_query/dn_expand (GResolver), getline: from libps5compat.
    pc_add_private glib-2.0 "$PS5COMPAT_LIBS"
    pc_add_private gio-2.0 "$PS5COMPAT_LIBS"
    # gmodule's .pc adds -Wl,--export-dynamic; payloads load no modules.
    sed -i '' 's/ *-Wl,--export-dynamic//' "$SYSROOT"/lib/pkgconfig/gmodule-*.pc
    cleanup glib
}

step_openssl() {
    log "OpenSSL 3.5.2 (static libssl/libcrypto; OPENSSLDIR=$PS5_RUNTIME/etc/ssl)"
    fetch https://github.com/openssl/openssl/releases/download/openssl-3.5.2/openssl-3.5.2.tar.gz \
        c53a47e5e441c930c3928cf7bf6fb00e5d129b630e0aa873b08258656e7345ec
    unpack openssl "$FETCHED"
    # OPENSSLDIR is a console path: the default CA file is then
    # $PS5_RUNTIME/etc/ssl/cert.pem (SSL_CERT_FILE overrides it).
    # no-dso/no-module: no dlopen on payloads; providers are built in.
    ( autotools_env
      cd "$BLD"
      "$SRC/Configure" BSD-x86_64 no-shared no-tests no-apps no-docs no-dso no-module \
          no-engine no-afalgeng no-async no-uplink no-ui-console \
          --prefix="$SYSROOT" --libdir=lib --openssldir="$PS5_RUNTIME/etc/ssl" \
          $CFLAGS_COMMON >"$W/build/openssl.configure.log" 2>&1 || { tail -20 "$W/build/openssl.configure.log"; exit 1; }
      make -j"$JOBS" build_sw >"$W/build/openssl.build.log" 2>&1 || { grep -E 'error' "$W/build/openssl.build.log" | head -30; exit 1; }
      make install_sw >/dev/null ) || die "openssl failed"
    rm -f "$SYSROOT/bin/c_rehash"
    fix_sysroot; cleanup openssl
}

step_nghttp2() {
    log "nghttp2 1.65.0 (library only)"
    fetch https://github.com/nghttp2/nghttp2/releases/download/v1.65.0/nghttp2-1.65.0.tar.xz \
        f1b9df5f02e9942b31247e3d415483553bc4ac501c87aa39340b6d19c92a9331
    unpack nghttp2 "$FETCHED"
    cmake_pkg nghttp2 -DENABLE_LIB_ONLY=ON -DBUILD_SHARED_LIBS=OFF -DBUILD_STATIC_LIBS=ON \
        -DENABLE_DOC=OFF -DBUILD_TESTING=OFF -DENABLE_FAILMALLOC=OFF
    # Static users must define NGHTTP2_STATICLIB on Windows only; nothing to do here.
    fix_sysroot; cleanup nghttp2
}

step_libsoup() {
    log "libsoup 3.8.1 (HTTP/2 via nghttp2, brotli, libpsl, sqlite; TLS via glib-networking)"
    fetch https://download.gnome.org/sources/libsoup/3.8/libsoup-3.8.1.tar.xz \
        5013dcef9136f6f861e6529fca578af50c1f52a39219027fb5aff652d1371a04
    unpack libsoup "$FETCHED"
    meson_pkg libsoup -Dgssapi=disabled -Dntlm=disabled -Dbrotli=enabled -Dzstd=disabled \
        -Dtls_check=false -Dintrospection=disabled -Dvapi=disabled -Ddocs=disabled \
        -Ddoc_tests=false -Dtests=false -Dautobahn=disabled -Dinstalled_tests=false \
        -Dsysprof=disabled -Dpkcs11_tests=disabled
    fix_sysroot; cleanup libsoup
}

step_glib_networking() {
    log "glib-networking 2.90.1 (OpenSSL TLS backend as a static GIO module)"
    fetch https://download.gnome.org/sources/glib-networking/2.90/glib-networking-2.90.1.tar.xz \
        ea95636590098355a05e3027579dba1a9a950b05e456b2488ea03d82e4b6c18a
    unpack glib-networking "$FETCHED"
    meson_pkg glib-networking -Dgnutls=disabled -Dopenssl=enabled -Denvironment_proxy=enabled \
        -Dlibproxy=disabled -Dgnome_proxy=disabled -Dtests=false -Dinstalled_tests=false ${GLIBNET_EXTRA:-}
    # The static modules land in lib/gio/modules with their .pc files in
    # lib/gio/modules/pkgconfig; expose them next to the others.
    local m
    for m in "$SYSROOT"/lib/gio/modules/pkgconfig/*.pc; do
        [ -f "$m" ] || continue
        sed -e 's|^prefix=.*|prefix=${pcfiledir}/../..|' -e "s|$SYSROOT|\${prefix}|g" \
            -e 's|^libdir=.*|libdir=${prefix}/lib/gio/modules|' "$m" > "$SYSROOT/lib/pkgconfig/$(basename "$m")"
    done
    rm -rf "$SYSROOT/lib/gio/modules/pkgconfig" "$SYSROOT"/lib/gio/modules/*.so
    fix_sysroot; cleanup glib-networking
}

step_libepoxy() {
    log "libepoxy 1.5.10 (EGL only; OptionsWPE requires it unconditionally)"
    fetch https://download.gnome.org/sources/libepoxy/1.5/libepoxy-1.5.10.tar.xz \
        072cda4b59dd098bba8c2363a6247299db1fa89411dc221c8b81b8ee8192e623
    unpack libepoxy "$FETCHED"
    # epoxy resolves GL/EGL entry points at runtime with dlopen/dlsym
    # ("libEGL.so.1"); a CPU-painted payload never calls into it.
    meson_pkg libepoxy -Ddocs=false -Dglx=no -Degl=yes -Dx11=false -Dtests=false
    fix_sysroot; cleanup libepoxy
}

step_libxkbcommon() {
    log "libxkbcommon 1.7.0 (no X11/Wayland; XKB data at $PS5_RUNTIME/share/xkb)"
    fetch https://xkbcommon.org/download/libxkbcommon-1.7.0.tar.xz \
        65782f0a10a4b455af9c6baab7040e2f537520caa2ec2092805cdfd36863b247
    unpack libxkbcommon "$FETCHED"
    meson_pkg libxkbcommon -Denable-x11=false -Denable-wayland=false -Denable-docs=false \
        -Denable-tools=false -Denable-xkbregistry=false -Denable-bash-completion=false \
        -Dxkb-config-root="$PS5_RUNTIME/share/xkb" -Dx-locale-root="$PS5_RUNTIME/share/X11/locale"
    fix_sysroot
    # eaccess() returns ENOSYS as a positive value on PS5 (compat/eaccess.c).
    pc_add_private xkbcommon "$PS5COMPAT_LIBS"
    cleanup libxkbcommon
}

# WPEPlatform's default keymap (wpe_keymap_xkb_new: evdev/pc105/us) is
# compiled from xkeyboard-config data at runtime. Data only (MIT).
step_xkeyboard_config() {
    log "xkeyboard-config 2.43 (XKB data) -> share/xkb in the runtime tree"
    fetch https://www.x.org/releases/individual/data/xkeyboard-config/xkeyboard-config-2.43.tar.xz \
        c810f362c82a834ee89da81e34cd1452c99789339f46f6037f4b9e227dd06c01
    unpack xkeyboard-config "$FETCHED"
    local stage="$W/build/xkeyboard-config-stage"
    rm -rf "$stage"
    ( unset CC CXX; meson setup "$BLD" "$SRC" --prefix=/usr \
        -Dcompat-rules=true -Dxorg-rules-symlinks=false -Dnls=false >"$W/build/xkeyboard-config.configure.log" 2>&1 &&
      ninja -C "$BLD" >"$W/build/xkeyboard-config.build.log" 2>&1 &&
      DESTDIR="$stage" meson install -C "$BLD" --no-rebuild >/dev/null ) || die "xkeyboard-config failed"
    mkdir -p "$RUNTIME/share"
    rm -rf "$RUNTIME/share/xkb"
    # Resolve symlinks (FTP upload and symlink-less /data).
    rsync -aL "$stage/usr/share/X11/xkb/" "$RUNTIME/share/xkb/"
    rm -rf "$stage"; cleanup xkeyboard-config
}

step_lcms2() {
    log "lcms2 2.17 (USE_LCMS)"
    fetch https://github.com/mm2/Little-CMS/releases/download/lcms2.17/lcms2-2.17.tar.gz \
        d11af569e42a1baa1650d20ad61d12e41af4fead4aa7964a01f93b08b53ab074
    unpack lcms2 "$FETCHED"
    meson_pkg lcms2 -Dutils=false -Dtests=disabled -Djpeg=disabled -Dtiff=disabled -Dfastfloat=false -Dthreaded=false
    fix_sysroot; cleanup lcms2
}

# ======================================================= Milestone 2: media
# GStreamer, static, plugins registered at gst_init() (gst-full style), with
# gst-libav on our own FFmpeg (audio + video decoders) and dav1d.

step_dav1d() {
    log "dav1d 1.5.1 (AV1, x86-64 asm)"
    fetch https://downloads.videolan.org/pub/videolan/dav1d/1.5.1/dav1d-1.5.1.tar.xz \
        401813f1f89fa8fd4295805aa5284d9aed9bc7fc1fdbe554af4292f64cbabe21
    unpack dav1d "$FETCHED"
    meson_pkg dav1d -Denable_tools=false -Denable_tests=false -Denable_asm=true -Denable_examples=false
    fix_sysroot; cleanup dav1d
}

step_ffmpeg() {
    log "FFmpeg 7.1.1 (decoders for gst-libav: AAC, MP3, Opus, Vorbis, FLAC, H.264, HEVC, VP8, VP9, AV1 via dav1d)"
    fetch https://ffmpeg.org/releases/ffmpeg-7.1.1.tar.xz \
        733984395e0dbbe5c046abda2dc49a5544e7e0e1e2366bba849222ae9e3a03b1
    unpack ffmpeg "$FETCHED"
    ( cd "$BLD"
      "$SRC/configure" --prefix="$SYSROOT" --enable-cross-compile --cross-prefix="$SDK/bin/prospero-" \
        --cc="$SDK/bin/prospero-clang" --cxx="$SDK/bin/prospero-clang++" \
        --ar="$SDK/bin/prospero-ar" --ranlib="$SDK/bin/prospero-ranlib" --nm="$SDK/bin/prospero-nm" \
        --strip="$SDK/bin/prospero-strip" --pkg-config="$PKGCONF" --pkg-config-flags=--static \
        --arch=x86_64 --target-os=freebsd --enable-static --disable-shared --enable-pic \
        --disable-everything --disable-programs --disable-doc --disable-debug --disable-network \
        --disable-avdevice --disable-swscale --disable-autodetect --enable-pthreads \
        --enable-avfilter --enable-swresample --enable-libdav1d \
        --enable-decoder=aac,aac_fixed,aac_latm,mp3,mp3float,opus,vorbis,flac,pcm_s16le,pcm_f32le,h264,hevc,vp8,vp9,libdav1d \
        --enable-parser=aac,aac_latm,mpegaudio,opus,vorbis,flac,h264,hevc,vp8,vp9,av1 \
        --enable-filter=buffer,buffersink,abuffer,abuffersink,format,aformat,yadif,scale \
        --extra-cflags="$CFLAGS_COMMON -I$SYSROOT/include" --extra-ldflags="-L$SYSROOT/lib" \
        >"$W/build/ffmpeg.configure.log" 2>&1 || { tail -20 "$W/build/ffmpeg.configure.log"; exit 1; }
      make -j"$JOBS" >"$W/build/ffmpeg.build.log" 2>&1 || { grep -E 'error' "$W/build/ffmpeg.build.log" | head -30; exit 1; }
      make install >/dev/null ) || die "ffmpeg failed"
    rm -rf "$SYSROOT/share/ffmpeg"
    fix_sysroot; cleanup ffmpeg
}

GST_VER=1.28.8
GST_COMMON=(-Dauto_features=disabled -Dtests=disabled -Ddoc=disabled -Dglib_debug=disabled)

step_gstreamer() {
    log "GStreamer $GST_VER core (static, gstreamer-static-full: gst_init() calls gst_init_static_plugins())"
    fetch https://gstreamer.freedesktop.org/src/gstreamer/gstreamer-$GST_VER.tar.xz \
        baa8e86985ad6417cc0ad6e7b79ff2884c6779c8b1c7a6240e7121822c86e524
    unpack gstreamer "$FETCHED"
    meson_pkg gstreamer "${GST_COMMON[@]}" -Dexamples=disabled -Dnls=disabled -Dintrospection=disabled -Dgstreamer-static-full=true -Dtools=disabled \
        -Dbenchmarks=disabled -Dcheck=disabled -Dlibunwind=disabled -Dlibdw=disabled \
        -Ddbghelp=disabled -Dbash-completion=disabled -Dptp-helper=disabled -Dcoretracers=disabled \
        -Dgst_debug=true -Dregistry=false -Doption-parsing=true
    fix_sysroot; cleanup gstreamer
}

step_gst_plugins_base() {
    log "gst-plugins-base $GST_VER (app, audio, video, pbutils, tag, fft; playback, typefind, converters; GL off)"
    fetch https://gstreamer.freedesktop.org/src/gst-plugins-base/gst-plugins-base-$GST_VER.tar.xz \
        ade70e3381fcbb024f1f20aa9ede9b5654aeed39d5bd70b1a98f53015e501117
    unpack gst-plugins-base "$FETCHED"
    meson_pkg gst-plugins-base "${GST_COMMON[@]}" -Dexamples=disabled -Dnls=disabled -Dintrospection=disabled -Dtools=disabled -Dorc=disabled -Dgl=disabled \
        -Dapp=enabled -Daudioconvert=enabled -Daudioresample=enabled -Dtypefind=enabled \
        -Dplayback=enabled -Dvideoconvertscale=enabled -Dvolume=enabled -Dpbtypes=enabled \
        -Drawparse=enabled -Dsubparse=enabled -Dgio=enabled -Dgio-typefinder=enabled
    fix_sysroot; cleanup gst-plugins-base
}

step_gst_plugins_good() {
    log "gst-plugins-good $GST_VER (isomp4, matroska, audioparsers, id3demux, adaptivedemux2 HLS/DASH)"
    fetch https://gstreamer.freedesktop.org/src/gst-plugins-good/gst-plugins-good-$GST_VER.tar.xz \
        a7670ed900d3d03be744d25c1873b239896e9a82ce6d17f8febc6fffbaee9521
    unpack gst-plugins-good "$FETCHED"
    meson_pkg gst-plugins-good "${GST_COMMON[@]}" -Dexamples=disabled -Dnls=disabled -Dorc=disabled -Dasm=disabled \
        -Disomp4=enabled -Dmatroska=enabled -Daudioparsers=enabled -Did3demux=enabled \
        -Dapetag=enabled -Dicydemux=enabled -Dwavparse=enabled \
        -Dadaptivedemux2=enabled -Dsoup=enabled -Dsoup-version=3 -Dhls-crypto=openssl
    fix_sysroot; cleanup gst-plugins-good
}

step_gst_plugins_bad() {
    log "gst-plugins-bad $GST_VER (videoparsers, mpegtsdemux + libgstmpegts, codecparsers)"
    fetch https://gstreamer.freedesktop.org/src/gst-plugins-bad/gst-plugins-bad-$GST_VER.tar.xz \
        670d888bde8242579b0d8e91f60cf337aa7be20ca7c315ebfbdd6e5a9218d104
    unpack gst-plugins-bad "$FETCHED"
    meson_pkg gst-plugins-bad "${GST_COMMON[@]}" -Dexamples=disabled -Dnls=disabled -Dintrospection=disabled -Dtools=disabled -Dorc=disabled -Dgpl=disabled \
        -Dvideoparsers=enabled -Dmpegtsdemux=enabled -Did3tag=enabled -Dmpegdemux=enabled
    fix_sysroot; cleanup gst-plugins-bad
}

step_gst_libav() {
    log "gst-libav $GST_VER (avdec_* on FFmpeg 7.1.1)"
    fetch https://gstreamer.freedesktop.org/src/gst-libav/gst-libav-$GST_VER.tar.xz \
        98ad3d7f8932c495e268708903f1c3388d5f26a0db34727a9f861ee23b724177
    unpack gst-libav "$FETCHED"
    meson_pkg gst-libav -Ddoc=disabled -Dtests=disabled
    fix_sysroot; cleanup gst-libav
}

# Plugins registered by port/deps/gst/gststaticplugins.c (keep in sync).
GST_FULL_PLUGINS=(coreelements app audioconvert audioresample typefindfunctions playback
                  videoconvertscale volume pbtypes rawparse subparse gio isomp4 matroska
                  audioparsers id3demux apetag icydemux wavparse adaptivedemux2
                  videoparsersbad mpegtsdemux mpegpsdemux id3tag libav)

# libgstreamer-full-1.0.a: every libgst*-1.0.a, the plugins above, the
# registration object and FFmpeg/dav1d merged into one archive (what
# upstream's gst-full produces and WebKit's USE_GSTREAMER_FULL links), plus
# gstreamer-full-1.0.pc whose Requires.private lists the remaining
# dependencies (glib/gio, libsoup, libxml2, OpenSSL, zlib), which WebKit
# links anyway.
step_gst_full() {
    log "gstreamer-full-1.0 (one archive + gst_init_static_plugins())"
    local b="$W/build/gst-full" lib="$SYSROOT/lib" p mri reqs
    rm -rf "$b"; mkdir -p "$b"
    prospero-clang $CFLAGS_COMMON $("$PKGCONF" --cflags gstreamer-1.0) \
        -c "$HERE/gst/gststaticplugins.c" -o "$b/gststaticplugins.o"
    prospero-ar rcs "$b/libgststaticplugins.a" "$b/gststaticplugins.o"
    mri="$b/full.mri"
    { echo "CREATE $lib/libgstreamer-full-1.0.a"
      echo "ADDLIB $b/libgststaticplugins.a"
      for p in "${GST_FULL_PLUGINS[@]}"; do
          [ -f "$lib/gstreamer-1.0/libgst$p.a" ] || die "missing plugin archive libgst$p.a"
          echo "ADDLIB $lib/gstreamer-1.0/libgst$p.a"
      done
      for p in "$lib"/libgst*-1.0.a; do
          case "$p" in */libgstreamer-full-1.0.a) continue ;; esac
          echo "ADDLIB $p"
      done
      # gst-libav's FFmpeg and dav1d too, so WebKit's USE_GSTREAMER_FULL
      # (which links only this archive) needs nothing media-specific.
      for p in avfilter avformat avcodec swresample avutil dav1d; do
          echo "ADDLIB $lib/lib$p.a"
      done
      echo "SAVE"; echo "END"; } > "$mri"
    rm -f "$lib/libgstreamer-full-1.0.a"
    prospero-ar -M < "$mri"
    prospero-ranlib "$lib/libgstreamer-full-1.0.a"
    # External (non-GStreamer) dependencies of everything merged above.
    local mods=() pc
    for p in "${GST_FULL_PLUGINS[@]}"; do mods+=("gst$p"); done
    for pc in "$lib"/pkgconfig/gstreamer-*-1.0.pc; do
        pc=$(basename "$pc" .pc)
        [ "$pc" = gstreamer-full-1.0 ] || mods+=("$pc")
    done
    reqs=$(PKG_CONFIG_LIBDIR="$lib/pkgconfig:$lib/gstreamer-1.0/pkgconfig:$SYSROOT/share/pkgconfig" \
        pkg-config --print-requires --print-requires-private "${mods[@]}" 2>/dev/null |
        awk '{print $1}' | grep -v -E '^(gst|libav|libswresample|dav1d)' | sort -u | tr '\n' ' ')
    cat > "$lib/pkgconfig/gstreamer-full-1.0.pc" <<PC
prefix=\${pcfiledir}/../..
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: gstreamer-full
Description: Static GStreamer $GST_VER for PS5 payloads: core, libraries and plugins (${GST_FULL_PLUGINS[*]}), registered by gst_init()
Version: $GST_VER
Requires.private: $reqs
Libs: -L\${libdir} -lgstreamer-full-1.0
Cflags: -I\${includedir}/gstreamer-1.0
PC
    echo "Requires.private: $reqs"
    rm -rf "$b"
}

# ------------------------------------------------------------ runtime files
# The console-side tree, staged in $RUNTIME (mirrors $PS5_RUNTIME on the
# console): etc/ssl/cert.pem, etc/fonts/fonts.conf, fonts/, cache/fontconfig/.
# port/deps/smoke/upload-runtime.sh copies it to the console over FTP.
RUNTIME="$SYSROOT/share/ps5webkit-runtime"

step_cacert() {
    log "Mozilla CA bundle 2026-10-20 (curl.se) -> etc/ssl/cert.pem"
    fetch https://curl.se/ca/cacert-2026-10-20.pem \
        796f1389bfea13c0ed78f663b765baaff54a508e5483786bd6ece73c5e3e3cbe
    mkdir -p "$RUNTIME/etc/ssl"
    install -m644 "$FETCHED" "$RUNTIME/etc/ssl/cert.pem"
}

NOTO=https://raw.githubusercontent.com/notofonts/notofonts.github.io/781896b8653bf9e09129c103643a3cc5e31a2cb7/fonts
NOTO_CJK=https://raw.githubusercontent.com/notofonts/noto-cjk/f8d157532fbfaeda587e826d4cd5b21a49186f7c/Sans/OTC
NOTO_EMOJI=https://raw.githubusercontent.com/googlefonts/noto-emoji/v2026-09-24-unicode18_0
FONTS=(
    "$NOTO/NotoSans/hinted/ttf/NotoSans-Regular.ttf 478c558ea716033cd60c03438f628dfa75694dcf6b5f6d505a2f05fd2b4f3823"
    "$NOTO/NotoSans/hinted/ttf/NotoSans-Bold.ttf 1df075a380fc7cb898acf64c1f7b3b4dd780de3caa860178bf929de35817a913"
    "$NOTO/NotoSans/hinted/ttf/NotoSans-Italic.ttf 467e3f89eeca4108bb8710a2b9e0cf2281ac56d5b0609211a83776d0505eecb5"
    "$NOTO/NotoSans/hinted/ttf/NotoSans-BoldItalic.ttf 1b602a9d6353be42c91df097a4857b69fa2696f26703d7a33b54a15d87c2622c"
    "$NOTO/NotoSerif/hinted/ttf/NotoSerif-Regular.ttf 19e72cd8d595fae5bd74a5206f5d938512e1183d4fed7abb1ec1be1d7efa5f88"
    "$NOTO/NotoSerif/hinted/ttf/NotoSerif-Bold.ttf 96656aa5cec8f1d6fd0e804c1fad397e1a1cfa082e6642124e0bda68cd8363ce"
    "$NOTO/NotoSerif/hinted/ttf/NotoSerif-Italic.ttf 749e80e313ef711f9373c6cce17c72297ef05490b3dcda7967d1d5d90bf1183f"
    "$NOTO/NotoSerif/hinted/ttf/NotoSerif-BoldItalic.ttf c710c5b9cf354ae46e7a10472a08019b28220fafeb5887a482a76856f8f6fc0b"
    "$NOTO/NotoSansMono/hinted/ttf/NotoSansMono-Regular.ttf 65b5e2b2c4a1fba9ae8be1f026cb35b03dcb8886d9b2a4147054fde12f7e767d"
    "$NOTO/NotoSansMono/hinted/ttf/NotoSansMono-Bold.ttf a21ea0ba6ea49fda7b34ca39a504b487f1130885d36e1a4f9f4255b3ba6994bc"
    "$NOTO_CJK/NotoSansCJK-Regular.ttc b76b0433203017ca80401b2ee0dd69350349871c4b19d504c34dbdd80541690a"
    "$NOTO_CJK/NotoSansCJK-Bold.ttc faa5f3656a78b2e2d450d27fe8382c778bc2b6bb5ea29c986664a6a435056ceb"
    # Other scripts (example.com's Arabic paragraph was tofu), symbols, math.
    "$NOTO/NotoSansArabic/hinted/ttf/NotoSansArabic-Regular.ttf bdff3e5659d67e67def05b33f749683b9376ae819d65d3dd62ac4640b3aaef48"
    "$NOTO/NotoSansArabic/hinted/ttf/NotoSansArabic-Bold.ttf 4e5462d2e8be880317b9f49b5b2da109ddb6a3563d91cc604b67f3535832a555"
    "$NOTO/NotoSansHebrew/hinted/ttf/NotoSansHebrew-Regular.ttf cdefaf8efd47045f6820928eba84db5bed7557539328952b5f828315485e02ee"
    "$NOTO/NotoSansHebrew/hinted/ttf/NotoSansHebrew-Bold.ttf da9226e886c245a7e11673c24dec82bded64d8574c1e1f03983bf89297d2aaa8"
    "$NOTO/NotoSansDevanagari/hinted/ttf/NotoSansDevanagari-Regular.ttf 4e3c66638958c3e2ab5d37f47a8deb89fffeb7be9985c665a519bbc7ba762313"
    "$NOTO/NotoSansDevanagari/hinted/ttf/NotoSansDevanagari-Bold.ttf 6a09c8d797cfc803d32cdc731e809424d74cbaff59f503de34ade421a08e5bc2"
    "$NOTO/NotoSansThai/hinted/ttf/NotoSansThai-Regular.ttf 61cf814eec46b294d6ea4401ac295d0cecd5207bd2331dcc5a15e7301d30ee44"
    "$NOTO/NotoSansThai/hinted/ttf/NotoSansThai-Bold.ttf 2ac6c6e8a478e23b15f76e4894af1fa2210f8f350e4e6e54aad530bec03efbfb"
    "$NOTO/NotoSansBengali/hinted/ttf/NotoSansBengali-Regular.ttf b55c62ee531e3214da6c0701daecea89a52ba42db7d8206b92e6b51f397a3193"
    "$NOTO/NotoSansTamil/hinted/ttf/NotoSansTamil-Regular.ttf 3c0a186feb3c63c7f6d63e1511dcdc144e745ae09b98e217c83f3e317974f6f9"
    "$NOTO/NotoSansArmenian/hinted/ttf/NotoSansArmenian-Regular.ttf 720df88c332417a235b4d6209d14ec2e2bf4bfe2a954b7453d869ea593bfce1e"
    "$NOTO/NotoSansGeorgian/hinted/ttf/NotoSansGeorgian-Regular.ttf d3e33254b09e7bb2c5cf0f17e554b80462056c5a107097f258d495168c3a9346"
    "$NOTO/NotoSansEthiopic/hinted/ttf/NotoSansEthiopic-Regular.ttf f6f7fc379db9438959a2b0527e7a2cf36ea9c84626d56ec444fff37fc24c3c10"
    "$NOTO/NotoSansSymbols/hinted/ttf/NotoSansSymbols-Regular.ttf d0e98e9a2c046594c5021437273943be7e79e0fd980fde125279e22302212595"
    "$NOTO/NotoSansSymbols2/hinted/ttf/NotoSansSymbols2-Regular.ttf c4a0a80f0041ce4be81e2478faad22776d23edb98ae3f0d19bd37044820ecf9d"
    "$NOTO/NotoSansMath/hinted/ttf/NotoSansMath-Regular.ttf d51afd5739c7ba6c44fcab35a88160e25dfb69a2d4ad0bd99533f8d894af1f96"
    # Emoji: CBDT colour bitmaps (FreeType has libpng), Unicode 18 release.
    "$NOTO_EMOJI/2D/fonts/NotoColorEmoji.ttf 15671215ab769fdc7162a045d56fd7d7e477c51b04e6b3c761d914d8fdd6cc44"
)
# OFL-1.1 texts, installed next to the fonts.
FONT_LICENSES=(
    "$NOTO/LICENSE f2095b08bed08b23a6fe26112fcd679a2bee3f002eef077eb05d215ed1051bd8 OFL-Noto.txt"
    "$NOTO_EMOJI/LICENSE 500bb1ccf43df7bbb522112f9133a52b16e1c35e809632f5d8609b179152de5b OFL-NotoColorEmoji.txt"
)

step_fonts() {
    log "Noto fonts (OFL-1.1) -> fonts/, fontconfig config -> etc/fonts/"
    local e url sum
    mkdir -p "$RUNTIME/fonts" "$RUNTIME/etc/fonts" "$RUNTIME/cache/fontconfig"
    for e in "${FONTS[@]}"; do
        url=${e% *}; sum=${e##* }
        fetch "$url" "$sum" "fonts/$(basename "$url")"
        install -m644 "$FETCHED" "$RUNTIME/fonts/"
    done
    for e in "${FONT_LICENSES[@]}"; do
        read -r url sum name <<< "$e"
        fetch "$url" "$sum" "fonts/$name"
        install -m644 "$FETCHED" "$RUNTIME/fonts/$name"
    done
    install -m644 "$HERE/runtime/etc/fonts/fonts.conf" "$RUNTIME/etc/fonts/fonts.conf"
}

# MIME types by file extension (no shared-mime-info on the console): a
# hand-written globs2 for xdgmime (WebKit's and GIO's copies); the children
# set XDG_DATA_DIRS=/data/ps5webkit/share.
step_mime() {
    log "share/mime/globs2 (file extension -> MIME type)"
    mkdir -p "$RUNTIME/share/mime"
    install -m644 "$HERE/runtime/share/mime/globs2" "$RUNTIME/share/mime/globs2"
}

STEPS_M1=(tools compat zlib brotli libpng libjpeg_turbo libwebp expat libiconv proxy_libintl
          libxml2 libxslt sqlite libpsl icu freetype harfbuzz fontconfig pcre2 libffi glib
          openssl nghttp2 libsoup glib_networking libepoxy libxkbcommon lcms2
          cacert fonts mime xkeyboard_config)
STEPS_M2=(dav1d ffmpeg gstreamer gst_plugins_base gst_plugins_good gst_plugins_bad gst_libav gst_full)

if [ "${1:-}" = "--list" ]; then
    echo "# Milestone 1 (default)"; printf '%s\n' "${STEPS_M1[@]}" | tr _ -
    echo "# Milestone 2 (media): build.sh m2"; printf '%s\n' "${STEPS_M2[@]}" | tr _ -
    exit 0
fi
STEPS=("$@")
[ ${#STEPS[@]} -eq 0 ] && STEPS=("${STEPS_M1[@]}")
[ "${STEPS[*]}" = m2 ] && STEPS=("${STEPS_M2[@]}")
[ "${STEPS[*]}" = all ] && STEPS=("${STEPS_M1[@]}" "${STEPS_M2[@]}")
for s in "${STEPS[@]}"; do
    fn="step_${s//-/_}"
    declare -F "$fn" >/dev/null || die "unknown step: $s (try --list)"
    "$fn"
done
