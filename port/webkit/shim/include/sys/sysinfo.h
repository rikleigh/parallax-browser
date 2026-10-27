/* PS5 payload SDK shim: Linux/FreeBSD-14-style sysinfo(2) on top of sysctl.
 * WTF (RAMSize.cpp, AvailableMemory.cpp) includes <sys/sysinfo.h> for OS(FREEBSD).
 * Only totalram/mem_unit are meaningful here. */
#pragma once
#include <string.h>
#include <sys/types.h>
#include <sys/sysctl.h>

struct sysinfo {
    long uptime;
    unsigned long loads[3];
    unsigned long totalram;
    unsigned long freeram;
    unsigned long sharedram;
    unsigned long bufferram;
    unsigned long totalswap;
    unsigned long freeswap;
    unsigned short procs;
    unsigned long totalhigh;
    unsigned long freehigh;
    unsigned int mem_unit;
};

static inline int sysinfo(struct sysinfo* info)
{
    unsigned long physmem = 0;
    size_t len = sizeof(physmem);
    memset(info, 0, sizeof(*info));
    info->mem_unit = 1;
    if (sysctlbyname("hw.physmem", &physmem, &len, 0, 0) || !physmem)
        physmem = 4ul << 30; /* fallback guess: 4 GiB budget */
    info->totalram = physmem;
    return 0;
}
