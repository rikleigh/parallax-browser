/* PS5 payload SDK shims: libc functions JSC/WTF/ICU reference that the
 * ps5-payload-sdk libc / libkernel stubs do not provide. Keep this tiny;
 * each entry is listed in NOTES.md. */
#include <stddef.h>
#include <string.h>

/* (filled in as link errors show up) */
int ps5shim_present = 1;

/* memmem(3) came from here in Spike 4; libps5compat (port/deps) provides it now. */

/* fcntl(F_DUPFD_CLOEXEC) is broken in the PS5's libkernel: it returns 0 with
 * errno 0 (fd 0, the payload's stdin socket) instead of a new descriptor
 * (port/webkit/test/seqpacket/dupfd.c, 2026-10-28). WTF::dupCloseOnExec and
 * GLib use it, so WebKit sent fd 0 in place of shared memory. Linked with
 * -Wl,--wrap=fcntl: emulate it with F_DUPFD + FD_CLOEXEC. */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>

int __real_fcntl(int fd, int cmd, ...);
int __wrap_fcntl(int fd, int cmd, ...)
{
    va_list ap;
    va_start(ap, cmd);
    intptr_t arg = va_arg(ap, intptr_t);
    va_end(ap);
    if (cmd == F_DUPFD_CLOEXEC) {
        int r = __real_fcntl(fd, F_DUPFD, arg);
        if (r >= 0)
            __real_fcntl(r, F_SETFD, FD_CLOEXEC);
        return r;
    }
    return __real_fcntl(fd, cmd, arg);
}
