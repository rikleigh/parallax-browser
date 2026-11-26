#pragma once
// The disk write test (writetest.c): development builds only
// (PS5WK_DEBUG_BUILD). The title runs it for POST /debug/write-test
// (apps/title/ps5browser.c), ps5wk-child as the task payload "writetest", so
// the same writes can be timed from the title process and from a payload.
#ifdef PS5WK_DEBUG_BUILD
#ifdef __cplusplus
extern "C" {
#endif

typedef void (*PS5WKWriteTestSay)(const char* line, void* data);

// The keys of a test's description, NULL at the end: what the title passes
// on from a request (nothing else reaches a payload's launch header).
extern const char* const ps5wk_write_test_keys[];

// One test on the calling thread, described by "key=value&key=value" (values
// percent-encoded; writetest.c lists the keys). Every line goes to say,
// without a newline. 0: every pass wrote all of its bytes.
int ps5wk_write_test(const char* spec, const char* process, PS5WKWriteTestSay say, void* data);

// One small write as the title's own saves do it (4 KB to a temporary file,
// fsync, close, rename) in the test's folder: the seconds it took, negative
// if it failed. For timing what else a process's writes cost while a test
// runs on another thread.
double ps5wk_write_test_probe(const char* spec);

#ifdef __cplusplus
}
#endif
#endif
