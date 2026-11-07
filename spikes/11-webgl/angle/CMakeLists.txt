# ANGLE for the PS5 title, out of tree: libANGLE (ES front end, ESSL/GLSL
# translator, validation) + the GL backend over EGL + the GL_*/EGL_*
# entry points WebKit calls, as one static archive. The unprefixed gl*/egl*
# shells (libGLESv2_autogen.cpp, libEGL) are left out: ps5-opengl exports those
# names in the same process.
#
#   cmake -S spikes/11-webgl/angle -B build/angle-ps5/build -G Ninja \
#       -DCMAKE_TOOLCHAIN_FILE=$PS5_PAYLOAD_SDK/toolchain/prospero.cmake \
#       -DANGLE_SRC=build/angle-ps5/src -DLIBCXX=toolchain/libcxx19/install
#
# The source lists come from ANGLE's own gni-to-cmake files (unchanged).
cmake_minimum_required(VERSION 3.20)
project(angle_ps5 CXX C)

set(ANGLE_SRC "" CACHE PATH "ANGLE source tree (a patched copy)")
set(LIBCXX "" CACHE PATH "libc++ 19 install prefix (nostdinc++)")
set(ZLIB_INCLUDE "" CACHE PATH "zlib include dir")
# ANGLE_PS5_HOST: the same selection built for x86_64 macOS (Spike 2's
# Rosetta host check of ps5-opengl), with the host's libc++ and a host
# system_utils (hostcheck/system_utils_host.cpp) instead of the Linux one.
option(ANGLE_PS5_HOST "build for the macOS x86_64 host check" OFF)
set(HOST_EXTRA_SOURCES "" CACHE STRING "extra sources for the host build")
if (NOT ANGLE_SRC OR (NOT LIBCXX AND NOT ANGLE_PS5_HOST))
    message(FATAL_ERROR "set ANGLE_SRC and LIBCXX")
endif ()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Select the Linux/EGL source sets from ANGLE's lists (the WPE port does the
# same: PlatformWPE.cmake + linux.cmake with is_linux).
set(is_linux TRUE)
set(angle_enable_cl FALSE)
set(angle_has_astc_encoder FALSE)
set(angle_use_x11 FALSE)
include(${ANGLE_SRC}/Compiler.cmake)
include(${ANGLE_SRC}/GLESv2.cmake)
include(${ANGLE_SRC}/GL.cmake)
include(${ANGLE_SRC}/linux.cmake)

set(sources
    ${libangle_common_sources}
    ${libangle_common_shader_state_sources}
    ${angle_preprocessor_sources}
    ${angle_translator_glsl_base_sources}
    ${angle_translator_essl_sources}
    ${angle_translator_essl_symbol_table_sources}
    ${angle_translator_glsl_and_vulkan_base_sources}
    ${angle_translator_glsl_sources}
    ${angle_translator_sources}
    src/common/angle_version_info.cpp
    src/libANGLE/capture/FrameCapture_mock.cpp
    src/libANGLE/capture/serialize_mock.cpp
    ${libangle_sources}
    ${libangle_image_util_sources}
    ${xxhash_sources}
    third_party/zlib/google/compression_utils_portable.cc
    ${gl_backend_sources}
    ${angle_dma_buf_sources}
    ${libangle_gpu_info_util_sources}
    ${libangle_gpu_info_util_linux_sources}
    ${libglesv2_entry_point_sources}
)
list(FILTER sources EXCLUDE REGEX "\\.(h|inc|inl)$")
list(REMOVE_DUPLICATES sources)
list(TRANSFORM sources PREPEND "${ANGLE_SRC}/")
if (ANGLE_PS5_HOST)
    list(FILTER sources EXCLUDE REGEX "system_utils_linux\\.cpp$")
    list(APPEND sources ${HOST_EXTRA_SOURCES})
    # Compiler.cpp calls UnfoldShortCircuitAST on Apple platforms.
    set(apple_sources ${angle_translator_glsl_apple_sources})
    list(FILTER apple_sources EXCLUDE REGEX "\\.h$")
    list(TRANSFORM apple_sources PREPEND "${ANGLE_SRC}/")
    list(APPEND sources ${apple_sources})
endif ()

add_library(angle_ps5 STATIC ${sources})
target_include_directories(angle_ps5 PRIVATE
    ${ANGLE_SRC}/WebKit
    ${ANGLE_SRC}/include
    ${ANGLE_SRC}/include/KHR
    ${ANGLE_SRC}/src
    ${ANGLE_SRC}/src/common/base
    ${ANGLE_SRC}/src/common/third_party/xxhash
    ${ANGLE_SRC}/third_party/zlib/google
    ${ZLIB_INCLUDE}
)
target_compile_definitions(angle_ps5 PRIVATE
    ANGLE_ENABLE_ESSL
    ANGLE_ENABLE_GLSL
    ANGLE_ENABLE_OPENGL
    ANGLE_PLATFORM_LINUX
    EGL_NO_PLATFORM_SPECIFIC_TYPES
    GL_GLES_PROTOTYPES=0
    EGL_EGL_PROTOTYPES=0
    LIBANGLE_IMPLEMENTATION
    LIBGLESV2_IMPLEMENTATION
    ANGLE_PLATFORM_PS5
    ANGLE_USE_STATIC_THREAD_LOCAL_VARIABLES
    NDEBUG
)
if (ANGLE_PS5_HOST)
    target_compile_options(angle_ps5 PRIVATE -O1 -g -w -arch x86_64 -mmacosx-version-min=13.0)
else ()
    target_compile_options(angle_ps5 PRIVATE
        -O2 -w -ffunction-sections -fdata-sections -fno-plt -fno-stack-protector
        $<$<COMPILE_LANGUAGE:CXX>:-nostdinc++>
        $<$<COMPILE_LANGUAGE:CXX>:-isystem${LIBCXX}/include/c++/v1>
    )
endif ()
