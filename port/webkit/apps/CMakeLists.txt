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
    ${DERIVED_SOURCES_WEBKIT_DIR}
    ${FORWARDING_HEADERS_WPE_DIR}
    ${FORWARDING_HEADERS_DIR}
    ${JavaScriptCoreGLib_DERIVED_SOURCES_DIR}
    ${JavaScriptCoreGLib_FRAMEWORK_HEADERS_DIR}
    ${WPEPlatform_DERIVED_SOURCES_DIR}
    ${WEBKIT_DIR}/WPEPlatform
    ${PS5_WPE_DIR}
)

# WebKit is a static library here, but WTF, bmalloc, JavaScriptCore, PAL and
# WebCore (and their subtargets) are OBJECT libraries linked privately into
# it, and CMake does not carry their objects into a static archive or its
# consumers. Collect every OBJECT library's objects into one thin archive
# that the executables link after WebKit (archive members are only pulled in
# when needed, so objects already on the link line are not duplicated).
function(_ps5_collect_targets _dir _out)
    get_property(_targets DIRECTORY ${_dir} PROPERTY BUILDSYSTEM_TARGETS)
    get_property(_subdirs DIRECTORY ${_dir} PROPERTY SUBDIRECTORIES)
    foreach (_sub IN LISTS _subdirs)
        _ps5_collect_targets(${_sub} _subtargets)
        list(APPEND _targets ${_subtargets})
    endforeach ()
    set(${_out} ${_targets} PARENT_SCOPE)
endfunction()
_ps5_collect_targets(${CMAKE_SOURCE_DIR}/Source _ps5_all_targets)
set(PS5_OBJECT_LIBRARIES "")
foreach (_t IN LISTS _ps5_all_targets)
    get_target_property(_type ${_t} TYPE)
    if (_type STREQUAL "OBJECT_LIBRARY")
        list(APPEND PS5_OBJECT_LIBRARIES ${_t})
    endif ()
endforeach ()
set(_ps5_objects "")
foreach (_t IN LISTS PS5_OBJECT_LIBRARIES)
    list(APPEND _ps5_objects "$<FILTER:$<TARGET_OBJECTS:${_t}>,EXCLUDE,\\.(g|p)ch$>")
endforeach ()
file(GENERATE OUTPUT ${CMAKE_BINARY_DIR}/ps5-apps/objects.rsp CONTENT "$<JOIN:${_ps5_objects},\n>\n")
set(PS5_OBJECTS_ARCHIVE ${CMAKE_BINARY_DIR}/lib/libps5wk-objects.a)
add_custom_command(
    OUTPUT ${PS5_OBJECTS_ARCHIVE}
    COMMAND ${CMAKE_COMMAND} -E rm -f ${PS5_OBJECTS_ARCHIVE}
    COMMAND ${CMAKE_AR} qcsT ${PS5_OBJECTS_ARCHIVE} @${CMAKE_BINARY_DIR}/ps5-apps/objects.rsp
    DEPENDS ${PS5_OBJECT_LIBRARIES} ${_ps5_objects} ${CMAKE_BINARY_DIR}/ps5-apps/objects.rsp
    COMMENT "Archiving WebKit's object libraries for the PS5 executables"
    VERBATIM
)
add_custom_target(ps5wk-objects DEPENDS ${PS5_OBJECTS_ARCHIVE})
message(STATUS "PS5: ${CMAKE_BINARY_DIR}/lib/libps5wk-objects.a from: ${PS5_OBJECT_LIBRARIES}")

separate_arguments(PS5_DEPS_LINK UNIX_COMMAND "${PS5_DEPS_LIBS}")

# crashguard: fatal signals are reported and end in _exit(70), so a crashing
# payload never reaches the system crash reporter; --wrap=pthread_create
# gives every thread an alternate signal stack.
set(PS5_CRASHGUARD ${PS5_APPS_DIR}/common/crashguard.c)

