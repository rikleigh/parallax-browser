/*
 * titleabort.c - say why the title aborts (--wrap=abort, title link only).
 *
 * WebKit's RELEASE_ASSERT/CRASH() is abort() here, and so are libc++
 * hardening failures (after libc++ prints its message to stderr, which is
 * the log socket) and g_error() (after logHandler logged it). The kernel's
 * crash report names only "abort is called" and a frame-pointer backtrace
 * of load addresses. This logs one line with the thread and the backtrace
 * before abort() runs, to the title log (TCP and /data/ps5webkit/<title>.log)
 * and to the kernel log:
 *
 *   [abort] abort() called on thread 0x... (main): backtrace 0x... 0x... (self 0x...)
 *
 * (titlelog.c's queue when it runs, flushed for up to 1 s; never a blocking
 * send.)
 *
 * Addresses are load addresses like the kernel's: symbolize with
 * llvm-addr2line -f -C -i -e titles/debug/<build id>/ps5browser.elf 0x<addr - 0x400000>
 * ("self" is __wrap_abort, to check the base). No stdio and no malloc: the
 * caller may hold either lock.
 */
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

extern int spike_log_fd __attribute__((weak));
extern int spike_log_file_fd __attribute__((weak));
/* titlelog.c's queue (spike.h): the line goes through it when it runs. */
extern void (*spike_log_sink)(const char* line, size_t n) __attribute__((weak));
extern int (*spike_log_flush_hook)(int timeout_ms) __attribute__((weak));
int sceKernelDebugOutText(int channel, const char* text) __attribute__((weak));
void __real_abort(void) __attribute__((noreturn));

static pthread_t mainThread;
__attribute__((constructor)) static void rememberMainThread(void)
{
    mainThread = pthread_self();
}

static void emit(const char* line, size_t n)
{
    /* The queue, and 1 s for it to reach the file and the log server;
     * otherwise straight to the file. Never a send that can block. */
    int queued = &spike_log_sink && spike_log_sink && &spike_log_flush_hook && spike_log_flush_hook;
    if (queued) {
        spike_log_sink(line, n);
        queued = spike_log_flush_hook(1000);
    }
    if (!queued && &spike_log_fd && spike_log_fd >= 0)
        (void)!send(spike_log_fd, line, n, MSG_DONTWAIT);
    if (!queued && &spike_log_file_fd && spike_log_file_fd >= 0)
        (void)!write(spike_log_file_fd, line, n);
    if (sceKernelDebugOutText)
        sceKernelDebugOutText(0, line);
}

void __wrap_abort(void) __attribute__((noreturn, noinline));
void __wrap_abort(void)
{
    static int entered;
    if (__atomic_exchange_n(&entered, 1, __ATOMIC_SEQ_CST))
        __real_abort();
    char line[1024];
    int n = snprintf(line, sizeof line, "[abort] abort() called on thread %p%s: backtrace",
        (void*)pthread_self(), pthread_equal(pthread_self(), mainThread) ? " (main)" : "");
    /* Frame pointers (the kernel's own backtrace walks them too): each
     * frame's saved rbp is above the previous one and within 8 MiB. */
    uintptr_t* fp = (uintptr_t*)__builtin_frame_address(0);
    for (int i = 0; i < 40 && fp && n < (int)sizeof line - 24; i++) {
        uintptr_t ret = fp[1];
        if (!ret)
            break;
        n += snprintf(line + n, sizeof line - (size_t)n, " %#lx", (unsigned long)ret);
        uintptr_t* next = (uintptr_t*)fp[0];
        if (next <= fp || (uintptr_t)next - (uintptr_t)fp > (8u << 20) || ((uintptr_t)next & 7))
            break;
        fp = next;
    }
    if (n < (int)sizeof line - 32)
        n += snprintf(line + n, sizeof line - (size_t)n, " (self %p)\n", (void*)__wrap_abort);
    else
        line[(n = (int)sizeof line - 1) - 1] = '\n';
    emit(line, (size_t)n);
    __real_abort();
}
