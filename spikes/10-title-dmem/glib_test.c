/* glib_test.c - does glib's g_malloc (static libglib-2.0.a from
 * toolchain/wpe-sysroot) land in the direct-memory heap? glib calls malloc
 * directly, and the title's --wrap reaches into static archives. */
#include <glib.h>
#include <stdio.h>

#include "dmem.h"

/* glib marks intentional leaks for LeakSanitizer through a weak reference the
 * title converter refuses to leave unresolved. */
void __lsan_ignore_object(const void *p);
void __lsan_ignore_object(const void *p)
{
    (void)p;
}

int glib_test(char *out, size_t n)
{
    int owned = 0, total = 0;
#define IN_HEAP(p) (total++, owned += dheap_owns(p))
    gpointer a = g_malloc(1000);
    IN_HEAP(a);
    gpointer b = g_malloc0(1 << 20);
    IN_HEAP(b);
    a = g_realloc(a, 100000);
    IN_HEAP(a);
    gchar *s = g_strdup_printf("glib %d.%d.%d", glib_major_version, glib_minor_version,
                               glib_micro_version);
    IN_HEAP(s);
    GHashTable *h = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    for (int i = 0; i < 1000; i++)
        g_hash_table_insert(h, g_strdup_printf("k%d", i), GINT_TO_POINTER(i));
    IN_HEAP(h);
    GString *gs = g_string_new(NULL);
    for (int i = 0; i < 1000; i++)
        g_string_append(gs, "0123456789");
    IN_HEAP(gs->str);
    const int ok = g_hash_table_size(h) == 1000 &&
                   GPOINTER_TO_INT(g_hash_table_lookup(h, "k500")) == 500 && gs->len == 10000;
    snprintf(out, n, "%d of %d glib allocations in the direct heap (%s: g_malloc, g_malloc0 1 MB, "
             "g_realloc, g_strdup_printf, GHashTable, GString); values %s",
             owned, total, s, ok ? "correct" : "WRONG");
    g_string_free(gs, TRUE);
    g_hash_table_destroy(h);
    g_free(s);
    g_free(b);
    g_free(a);
    return owned == total && ok ? 0 : 1;
}