# ps5locks: pthread mutexes, condition variables and rwlocks on futex words
# (the console's convoy at 4-20 us per contended hand-over, test/lockbench).
# Every pthread function that touches these objects must be in this list:
# check-lock-imports.sh fails the build if the executable imports another.
set(PS5_LOCKS ${PS5_APPS_DIR}/common/ps5locks.c)
set(PS5_LOCK_FUNCTIONS
    pthread_mutex_init pthread_mutex_destroy pthread_mutex_lock pthread_mutex_trylock pthread_mutex_timedlock pthread_mutex_unlock
    pthread_mutexattr_init pthread_mutexattr_destroy pthread_mutexattr_settype pthread_mutexattr_gettype
    pthread_cond_init pthread_cond_destroy pthread_cond_wait pthread_cond_timedwait pthread_cond_signal pthread_cond_broadcast
    pthread_condattr_init pthread_condattr_destroy pthread_condattr_setclock pthread_condattr_getclock
    pthread_rwlock_init pthread_rwlock_destroy pthread_rwlock_rdlock pthread_rwlock_tryrdlock pthread_rwlock_timedrdlock
    pthread_rwlock_wrlock pthread_rwlock_trywrlock pthread_rwlock_timedwrlock pthread_rwlock_unlock)
set(PS5_LOCK_WRAPS "")
foreach (_f IN LISTS PS5_LOCK_FUNCTIONS)
    list(APPEND PS5_LOCK_WRAPS "-Wl,--wrap=${_f}")
endforeach ()
# ps5malloc: malloc and its family on libpas in ps5wk-child (ps5malloc.c).
set(PS5_MALLOC_WRAPS -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc -Wl,--wrap=free -Wl,--wrap=posix_memalign
    -Wl,--wrap=aligned_alloc -Wl,--wrap=memalign -Wl,--wrap=malloc_usable_size -Wl,--wrap=strdup -Wl,--wrap=vasprintf)

add_executable(ps5wk-child ${PS5_APPS_DIR}/child/ps5wk-child.cpp ${PS5_CRASHGUARD} ${PS5_APPS_DIR}/common/profiler.c ${PS5_APPS_DIR}/common/ps5unwind.c
    ${PS5_APPS_DIR}/common/watchdog.c ${PS5_LOCKS} ${PS5_APPS_DIR}/common/ps5malloc.c ${PS5_APPS_DIR}/common/ps5clock.c ${PS5_APPS_DIR}/child/adblock-job.c)
target_include_directories(ps5wk-child PRIVATE ${PS5_WPE_DIR})
# adblock-job.c uses WebKit's public API (WebKitUserContentFilterStore, JSC).
set_source_files_properties(${PS5_APPS_DIR}/child/adblock-job.c PROPERTIES INCLUDE_DIRECTORIES "${PS5_API_INCLUDE_DIRECTORIES}")
# ps5clock: clock_gettime(MONOTONIC/REALTIME) from the TSC (ps5clock.c: the
# console's is a ~1.1 us system call).
target_link_options(ps5wk-child PRIVATE -Wl,--wrap=pthread_create -Wl,--wrap=close -Wl,--wrap=socket -Wl,--wrap=mmap -Wl,--wrap=munmap
    -Wl,--wrap=clock_gettime ${PS5_LOCK_WRAPS} ${PS5_MALLOC_WRAPS})
target_link_libraries(ps5wk-child PRIVATE WebKit ${PS5_OBJECTS_ARCHIVE} ${PS5_DEPS_LINK})
add_dependencies(ps5wk-child ps5wk-objects)
add_custom_command(TARGET ps5wk-child POST_BUILD
    COMMAND ${PS5_APPS_DIR}/common/check-lock-imports.sh $<TARGET_FILE:ps5wk-child> ${PS5_LOCK_FUNCTIONS}
    VERBATIM)

