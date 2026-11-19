/*
 * radv_title.c - what RADV and ANGLE need from a title that links RADV and no
 * ps5-opengl (spike 15). Before, ps5-opengl's ps5_mesa_shims.c supplied
 * open_memstream; the rest is as port/webkit/apps/title/titleradv.c.
 */
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* compiler-rt's __cpu_model, which __builtin_cpu_supports() reads: addrlib's
 * swizzler asks for "avx2", and nothing else in RADV asks for anything. The
 * title links no compiler-rt builtins. The console's CPU is always a Zen 2,
 * which has AVX2 (bit 10 of the first feature word, FEATURE_AVX2). */
struct {
    unsigned vendor, type, subtype, features[1];
} __cpu_model = { 2 /* VENDOR_AMD */, 0, 0, { 1u << 10 } };
unsigned __cpu_features2[3];

/* The SDK's libc declares open_memstream but has none (nor funopen,
 * fopencookie or fmemopen). Mesa uses memory streams to build diagnostic
 * text (ACO's and NIR's printers); none of that is on a normal path. */
FILE *open_memstream(char **bufp, size_t *sizep)
{
    if (bufp)
        *bufp = NULL;
    if (sizep)
        *sizep = 0;
    errno = ENOSYS;
    return NULL;
}

/* Mesa's on-disk shader cache sizes its index file with this. libc.a's is a
 * raw syscall, which a title may not make; failing makes the cache fall back
 * to its in-memory form. */
int posix_fallocate(int fd, off_t offset, off_t len)
{
    (void)fd;
    (void)offset;
    (void)len;
    return ENOSYS;
}

/* The title links libc.a without syscalls.o (raw syscalls are illegal in a
 * title), and libkernel's stub doesn't export these two; Mesa's disk cache
 * calls them. As port/webkit/apps/title/titlesys.c: AT_FDCWD or absolute
 * paths only, and /data has no links. */
int fstatat(int fd, const char *path, struct stat *st, int flag)
{
    if (fd != AT_FDCWD && !(path && path[0] == '/')) {
        errno = ENOSYS;
        return -1;
    }
    return (flag & AT_SYMLINK_NOFOLLOW) ? lstat(path, st) : stat(path, st);
}

ssize_t readlink(const char *path, char *buf, size_t size)
{
    (void)path;
    (void)buf;
    (void)size;
    errno = EINVAL;
    return -1;
}

/* libc.a's dlfcn.o forwards to libkernel's __dladdr and __dlclose, which a
 * title cannot import, and the title has no shared objects to load anyway.
 * ANGLE calls these for its module directory and, in its GPU info code, to
 * look for a libvulkan (the renderer takes RADV through the hook below). All
 * four are defined here so dlfcn.o stays out of the link. */
typedef struct dl_info Dl_info;
void *dlopen(const char *path, int mode)
{
    (void)path;
    (void)mode;
    return NULL;
}
void *dlsym(void *handle, const char *name)
{
    (void)handle;
    (void)name;
    return NULL;
}
int dlclose(void *handle)
{
    (void)handle;
    return 0;
}
int dladdr(const void *addr, Dl_info *info)
{
    (void)addr;
    (void)info;
    return 0;
}

/* ANGLE's Vulkan loader hook (patches/0001): the driver's vkGetInstanceProcAddr. */
void *vk_icdGetInstanceProcAddr(void *instance, const char *name);
void *angle_ps5_vk_get_instance_proc_addr(void)
{
    return (void *)&vk_icdGetInstanceProcAddr;
}

/* Thread stacks: package-title.sh links with --wrap=pthread_create, and the
 * crash guard's __wrap_pthread_create (port/webkit/apps/common/crashguard.c,
 * linked though the guard isn't installed) gives every thread that asks for
 * less at least 2 MiB. A title's default is 64 KiB, and ANGLE's worker
 * threads (std::thread, no size asked) compile pipelines in RADV, whose
 * nir_opt_varyings alone has a frame of about 72 KB: the first runs died
 * writing below the stack (SIGSEGV, "user write data, page not present").
 * These are libc stacks (flexible memory); the browser's titlemem.c makes
 * them in direct memory. */
