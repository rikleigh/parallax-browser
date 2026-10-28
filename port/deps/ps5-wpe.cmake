# CMake toolchain file: PS5 payloads (ps5-payload-sdk) against the WPE
# dependency sysroot (toolchain/wpe-sysroot).
#
#   cmake -G Ninja -DCMAKE_TOOLCHAIN_FILE=port/deps/ps5-wpe.cmake ...
#
# It layers on the SDK's prospero.cmake (FreeBSD/x86_64, CMAKE_SYSROOT = SDK
# target, find mode ONLY) and adds:
#   - toolchain/wpe-sysroot to CMAKE_FIND_ROOT_PATH, so find_library/find_path
#     /find_package see its lib/, include/ and lib/cmake/;
#   - pkg-config through port/deps/pkg-config-ps5 (PKG_CONFIG_LIBDIR = the
#     sysroot's pkgconfig dirs, PKG_CONFIG_SYSROOT_DIR empty, always --static);
#   - PS5_WPE_SYSROOT and PS5_LIBCXX19_DIR variables for the consumer.
#
# It does not change compile flags. C++ consumers that need libc++ 19 (WebKit
# does) add, as spikes/04-jsc/build.sh does:
#   CXXFLAGS:  -nostdinc++ -isystem ${PS5_LIBCXX19_DIR}/include/c++/v1
#   link:      -nostdlib++ ${PS5_LIBCXX19_DIR}/lib/libc++.a libc++abi.a libunwind.a
# and -DLD_PACK_RELATIVE_RELOCS=OFF for WebKit (the SDK loader ignores DT_RELR).

get_filename_component(_ps5wpe_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

include("${_ps5wpe_root}/toolchain/ps5-payload-sdk/toolchain/prospero.cmake")

if (NOT PS5_WPE_SYSROOT)
    set(PS5_WPE_SYSROOT "${_ps5wpe_root}/toolchain/wpe-sysroot")
endif ()
set(PS5_WPE_SYSROOT "${PS5_WPE_SYSROOT}" CACHE PATH "PS5 WPE dependency sysroot")
set(PS5_LIBCXX19_DIR "${_ps5wpe_root}/toolchain/libcxx19/install" CACHE PATH "libc++ 19 for PS5")

# find_* re-root every search path under each CMAKE_FIND_ROOT_PATH entry
# (mode ONLY, from prospero.cmake): /lib -> ${PS5_WPE_SYSROOT}/lib, etc.
list(APPEND CMAKE_FIND_ROOT_PATH "${PS5_WPE_SYSROOT}")
list(REMOVE_DUPLICATES CMAKE_FIND_ROOT_PATH)

set(PKG_CONFIG_EXECUTABLE "${CMAKE_CURRENT_LIST_DIR}/pkg-config-ps5" CACHE FILEPATH "" FORCE)
set(ENV{PKG_CONFIG_LIBDIR} "${PS5_WPE_SYSROOT}/lib/pkgconfig:${PS5_WPE_SYSROOT}/share/pkgconfig")
set(ENV{PKG_CONFIG_PATH} "")
set(ENV{PKG_CONFIG_SYSROOT_DIR} "")

# prospero.cmake turns this on; it floods build logs.
set(CMAKE_VERBOSE_MAKEFILE OFF CACHE BOOL "" FORCE)
