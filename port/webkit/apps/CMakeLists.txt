# PS5 executables built with WebKit (port/webkit/apps; added from the top
# level CMakeLists.txt by patch 0010 when PS5_PORT_DIR is set).
#
#   ps5wk-child   payload: WebProcess + NetworkProcess (multi-call, chosen by
#                 the launch header), started by the UIProcess through elfldr
#   ps5wk-shot    payload: a UIProcess on WPE's headless display that loads a
#                 URL and reports a checksum + thumbnail of the painted page
#   ps5browser    the UIProcess linked as a payload-style ELF; port/webkit/
#                 apps/title/package.sh relinks its objects as the title
set(PS5_APPS_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(PS5_WPE_DIR ${PS5_PORT_DIR}/wpe-ps5)

set(PS5_API_INCLUDE_DIRECTORIES
    ${CMAKE_BINARY_DIR}
    ${DERIVED_SOURCES_WPE_API_DIR}
    ${FORWARDING_HEADERS_WPE_DIR}
    ${FORWARDING_HEADERS_DIR}
    ${JavaScriptCoreGLib_DERIVED_SOURCES_DIR}
    ${JavaScriptCoreGLib_FRAMEWORK_HEADERS_DIR}
    ${WPEPlatform_DERIVED_SOURCES_DIR}
    ${WEBKIT_DIR}/WPEPlatform
    ${PS5_WPE_DIR}
)

add_executable(ps5wk-child ${PS5_APPS_DIR}/child/ps5wk-child.cpp)
target_include_directories(ps5wk-child PRIVATE ${PS5_WPE_DIR})
target_link_libraries(ps5wk-child PRIVATE WebKit)

add_executable(ps5wk-shot ${PS5_APPS_DIR}/shot/ps5wk-shot.c)
target_include_directories(ps5wk-shot PRIVATE ${PS5_API_INCLUDE_DIRECTORIES})
target_link_libraries(ps5wk-shot PRIVATE WebKit GLib::Gio)
add_dependencies(ps5wk-shot WebKit)

set(PS5WK_LOG_HOST "192.168.1.20" CACHE STRING "Mac address for the title's TCP log (tools/logserver.sh)")
set(PS5WK_LOG_PORT "9999" CACHE STRING "Port of tools/logserver.sh")
add_executable(ps5browser ${PS5_APPS_DIR}/title/ps5browser.c)
target_include_directories(ps5browser PRIVATE ${PS5_API_INCLUDE_DIRECTORIES} ${PS5_PORT_DIR}/../../spikes)
target_compile_definitions(ps5browser PRIVATE LOG_HOST="${PS5WK_LOG_HOST}" LOG_PORT=${PS5WK_LOG_PORT})
target_link_libraries(ps5browser PRIVATE WebKit GLib::Gio)
add_dependencies(ps5browser WebKit)
