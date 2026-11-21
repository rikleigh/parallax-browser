/*
 * minijson.c: see minijson.h.
 */
#include "minijson.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MJ_DEPTH 64

struct MJ {
    MJKind kind;
    gboolean truth;
    double number;
    char* text;      /* MJ_STRING */
    GPtrArray* items; /* MJ_ARRAY: MJ*; MJ_OBJECT: MJ* values */
    GPtrArray* keys;  /* MJ_OBJECT: char*, as items */
};

typedef struct {
    const char* p;
    int depth;
} Reader;

static void skipSpace(Reader* r)
{
    while (*r->p == ' ' || *r->p == '\t' || *r->p == '\n' || *r->p == '\r')
        r->p++;
}

void mj_free(MJ* value)
{
    if (!value)
        return;
    g_free(value->text);
    if (value->items)
        g_ptr_array_unref(value->items);
    if (value->keys)
        g_ptr_array_unref(value->keys);
    g_free(value);
}

static int hex4(const char* p)
{
    int v = 0;
    for (int i = 0; i < 4; i++) {
        if (!g_ascii_isxdigit(p[i]))
            return -1;
        v = v * 16 + g_ascii_xdigit_value(p[i]);
    }
    return v;
}

/* At the opening quote; the string's text (g_free) or NULL. */
static char* readString(Reader* r)
{
    if (*r->p != '"')
        return NULL;
    r->p++;
    GString* out = g_string_new(NULL);
    while (*r->p && *r->p != '"') {
        unsigned char c = (unsigned char)*r->p;
        if (c < 0x20)
            break;
        if (c != '\\') {
            g_string_append_c(out, (char)c);
            r->p++;
            continue;
        }
        r->p++;
        char e = *r->p;
        if (!e)
            goto bad; /* the text ends after the backslash: stay on its NUL */
        r->p++;
        switch (e) {
        case '"':
        case '\\':
        case '/':
            g_string_append_c(out, e);
            break;
        case 'b':
            g_string_append_c(out, '\b');
            break;
        case 'f':
            g_string_append_c(out, '\f');
            break;
        case 'n':
            g_string_append_c(out, '\n');
            break;
        case 'r':
            g_string_append_c(out, '\r');
            break;
        case 't':
            g_string_append_c(out, '\t');
            break;
        case 'u': {
            int u = hex4(r->p);
            if (u < 0)
                goto bad;
            r->p += 4;
            gunichar ch = (gunichar)u;
            if (u >= 0xd800 && u < 0xdc00) {
                int low = r->p[0] == '\\' && r->p[1] == 'u' ? hex4(r->p + 2) : -1;
                if (low >= 0xdc00 && low < 0xe000) {
                    r->p += 6;
                    ch = 0x10000 + (((gunichar)u - 0xd800) << 10) + ((gunichar)low - 0xdc00);
                } else
                    ch = 0xfffd;
            } else if (u >= 0xdc00 && u < 0xe000)
                ch = 0xfffd;
            if (!ch)
                ch = 0xfffd; /* no NUL inside C strings */
            g_string_append_unichar(out, ch);
            break;
        }
        default:
            goto bad;
        }
    }
    if (*r->p != '"')
        goto bad;
    r->p++;
    if (!g_utf8_validate(out->str, (gssize)out->len, NULL)) {
        char* valid = g_utf8_make_valid(out->str, (gssize)out->len);
        g_string_free(out, TRUE);
        return valid;
    }
    return g_string_free(out, FALSE);
bad:
    g_string_free(out, TRUE);
    return NULL;
}

static MJ* readValue(Reader* r);

static MJ* newValue(MJKind kind)
{
    MJ* v = g_new0(MJ, 1);
    v->kind = kind;
    return v;
}

