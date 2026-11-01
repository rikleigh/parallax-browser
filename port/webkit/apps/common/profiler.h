#pragma once
// Sampling profiler (PS5WK_PROFILE, see profiler.c). Start it on the thread
// that runs WebKit's main loop; the crash guard's thread trampoline calls
// ps5wk_profiler_thread(1) at the start of every thread and (0) at its end.
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
void ps5wk_profiler_start(const char* tag);
void ps5wk_profiler_thread(int starting);

// The profiler's unwinder and thread table for others (watchdog.c); these
// work whether PS5WK_PROFILE is on or not.
#define PS5WK_BACKTRACE_MAX 128
// Reads the executable's unwind tables (once; 0 if CFI unwinding works).
int ps5wk_backtrace_init(void);
// In a signal handler: the interrupted frame (ucontext) and its callers,
// innermost first, up to PS5WK_BACKTRACE_MAX pcs. Async-signal-safe.
unsigned ps5wk_backtrace_context(const void* ucontext, uintptr_t* out);
// Re-reads JSC's executable memory range (JIT pcs are named "j<tier>").
void ps5wk_backtrace_refresh_jit(void);
// " <hex offset in the ELF>" per pc, "j<tier>" for JIT code, "x<hex>" for
// other modules: the profiler's notation. Returns the length written.
size_t ps5wk_backtrace_format(char* buffer, size_t size, const uintptr_t* pcs, unsigned n);
// Live threads that can be signalled (pthread_t values and table indices),
// leaving out the profiler's and the watchdog's own.
unsigned ps5wk_profiler_threads(uintptr_t* keys, unsigned* indices, unsigned max);
int ps5wk_profiler_thread_index(uintptr_t key);
// The calling thread is a helper: never signalled, never listed.
void ps5wk_profiler_own_thread(void);
// The calling thread in the table with its stack bounds (the trampoline
// does this for threads made with pthread_create).
void ps5wk_profiler_register_self(void);
#ifdef __cplusplus
}
#endif
