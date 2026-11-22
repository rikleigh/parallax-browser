#!/usr/bin/env bash
# libarchive 3.8.1 with port/deps/patches/libarchive (RAR5 decryption) for
# the Mac tests, the way port/deps/build.sh builds it for the PS5: with
# OpenSSL as its crypto. Built once per set of patches into
# toolchain/mac-libarchive-3.8.1-<hash> (bsdtar too); prints that folder.
#   port/webkit/test/downloads/mac-libarchive.sh
# OPENSSL_ROOT: Homebrew's openssl@3 by default.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../../../.." && pwd)"
patches="$root/port/deps/patches/libarchive"
tarball="$root/toolchain/wpe-deps/dl/libarchive-3.8.1.tar.xz"
url=https://www.libarchive.org/downloads/libarchive-3.8.1.tar.xz
sha=19f917d42d530f98815ac824d90c7eaf648e9d9a50e4f309c812457ffa5496b5
hash="$(cat "$patches"/*.patch | shasum -a 256 | cut -c1-12)"
prefix="$root/toolchain/mac-libarchive-3.8.1-$hash"
if [ -f "$prefix/lib/libarchive.dylib" ] && [ -x "$prefix/bin/bsdtar" ]; then
    echo "$prefix"
    exit 0
fi
openssl="${OPENSSL_ROOT:-$(brew --prefix openssl@3 2>/dev/null || echo /opt/homebrew/opt/openssl@3)}"
[ -f "$openssl/include/openssl/evp.h" ] || { echo "mac-libarchive: no OpenSSL at $openssl (brew install openssl@3)" >&2; exit 1; }
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
if [ ! -s "$tarball" ]; then
    mkdir -p "$(dirname "$tarball")"
    curl -sSfL -o "$tarball.part" "$url" && mv "$tarball.part" "$tarball"
fi
echo "$sha  $tarball" | shasum -a 256 -c - >/dev/null || { echo "mac-libarchive: $tarball: wrong sha256" >&2; exit 1; }
mkdir -p "$work/src"
tar -xf "$tarball" -C "$work/src" --strip-components=1
for p in "$patches"/*.patch; do
    patch -s -d "$work/src" -p1 <"$p"
done
echo "mac-libarchive: building $prefix" >&2
cmake -G Ninja -S "$work/src" -B "$work/build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_INSTALL_LIBDIR=lib \
    -DENABLE_TEST=OFF -DENABLE_TAR=ON -DENABLE_CPIO=OFF -DENABLE_CAT=OFF -DENABLE_UNZIP=OFF -DENABLE_WERROR=OFF \
    -DENABLE_OPENSSL=ON -DENABLE_OPENSSL_DARWIN=ON -DOPENSSL_ROOT_DIR="$openssl" -DENABLE_MBEDTLS=OFF -DENABLE_NETTLE=OFF \
    -DENABLE_LIBB2=OFF -DENABLE_LZ4=OFF -DENABLE_LZO=OFF -DENABLE_LIBXML2=OFF -DENABLE_EXPAT=OFF -DENABLE_ICONV=ON \
    -DENABLE_ACL=OFF -DENABLE_XATTR=OFF -DENABLE_PCREPOSIX=OFF -DENABLE_PCRE2POSIX=OFF \
    -DENABLE_ZLIB=ON -DENABLE_BZip2=ON -DENABLE_LZMA=ON -DENABLE_ZSTD=ON -DCMAKE_PREFIX_PATH=/opt/homebrew \
    -DCMAKE_INSTALL_RPATH="$prefix/lib" >"$work/configure.log" 2>&1 || { tail -30 "$work/configure.log" >&2; exit 1; }
grep -q '#define HAVE_LIBCRYPTO 1' "$work/build/config.h" || { echo "mac-libarchive: OpenSSL wasn't found" >&2; exit 1; }
ninja -C "$work/build" install >"$work/build.log" 2>&1 || { tail -20 "$work/build.log" >&2; exit 1; }
echo "$prefix"
