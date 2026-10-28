/* ps5compat: small libc additions for PS5 payloads built with
 * ps5-payload-sdk (libps5compat.a in the WPE sysroot). Each function lives
 * in its own object, so linking this archive next to another shim that also
 * defines one of them (e.g. WebKit's memmem) pulls only what is missing.
 *
 *   memmem             missing from the SDK libc
 *   getline, getdelim  missing from the SDK libc
 *   setfsent/getfsent/endfsent  empty fstab (GIO unix mounts)
 *   __wrap_readlink, __wrap_eaccess, __wrap_faccessat, __wrap_link,
 *   __wrap_symlink, __wrap_ppoll
 *                      fixes for libkernel wrappers that return errno values
 *                      as positive results (13.60); link with
 *                      -Wl,--wrap=readlink,--wrap=eaccess,--wrap=faccessat,
 *                      --wrap=link,--wrap=symlink,--wrap=ppoll
 *                      (ps5compat.pc and the glib/fontconfig/xkbcommon .pc
 *                      files carry these)
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
