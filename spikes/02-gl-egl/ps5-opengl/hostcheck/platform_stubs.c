/* Host (macOS x86_64 under Rosetta) stand-ins for the PS5 platform symbols the
 * ps5 Gallium driver references. Only what screen/context/resource creation
 * touches does real work: direct memory is anonymous mmap. Everything else is
 * generated into gen_stubs.c and aborts loudly if reached. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

static int64_t next_direct = 0x100000;

int64_t sceKernelGetDirectMemorySize(void) { return INT64_C(16) << 30; }
int64_t sceKernelAvailableDirectMemorySize(int64_t a, int64_t b, size_t al,
                                           int64_t *start, size_t *size)
{
   (void)a; (void)b; (void)al;
   if (start) *start = 0;
   if (size) *size = (size_t)12 << 30;
   return 0;
}

int32_t sceKernelAllocateDirectMemory(int64_t search_start, int64_t search_end,
                                     size_t length, size_t alignment,
                                     int memory_type, int64_t *physical_start)
{
   (void)search_start; (void)search_end; (void)memory_type;
   if (!alignment) alignment = 0x10000;
   next_direct = (next_direct + (int64_t)alignment - 1) & ~((int64_t)alignment - 1);
   *physical_start = next_direct;
   next_direct += (int64_t)length;
   return 0;
}

int32_t sceKernelMapDirectMemory(void **address, size_t length, int protection,
                                int flags, int64_t physical_start, size_t alignment)
{
   (void)protection; (void)flags; (void)physical_start;
   if (alignment < 0x4000) alignment = 0x4000;
   size_t span = length + alignment;
   uint8_t *raw = mmap(NULL, span, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANON, -1, 0);
   if (raw == MAP_FAILED) return -1;
   uintptr_t aligned = ((uintptr_t)raw + alignment - 1) & ~(uintptr_t)(alignment - 1);
   *address = (void *)aligned;
   return 0;
}

int32_t sceKernelReleaseDirectMemory(int64_t physical_start, size_t length)
{
   (void)physical_start; (void)length;
   return 0;
}

int sceSystemServiceHideSplashScreen(void) { return 0; }
int __isthreaded = 1;
