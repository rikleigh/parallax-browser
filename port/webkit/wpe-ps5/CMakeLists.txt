# PS5 WPE Platform backend (port/webkit/wpe-ps5, linked into the tree as
# Source/WebKit/WPEPlatform/wpe/ps5 by port/webkit/build.sh).
set(WPEPlatformPS5_SOURCES
    ${WEBKIT_DIR}/WPEPlatform/wpe/ps5/PS5GLBench.cpp
    ${WEBKIT_DIR}/WPEPlatform/wpe/ps5/PS5Renderer.cpp
    ${WEBKIT_DIR}/WPEPlatform/wpe/ps5/PS5RendererVR.cpp
    ${WEBKIT_DIR}/WPEPlatform/wpe/ps5/PS5Text.cpp
    ${WEBKIT_DIR}/WPEPlatform/wpe/ps5/PS5VideoDecoder.cpp
    ${WEBKIT_DIR}/WPEPlatform/wpe/ps5/PS5VideoServer.cpp
    ${WEBKIT_DIR}/WPEPlatform/wpe/ps5/WPEDisplayPS5.cpp
    ${WEBKIT_DIR}/WPEPlatform/wpe/ps5/WPEInputMethodContextPS5.cpp
    ${WEBKIT_DIR}/WPEPlatform/wpe/ps5/WPEProcessManagerPS5.cpp
    ${WEBKIT_DIR}/WPEPlatform/wpe/ps5/WPEToplevelPS5.cpp
    ${WEBKIT_DIR}/WPEPlatform/wpe/ps5/WPEViewPS5.cpp
)

set(WPEPlatformPS5_PRIVATE_INCLUDE_DIRECTORIES
    "${WPEPlatform_PRIVATE_INCLUDE_DIRECTORIES}"
    "${WEBKIT_DIR}/WPEPlatform/wpe/ps5"
)

add_library(WPEPlatformPS5 OBJECT ${WPEPlatformPS5_SOURCES})
target_include_directories(WPEPlatformPS5 PRIVATE ${WPEPlatformPS5_PRIVATE_INCLUDE_DIRECTORIES})
target_include_directories(WPEPlatformPS5 SYSTEM PRIVATE ${WPEPlatform_SYSTEM_INCLUDE_DIRECTORIES})
# View in VR: FreeType for the title's own text (PS5Text.cpp) and the PS VR2
# backend's header (spikes/common/psvr2; its code is linked into the title).
target_include_directories(WPEPlatformPS5 SYSTEM PRIVATE ${FREETYPE_INCLUDE_DIR_ft2build} ${PS5_PORT_DIR}/../../spikes/common)
target_link_libraries(WPEPlatformPS5 ${WPEPlatform_LIBRARIES})
add_dependencies(WPEPlatformPS5 WPEPlatformGeneratedEnumTypesHeader)
