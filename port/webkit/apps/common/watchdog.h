#pragma once
// Main-thread watchdog for ps5wk-child (watchdog.c): logs where WebKit's main
// thread is when its run loop has not turned for PS5WK_WATCHDOG_S seconds
// (default 3, 0 off). Start it on the main thread before the run loop runs.
#ifdef __cplusplus
extern "C" {
#endif
void ps5wk_watchdog_start(const char* tag);
// The run loop has ended (process shutdown): no more reports.
void ps5wk_watchdog_stop(void);
#ifdef __cplusplus
}
#endif
