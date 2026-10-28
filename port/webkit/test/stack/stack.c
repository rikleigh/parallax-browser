/*
 * stack.c - default pthread stack size in a payload (WebKit's WTF threads
 * use the platform default unless the thread type asks for more).
 *   prospero-clang -O2 stack.c -o stack.elf -lpthread && socat -u FILE:stack.elf TCP:$PS5_HOST:9021
 * (output on the elfldr connection: socat - TCP:... < stack.elf)
 */
#include <pthread.h>
#include <pthread_np.h>
#include <stdint.h>
#include <stdio.h>

static void* probe(void* data)
{
    (void)data;
    pthread_attr_t a;
    void* base = NULL;
    size_t size = 0, guard = 0;
    if (pthread_attr_init(&a) == 0 && pthread_attr_get_np(pthread_self(), &a) == 0) {
        pthread_attr_getstack(&a, &base, &size);
        pthread_attr_getguardsize(&a, &guard);
    }
    int local;
    printf("RESULT stack thread: base %p size %zu KiB guard %zu, local at %p (%ld bytes above base)\n", base, size >> 10, guard,
        (void*)&local, (long)((uintptr_t)&local - (uintptr_t)base));
    return NULL;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    pthread_attr_t a;
    size_t size = 0;
    pthread_attr_init(&a);
    pthread_attr_getstacksize(&a, &size);
    printf("RESULT stack default attr: %zu KiB\n", size >> 10);
    pthread_t t;
    if (pthread_create(&t, NULL, probe, NULL) == 0)
        pthread_join(t, NULL);
    pthread_attr_setstacksize(&a, 1 << 20);
    if (pthread_create(&t, &a, probe, NULL) == 0)
        pthread_join(t, NULL);
    return 0;
}
