/*
 * crashguard: fatal-signal handlers for the WebKit payloads (ps5wk-child,
 * ps5wk-shot). A payload that dies from a signal is crash-reported by the
 * system (a core of a ~600 MB process), which is the prime suspect for two
 * console shutdowns. The handlers print one report to stderr (the elfldr
 * connection, relayed to the title log) and _exit(70) instead.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Installs handlers for SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP and
 * SIGSYS, and an alternate signal stack for the calling thread. tag goes into
 * the report ("web4", "network1", "shot"). Call before WebKit starts (WTF
 * chains to handlers installed earlier). */
void ps5wk_crash_guard_install(const char* tag);

/* An alternate signal stack for the calling thread (sigaltstack is
 * per-thread), so a stack overflow can still be reported. Every thread made
 * through pthread_create gets one when the executable links with
 * -Wl,--wrap=pthread_create (CMakeLists.txt). */
void ps5wk_crash_guard_thread(void);

#ifdef __cplusplus
}
#endif
