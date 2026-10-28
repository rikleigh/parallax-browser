/* memmem(3): missing from the SDK's libc and libSceLibcInternal. */
#include <string.h>

void *memmem(const void *h, size_t hl, const void *n, size_t nl)
{
    const unsigned char *hp = h, *np = n;
    if (nl == 0)
        return (void *)h;
    if (hl < nl)
        return NULL;
    for (size_t i = 0; i + nl <= hl; i++)
        if (hp[i] == np[0] && memcmp(hp + i, np, nl) == 0)
            return (void *)(hp + i);
    return NULL;
}
