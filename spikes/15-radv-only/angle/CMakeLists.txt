# ANGLE on Vulkan for the PS5 title (spike 15a), out of tree: libANGLE (ES
# front end, ESSL translator, SPIR-V output, validation) + the Vulkan backend
# with ANGLE's "simple" display (VK_KHR_display, which PS5_Mesa's RADV
# implements on VideoOut) + the GL_*/EGL_* entry points WebKit calls, as one
# static archive. No GL backend: the title links RADV and no ps5-opengl.
#
# Based on spikes/11-webgl/angle/CMakeLists.txt (the GL backend build). The
# Vulkan source lists come from Vulkan.cmake, which build-angle-vk.sh writes
# from ANGLE's vulkan_backend.gni (WebKit's copy has no Vulkan.cmake).
cmake_minimum_required(VERSION 3.20)
project(angle_vk_ps5 CXX C)

set(ANGLE_SRC "" CACHE PATH "ANGLE source tree (a patched copy)")
set(LIBCXX "" CACHE PATH "libc++ 19 install prefix (nostdinc++)")
set(ZLIB_INCLUDE "" CACHE PATH "zlib include dir")
set(VULKAN_INCLUDE "" CACHE PATH "Vulkan headers (RADV's install: vulkan/, vk_video/)")
set(SPIRV_HEADERS_INCLUDE "" CACHE PATH "SPIRV-Headers include dir (spirv/unified1)")
set(SPIRV_TOOLS_INCLUDE "" CACHE PATH "SPIRV-Tools include dir (headers only)")
set(VMA_DIR "" CACHE PATH "VulkanMemoryAllocator checkout (include/, vma.cpp)")
set(PS5_EXTRA_SOURCES "" CACHE STRING "spike sources added to the archive")
foreach (v ANGLE_SRC LIBCXX VULKAN_INCLUDE SPIRV_HEADERS_INCLUDE SPIRV_TOOLS_INCLUDE VMA_DIR)
    if (NOT ${v})
        message(FATAL_ERROR "set ${v}")
    endif ()
endforeach ()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(is_linux TRUE)
set(angle_enable_cl FALSE)
set(angle_has_astc_encoder FALSE)
set(angle_use_x11 FALSE)
include(${ANGLE_SRC}/Compiler.cmake)
include(${ANGLE_SRC}/GLESv2.cmake)
include(${ANGLE_SRC}/linux.cmake)
include(${ANGLE_SRC}/Vulkan.cmake)

set(sources
    ${libangle_common_sources}
    ${libangle_common_shader_state_sources}
    ${angle_preprocessor_sources}
    ${angle_translator_glsl_base_sources}
    ${angle_translator_essl_sources}
    # TranslatorESSL calls the atan emulator, which ANGLE lists with the GLSL
    # output (angle_translator_glsl_sources); only that file is needed.
    src/compiler/translator/glsl/BuiltInFunctionEmulatorGLSL.cpp
    ${angle_translator_essl_symbol_table_sources}
    ${angle_translator_glsl_and_vulkan_base_sources}
    ${angle_translator_lib_spirv_sources}
    ${angle_translator_sources}
    src/common/angle_version_info.cpp
    src/libANGLE/capture/FrameCapture_mock.cpp
    src/libANGLE/capture/serialize_mock.cpp
    ${libangle_sources}
    ${libangle_image_util_sources}
    ${xxhash_sources}
    third_party/zlib/google/compression_utils_portable.cc
    ${vulkan_backend_sources}
    ${vulkan_backend_dma_buf_sources}
    src/libANGLE/renderer/vulkan/vk_mem_alloc_wrapper.cpp
    src/common/vulkan/libvulkan_loader.cpp
    src/common/vulkan/vulkan_icd.cpp
    src/common/spirv/spirv_instruction_builder_autogen.cpp
    src/common/spirv/spirv_instruction_parser_autogen.cpp
    src/third_party/volk/volk.c
    ${angle_dma_buf_sources}
    ${libangle_gpu_info_util_sources}
    ${libangle_gpu_info_util_linux_sources}
    ${libangle_gpu_info_util_vulkan_sources}
    ${libglesv2_entry_point_sources}
)
list(FILTER sources EXCLUDE REGEX "\\.(h|inc|inl)$")
list(REMOVE_DUPLICATES sources)
list(TRANSFORM sources PREPEND "${ANGLE_SRC}/")
# angle_spirv_utils.cpp (SPIR-V validation and printing, used from asserts
# only) needs SPIRV-Tools' libraries; src/ps5_spirv_utils.cpp stands in.
list(APPEND sources ${PS5_EXTRA_SOURCES})
# VMA's implementation, as ANGLE's GN build compiles it (VMA's BUILD.gn).
list(APPEND sources ${VMA_DIR}/vma.cpp)

