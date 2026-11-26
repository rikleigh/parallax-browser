/*
 * crashguard: fatal-signal handlers for the WebKit payloads (ps5wk-child,
 * ps5wk-shot). A payload that dies from a signal is crash-reported by the
 * system (a core of a ~600 MB process), which is the prime suspect for two
 * console shutdowns. The handlers print one report to stderr (the elfldr
 * connection, relayed to the title log) and _exit(70) instead. In the
 * browser title (titlecrash.c) the report also goes to its log and the
 * title then SIGKILLs itself, watched by a thread that does it anyway if the
 * handler hangs (crashguard.c).
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

/* The report also goes to this file (appended), written from the handler:
 * the browser title's stderr is a pipe drained by a log thread that dies
 * with it. Call before ps5wk_crash_guard_install. */
void ps5wk_crash_guard_report_file(const char* path);

/* An alternate signal stack for the calling thread (sigaltstack is
 * per-thread), so a stack overflow can still be reported. Every thread made
 * through pthread_create gets one when the executable links with
 * -Wl,--wrap=pthread_create (CMakeLists.txt). That wrapper also raises
 * threads below 2 MiB of stack (the payload libc's default is 64 KiB) to
 * 2 MiB. Built with PS5WK_CRASHGUARD_NO_THREAD_WRAP (the browser title,
 * whose titlemem.c wraps pthread_create) there is no wrapper: only the
 * thread that installed the guard has an alternate stack. */
void ps5wk_crash_guard_thread(void);

#ifdef __cplusplus
}
#endif
