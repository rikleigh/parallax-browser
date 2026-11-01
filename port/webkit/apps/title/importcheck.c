/*
 * importcheck.c - which of the title's imports the console left NULL.
 *
 * The loader leaves an import it can't resolve as NULL; calling it is a
 * SIGSEGV at address 0 (readlink, 2026-10-28). package.sh lists every
 * function the title imports (imports.inc, from the first link) and links
 * this table; ps5browser logs the NULL ones at start.
 */
#include <stddef.h> /* nothing else: the declarations below would clash */

#define IMPORT(name) extern void name(void);
#include "imports.inc"
#undef IMPORT

static const struct {
    const char* name;
    void (*address)(void);
} imports[] = {
#define IMPORT(name) { #name, name },
#include "imports.inc"
#undef IMPORT
};

/* Writes the NULL imports' names into out; returns how many there are. */
unsigned ps5wk_title_check_imports(char* out, size_t size, unsigned* total)
{
    unsigned missing = 0;
    size_t n = 0;
    if (size)
        out[0] = 0;
    for (size_t i = 0; i < sizeof imports / sizeof imports[0]; i++) {
        if (imports[i].address)
            continue;
        missing++;
        if (n && n + 1 < size)
            out[n++] = ' ';
        for (const char* c = imports[i].name; *c && n + 1 < size; c++)
            out[n++] = *c;
        if (size)
            out[n < size ? n : size - 1] = 0;
    }
    *total = (unsigned)(sizeof imports / sizeof imports[0]);
    return missing;
}

/* The package's build id (package.sh): logged at start, names the folder
 * with this package's unstripped binaries (titles/debug/<id>/). */
#ifndef PS5WK_TITLE_BUILD_ID
#define PS5WK_TITLE_BUILD_ID "unknown"
#endif
const char* ps5wk_title_build_id(void)
{
    return PS5WK_TITLE_BUILD_ID;
}
