#pragma once
// Sampling profiler (PS5WK_PROFILE, see profiler.c). Start it on the thread
// that runs WebKit's main loop; the crash guard's thread trampoline calls
// ps5wk_profiler_thread(1) at the start of every thread and (0) at its end.
#ifdef __cplusplus
extern "C" {
#endif
void ps5wk_profiler_start(const char* tag);
void ps5wk_profiler_thread(int starting);
#ifdef __cplusplus
}
#endif
