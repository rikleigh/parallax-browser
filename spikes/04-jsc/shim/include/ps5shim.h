/* Force-included (-include ps5shim.h) into every C/C++ TU of the PS5 build.
 * Fills gaps between the ps5-payload-sdk headers (an older FreeBSD userland)
 * and what WTF/JSC/libpas expect from a current FreeBSD. Declarations only;
 * definitions (if any) live in ps5shim.c. */
#pragma once
#include <sys/types.h>   /* old <sys/sysctl.h> uses u_int without including it */
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Exported by libkernel (stub present in libkernel_web) but not declared in
 * the SDK's pthread headers. Only used for debug logging in libpas. */
int pthread_getname_np(pthread_t, char*, size_t);

#ifdef __cplusplus
}
#endif
