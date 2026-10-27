/* PS5 payload SDK shims: libc functions JSC/WTF/ICU reference that the
 * ps5-payload-sdk libc / libkernel stubs do not provide. Keep this tiny;
 * each entry is listed in NOTES.md. */
#include <stddef.h>
#include <string.h>

/* (filled in as link errors show up) */
int ps5shim_present = 1;

/* memmem(3): GNU/BSD extension, missing from the SDK libc and libSceLibcInternal
 * stubs. Used by WTF/JSC string search. Simple O(n*m) version. */
void* memmem(const void* haystack, size_t haystackLen, const void* needle, size_t needleLen)
{
    const unsigned char* h = (const unsigned char*)haystack;
    const unsigned char* n = (const unsigned char*)needle;
    if (!needleLen)
        return (void*)h;
    if (haystackLen < needleLen)
        return NULL;
    for (size_t i = 0; i + needleLen <= haystackLen; i++) {
        if (h[i] == n[0] && !memcmp(h + i, n, needleLen))
            return (void*)(h + i);
    }
    return NULL;
}
