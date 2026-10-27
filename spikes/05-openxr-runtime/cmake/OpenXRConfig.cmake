# OpenXRConfig.cmake - lets WebKit's `find_package(OpenXR REQUIRED CONFIG)`
# (Source/cmake/OptionsWPE.cmake) resolve to libopenxr_psvr2 instead of the
# Khronos loader. Point CMake at this directory:
#   -DOpenXR_DIR=<repo>/spikes/05-openxr-runtime/cmake
# The PS5 archive exports the core xr* symbols, so WebKit's direct calls link
# without a loader. Its own dependencies (ps5-opengl's GL/EGL, libSceVideoOut,
# libSceSysmodule, libSceUserService, libkernel) are listed below; the final
# link still has to provide the ps5-opengl libraries.

get_filename_component(_xrpsvr2_dir "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
get_filename_component(_xrpsvr2_root "${_xrpsvr2_dir}/../.." ABSOLUTE)

set(OpenXR_VERSION 1.1.65) # headers: toolchain/khronos/include/openxr (release-1.1.65)
set(OpenXR_FOUND TRUE)

if (NOT TARGET OpenXR::openxr_loader)
    add_library(OpenXR::openxr_loader STATIC IMPORTED GLOBAL)
    set_target_properties(OpenXR::openxr_loader PROPERTIES
        IMPORTED_LOCATION "${_xrpsvr2_dir}/build/ps5/libopenxr_psvr2.a"
        INTERFACE_INCLUDE_DIRECTORIES "${_xrpsvr2_root}/toolchain/khronos/include;${_xrpsvr2_dir}/runtime"
        INTERFACE_LINK_LIBRARIES "SceVideoOut;SceSysmodule;SceUserService"
    )
endif ()
if (NOT TARGET OpenXR::headers)
    add_library(OpenXR::headers INTERFACE IMPORTED GLOBAL)
    set_target_properties(OpenXR::headers PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${_xrpsvr2_root}/toolchain/khronos/include")
endif ()