add_executable(ps5wk-shot ${PS5_APPS_DIR}/shot/ps5wk-shot.c ${PS5_CRASHGUARD} ${PS5_APPS_DIR}/common/ps5audioserver.c)
target_include_directories(ps5wk-shot PRIVATE ${PS5_API_INCLUDE_DIRECTORIES})
target_link_options(ps5wk-shot PRIVATE -Wl,--wrap=pthread_create -Wl,--wrap=close -Wl,--wrap=socket)
target_link_libraries(ps5wk-shot PRIVATE WebKit ${PS5_OBJECTS_ARCHIVE} ${PS5_DEPS_LINK} GLib::Gio)
add_dependencies(ps5wk-shot ps5wk-objects)
add_dependencies(ps5wk-shot WebKit)

set(PS5WK_LOG_HOST "$ENV{PS5WK_LOG_HOST}" CACHE STRING "Mac address for the title's TCP log (tools/logserver.sh; empty: none)")
set(PS5WK_LOG_PORT "9999" CACHE STRING "Port of tools/logserver.sh")
# The title gets ps5locks too: package.sh relinks these objects and copies
# the --wrap options from this link line (its heap, Spike 10's dlmalloc
# arenas, locks with pthread mutexes).
add_executable(ps5browser ${PS5_APPS_DIR}/title/ps5browser.c ${PS5_APPS_DIR}/title/settings.c ${PS5_APPS_DIR}/title/adblock.c ${PS5_APPS_DIR}/title/viewinvr.c ${PS5_APPS_DIR}/title/vrlayout.c ${PS5_APPS_DIR}/title/browserui.c ${PS5_APPS_DIR}/title/browserdraw.c ${PS5_APPS_DIR}/title/browserdata.c ${PS5_APPS_DIR}/title/gamepad.c ${PS5_APPS_DIR}/title/profile.c ${PS5_APPS_DIR}/title/runtime.c ${PS5_APPS_DIR}/title/downloads.c ${PS5_APPS_DIR}/title/qrcode.c ${PS5_APPS_DIR}/common/ps5audioserver.c
    ${PS5_APPS_DIR}/title/ps5locks-title.c ${PS5_APPS_DIR}/title/titlelog.c)
# The ad blocker's per-site cosmetic filters and scriptlets (ADBLOCK.md, phase 2).
target_sources(ps5browser PRIVATE ${PS5_APPS_DIR}/title/adblock-scripting.c)
target_include_directories(ps5browser PRIVATE ${PS5_API_INCLUDE_DIRECTORIES} ${PS5_PORT_DIR}/../../spikes)
target_compile_definitions(ps5browser PRIVATE LOG_HOST="${PS5WK_LOG_HOST}" LOG_PORT=${PS5WK_LOG_PORT})
target_link_options(ps5browser PRIVATE ${PS5_LOCK_WRAPS})
target_link_libraries(ps5browser PRIVATE WebKit ${PS5_OBJECTS_ARCHIVE} ${PS5_DEPS_LINK} GLib::Gio)
add_dependencies(ps5browser ps5wk-objects)
add_dependencies(ps5browser WebKit)
add_custom_command(TARGET ps5browser POST_BUILD
    COMMAND ${PS5_APPS_DIR}/common/check-lock-imports.sh $<TARGET_FILE:ps5browser> ${PS5_LOCK_FUNCTIONS}
    VERBATIM)

# lockbench (test/lockbench): the console's libc malloc and locks against
# ps5malloc (libpas, from WebKit's objects) and ps5locks. A payload.
add_executable(ps5wk-lockbench ${PS5_PORT_DIR}/test/lockbench/lockbench.c ${PS5_LOCKS} ${PS5_APPS_DIR}/common/ps5malloc.c)
target_link_options(ps5wk-lockbench PRIVATE -Wl,--wrap=mmap -Wl,--wrap=munmap ${PS5_LOCK_WRAPS} ${PS5_MALLOC_WRAPS})
target_link_libraries(ps5wk-lockbench PRIVATE ${PS5_OBJECTS_ARCHIVE} ${PS5_DEPS_LINK})
add_dependencies(ps5wk-lockbench ps5wk-objects)
