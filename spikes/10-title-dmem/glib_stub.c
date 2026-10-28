/* glib_stub.c - linked instead of glib_test.c when glib isn't (see build-title.sh). */
#include <stddef.h>
#include <stdio.h>

int glib_test(char *out, size_t n)
{
    snprintf(out, n, "glib not linked into this build");
    return -1;
}
