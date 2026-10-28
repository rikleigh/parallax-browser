/* The SDK declares the BIND resolver API (resolv.h) but no library exports
 * it. GLib's GResolver needs res_query/dn_expand to link; only SRV/MX/TXT
 * record lookups use them (g_resolver_lookup_records), which WebKit does not.
 * These stubs fail with HOST_NOT_FOUND. Host name lookups use getaddrinfo,
 * which libSceNet/libkernel provide. */
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/nameser.h>
#include <netdb.h>
#include <resolv.h>
#include <errno.h>

int res_init(void) { return 0; }

int res_query(const char *name, int class, int type, u_char *answer, int anslen)
{
    (void)name; (void)class; (void)type; (void)answer; (void)anslen;
    h_errno = HOST_NOT_FOUND;
    errno = ENOENT;
    return -1;
}

int res_search(const char *name, int class, int type, u_char *answer, int anslen)
{
    return res_query(name, class, type, answer, anslen);
}

int res_ninit(res_state st) { (void)st; return 0; }
void res_nclose(res_state st) { (void)st; }

int res_nquery(res_state st, const char *name, int class, int type, u_char *answer, int anslen)
{
    (void)st;
    return res_query(name, class, type, answer, anslen);
}

int res_nsearch(res_state st, const char *name, int class, int type, u_char *answer, int anslen)
{
    (void)st;
    return res_query(name, class, type, answer, anslen);
}

int dn_expand(const u_char *msg, const u_char *eom, const u_char *src, char *dst, int dstsiz)
{
    (void)msg; (void)eom; (void)src; (void)dst; (void)dstsiz;
    errno = EMSGSIZE;
    return -1;
}

int dn_skipname(const u_char *ptr, const u_char *eom)
{
    (void)ptr; (void)eom;
    return -1;
}