static MJ* readValue(Reader* r)
{
    skipSpace(r);
    if (++r->depth > MJ_DEPTH)
        return NULL;
    MJ* v = NULL;
    const char c = *r->p;
    if (c == '{' || c == '[') {
        gboolean object = c == '{';
        v = newValue(object ? MJ_OBJECT : MJ_ARRAY);
        v->items = g_ptr_array_new_with_free_func((GDestroyNotify)mj_free);
        if (object)
            v->keys = g_ptr_array_new_with_free_func(g_free);
        r->p++;
        skipSpace(r);
        if (*r->p == (object ? '}' : ']')) {
            r->p++;
            goto done;
        }
        for (;;) {
            char* key = NULL;
            if (object) {
                skipSpace(r);
                key = readString(r);
                skipSpace(r);
                if (!key || *r->p != ':') {
                    g_free(key);
                    goto bad;
                }
                r->p++;
            }
            MJ* item = readValue(r);
            if (!item) {
                g_free(key);
                goto bad;
            }
            g_ptr_array_add(v->items, item);
            if (object)
                g_ptr_array_add(v->keys, key);
            skipSpace(r);
            if (*r->p == ',') {
                r->p++;
                continue;
            }
            if (*r->p == (object ? '}' : ']')) {
                r->p++;
                goto done;
            }
            goto bad;
        }
    }
    if (c == '"') {
        char* text = readString(r);
        if (!text)
            return NULL;
        v = newValue(MJ_STRING);
        v->text = text;
        goto done;
    }
    if (!strncmp(r->p, "true", 4) || !strncmp(r->p, "false", 5)) {
        v = newValue(MJ_BOOL);
        v->truth = c == 't';
        r->p += v->truth ? 4 : 5;
        goto done;
    }
    if (!strncmp(r->p, "null", 4)) {
        v = newValue(MJ_NULL);
        r->p += 4;
        goto done;
    }
    if (c == '-' || g_ascii_isdigit(c)) {
        /* JSON's own form only: g_ascii_strtod also takes hex, "inf", "nan" */
        const char* q = r->p + (c == '-');
        if (!g_ascii_isdigit(*q) || (q[0] == '0' && g_ascii_isdigit(q[1])) || (q[0] == '0' && (q[1] == 'x' || q[1] == 'X')))
            return NULL;
        char* end = NULL;
        double n = g_ascii_strtod(r->p, &end);
        if (!end || end == r->p || !(fabs(n) <= DBL_MAX)) /* 1e999: infinity (isfinite warns with this libc) */
            return NULL;
        v = newValue(MJ_NUMBER);
        v->number = n;
        r->p = end;
        goto done;
    }
    return NULL;
done:
    r->depth--;
    return v;
bad:
    mj_free(v);
    return NULL;
}

MJ* mj_parse(const char* text)
{
    if (!text)
        return NULL;
    Reader r = { text, 0 };
    MJ* v = readValue(&r);
    if (!v)
        return NULL;
    skipSpace(&r);
    if (*r.p) {
        mj_free(v);
        return NULL;
    }
    return v;
}

MJKind mj_kind(const MJ* value)
{
    return value ? value->kind : MJ_NULL;
}

unsigned mj_length(const MJ* value)
{
    return value && value->kind == MJ_ARRAY ? value->items->len : 0;
}

const MJ* mj_at(const MJ* value, unsigned index)
{
    return value && value->kind == MJ_ARRAY && index < value->items->len ? g_ptr_array_index(value->items, index) : NULL;
}

const MJ* mj_get(const MJ* object, const char* key)
{
    if (!object || object->kind != MJ_OBJECT || !key)
        return NULL;
    for (unsigned i = 0; i < object->keys->len; i++) {
        if (!strcmp(g_ptr_array_index(object->keys, i), key))
            return g_ptr_array_index(object->items, i);
    }
    return NULL;
}

const char* mj_string(const MJ* object, const char* key)
{
    return mj_text(mj_get(object, key));
}

double mj_number(const MJ* object, const char* key, double fallback)
{
    const MJ* v = mj_get(object, key);
    return v && v->kind == MJ_NUMBER ? v->number : fallback;
}

gboolean mj_bool(const MJ* object, const char* key)
{
    const MJ* v = mj_get(object, key);
    return v && v->kind == MJ_BOOL && v->truth;
}

const char* mj_text(const MJ* value)
{
    return value && value->kind == MJ_STRING ? value->text : NULL;
}
