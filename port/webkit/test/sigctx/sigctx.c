/*
 * sigctx.c - the layout of the ucontext a PS5 payload's SA_SIGINFO handler
 * receives (the crash guard printed sp 0 with FreeBSD's layout).
 * Raises abort() and a NULL write in functions whose sp and pc are known,
 * dumps the context words that match them, and recovers with siglongjmp.
 *   prospero-clang -O1 sigctx.c -o sigctx.elf && socat - TCP:$PS5_HOST:9021 < sigctx.elf
 */
#include <setjmp.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ucontext.h>
#include <unistd.h>

static sigjmp_buf recover;
static volatile uintptr_t expectSp, expectPcLo, expectPcHi;
static volatile int which;

static void handler(int sig, siginfo_t* info, void* context)
{
    const uintptr_t* w = (const uintptr_t*)context;
    printf("RESULT sigctx test %d: signal %d code %d addr %p; expect sp ~%p pc in [%p,%p)\n", which, sig, info->si_code, info->si_addr,
        (void*)expectSp, (void*)expectPcLo, (void*)expectPcHi);
    for (int i = 0; i < 160; i++) {
        uintptr_t v = w[i];
        const char* tag = "";
        if (v >= expectPcLo && v < expectPcHi)
            tag = "  <== pc";
        else if (v && v <= expectSp && expectSp - v < 4096)
            tag = "  <== sp?";
        if (*tag)
            printf("  ctx[%3d] (+0x%03x) = %p%s\n", i, i * 8, (void*)v, tag);
    }
    // PS5 layout found with this probe: FreeBSD's mcontext at +0x40, not +0x10.
    printf("  ps5 layout: rip %p rsp %p rbp %p (ctx +0xe0, +0xf8, +0x88)\n", (void*)w[0xe0 / 8], (void*)w[0xf8 / 8], (void*)w[0x88 / 8]);
    // FreeBSD's own idea, for comparison.
    const ucontext_t* uc = (const ucontext_t*)context;
    printf("  header: sizeof(ucontext_t) %zu, uc_mcontext at +0x%zx, mc_rip at +0x%zx (%p), mc_rsp at +0x%zx (%p)\n", sizeof(ucontext_t),
        offsetof(ucontext_t, uc_mcontext), offsetof(ucontext_t, uc_mcontext.mc_rip), (void*)uc->uc_mcontext.mc_rip,
        offsetof(ucontext_t, uc_mcontext.mc_rsp), (void*)uc->uc_mcontext.mc_rsp);
    fflush(stdout);
    siglongjmp(recover, 1);
}

__attribute__((noinline)) static void doAbort(void)
{
    volatile int local = 0;
    expectSp = (uintptr_t)&local;
    abort();
}

__attribute__((noinline)) static void doNullWrite(void)
{
    volatile int local = 0;
    expectSp = (uintptr_t)&local;
    *(volatile int*)(uintptr_t)local = 1;
}

__attribute__((noinline)) static void doRaise(void)
{
    volatile int local = 0;
    expectSp = (uintptr_t)&local;
    raise(SIGABRT);
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    struct sigaction action;
    memset(&action, 0, sizeof action);
    action.sa_sigaction = handler;
    action.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigaction(SIGABRT, &action, NULL);
    sigaction(SIGSEGV, &action, NULL);
    sigaction(SIGBUS, &action, NULL);

    // abort() lives in libc: accept any pc; the null write's pc is in doNullWrite.
    which = 1;
    expectPcLo = (uintptr_t)doNullWrite;
    expectPcHi = (uintptr_t)doNullWrite + 64;
    if (!sigsetjmp(recover, 1))
        doNullWrite();
    which = 2;
    expectPcLo = 0x200000000ull;
    expectPcHi = 0x300000000ull;
    if (!sigsetjmp(recover, 1))
        doRaise();
    which = 3;
    if (!sigsetjmp(recover, 1))
        doAbort();
    printf("RESULT sigctx done\n");
    _exit(0);
}