add_library(angle_vk_ps5 STATIC ${sources})
target_include_directories(angle_vk_ps5 PRIVATE
    ${ANGLE_SRC}/WebKit
    ${ANGLE_SRC}/include
    ${ANGLE_SRC}/include/KHR
    ${ANGLE_SRC}/src
    ${ANGLE_SRC}/src/common/base
    ${ANGLE_SRC}/src/common/spirv
    ${ANGLE_SRC}/src/common/third_party/xxhash
    ${ANGLE_SRC}/src/third_party/volk
    ${ANGLE_SRC}/third_party/zlib/google
    ${VULKAN_INCLUDE}
    ${SPIRV_HEADERS_INCLUDE}
    ${SPIRV_TOOLS_INCLUDE}
    ${VMA_DIR}/include
    ${ZLIB_INCLUDE}
)
target_compile_definitions(angle_vk_ps5 PRIVATE
    ANGLE_ENABLE_ESSL
    ANGLE_ENABLE_VULKAN
    ANGLE_USE_VULKAN_DISPLAY
    ANGLE_VULKAN_DISPLAY_MODE_SIMPLE
    ANGLE_SHARED_LIBVULKAN=1
    ANGLE_USE_CUSTOM_VULKAN_OUTSIDE_RENDER_PASS_CMD_BUFFERS=1
    ANGLE_USE_CUSTOM_VULKAN_RENDER_PASS_CMD_BUFFERS=1
    ANGLE_VULKAN_API_PERF_COUNTERS_MODE=0
    ANGLE_ENABLE_CRC_FOR_PIPELINE_CACHE
    # src/common/vulkan/vulkan_icd.cpp: paths for the mock ICD and layers,
    # which the title never loads (as GN's angle_vulkan_icd sets them).
    ANGLE_VK_MOCK_ICD_JSON="VkICD_mock_icd.json"
    ANGLE_VK_LAYERS_DIR="."
    # VMA's BUILD.gn: ANGLE hands VMA its function table.
    VMA_DYNAMIC_VULKAN_FUNCTIONS=0
    VMA_STATIC_VULKAN_FUNCTIONS=0
    VMA_USE_STL_CONTAINERS=1
    ANGLE_PLATFORM_LINUX
    VK_NO_PROTOTYPES
    EGL_NO_PLATFORM_SPECIFIC_TYPES
    GL_GLES_PROTOTYPES=0
    EGL_EGL_PROTOTYPES=0
    LIBANGLE_IMPLEMENTATION
    LIBGLESV2_IMPLEMENTATION
    ANGLE_PLATFORM_PS5
    ANGLE_USE_STATIC_THREAD_LOCAL_VARIABLES
    NDEBUG
)
target_compile_options(angle_vk_ps5 PRIVATE
    -O2 -w -ffunction-sections -fdata-sections -fno-plt -fno-stack-protector
    $<$<COMPILE_LANGUAGE:CXX>:-nostdinc++>
    $<$<COMPILE_LANGUAGE:CXX>:-isystem${LIBCXX}/include/c++/v1>
)
