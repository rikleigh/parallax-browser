#pragma once
// Sampling profiler for the calling thread, on when PS5WK_PROFILE=1 (see profiler.c).
#ifdef __cplusplus
extern "C" {
#endif
void ps5wk_profiler_start(const char* tag);
#ifdef __cplusplus
}
#endif
