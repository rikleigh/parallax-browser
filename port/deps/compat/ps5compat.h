/* ps5compat: small libc additions for PS5 payloads built with
 * ps5-payload-sdk (libps5compat.a in the WPE sysroot). Each function lives
 * in its own object, so linking this archive next to another shim that also
 * defines one of them (e.g. WebKit's memmem) pulls only what is missing.
 *
 *   memmem             missing from the SDK libc
 *   getline, getdelim  missing from the SDK libc
 *   setfsent/getfsent/endfsent  empty fstab (GIO unix mounts)
 *   __wrap_readlink    readlink fix (libkernel returns errno values); link
 *                      with -Wl,--wrap=readlink (ps5compat.pc does)
 *   res_* / dn_*       resolver stubs (declared by resolv.h, exported by
 *                      nothing); they fail with HOST_NOT_FOUND
 */
#ifndef PS5COMPAT_H
#define PS5COMPAT_H
#include <stddef.h>
#include <stdio.h>
#include <sys/types.h>
void *memmem(const void *, size_t, const void *, size_t);
ssize_t getdelim(char **, size_t *, int, FILE *);
ssize_t getline(char **, size_t *, FILE *);
#endif
