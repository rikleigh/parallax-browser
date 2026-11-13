/*
 * titleradv.c - what RADV (the Vulkan driver under WebGPU; spikes/12-webgpu)
 * needs from the title beyond the SDK's libraries. Linked in WebGPU builds
 * only (package.sh).
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/types.h>

/* compiler-rt's __cpu_model, which __builtin_cpu_supports() reads: addrlib's
 * swizzler asks for "avx2", and nothing else in RADV asks for anything. The
 * title links no compiler-rt builtins. The console's CPU is always a Zen 2,
 * which has AVX2 (bit 10 of the first feature word, FEATURE_AVX2). */
struct {
    unsigned vendor, type, subtype, features[1];
} __cpu_model = { 2 /* VENDOR_AMD */, 0, 0, { 1u << 10 } };
unsigned __cpu_features2[3];

/* Mesa's on-disk shader cache sizes its index file with this. libc.a's is a
 * raw syscall (not allowed in a title, and package.sh takes syscalls.o out);
 * failing makes the cache fall back to its in-memory form. */
int posix_fallocate(int fd, off_t offset, off_t len)
{
    (void)fd;
    (void)offset;
    (void)len;
    return ENOSYS;
}

/* One lock for GPU submissions, in the order the GPU will run them. The title
 * has two drivers on one GPU: ps5-opengl (the compositor, on the render
 * thread) and RADV (WebGPU, on Dawn's thread). Each sets the GPU's state in
 * one submission and relies on it in the next ones of the same frame or
 * queue submit; a submission of the other driver in between ran those with
 * the wrong state: layers drawn without blending (black where they are
 * transparent) and broken geometry in the WebGPU canvas. Dawn holds this
 * around a vkQueueSubmit (port/deps/patches/dawn), the render thread around
 * the GL work of a frame (PS5Renderer.cpp), not across its waits.
 * Recursive; unlock reports whether the caller held it. */
static pthread_mutex_t gpu_submit_mutex;
static pthread_once_t gpu_submit_once = PTHREAD_ONCE_INIT;
static __thread int gpu_submit_depth;

static void gpu_submit_init(void)
{
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&gpu_submit_mutex, &attr);
    pthread_mutexattr_destroy(&attr);
}

void ps5wk_gpu_submit_lock(void)
{
    pthread_once(&gpu_submit_once, gpu_submit_init);
    pthread_mutex_lock(&gpu_submit_mutex);
    gpu_submit_depth++;
}

int ps5wk_gpu_submit_unlock(void)
{
    if (!gpu_submit_depth)
        return 0;
    gpu_submit_depth--;
    pthread_mutex_unlock(&gpu_submit_mutex);
    return 1;
}
