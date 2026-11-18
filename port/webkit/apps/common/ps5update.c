/* ps5update.c: see ps5update.h. */
#include "ps5update.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define RECEIPT_LIMIT (1u << 20)
#define JSON_DEPTH 64

char* ps5update_read(const char* path, size_t limit)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return NULL;
    size_t capacity = 4096, length = 0;
    char* text = malloc(capacity);
    for (;;) {
        if (!text) {
            close(fd);
            errno = ENOMEM;
            return NULL;
        }
        if (length + 1 >= capacity) {
            if (capacity > limit + 1) {
                free(text);
                close(fd);
                errno = EFBIG;
                return NULL;
            }
            capacity *= 2;
            char* bigger = realloc(text, capacity);
            if (!bigger)
                free(text);
            text = bigger;
            continue;
        }
        ssize_t n = read(fd, text + length, capacity - 1 - length);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) {
            int saved = errno;
            free(text);
            close(fd);
            errno = saved;
            return NULL;
        }
        if (!n)
            break;
        length += (size_t)n;
    }
    close(fd);
    if (length > limit) {
        free(text);
        errno = EFBIG;
        return NULL;
    }
    if (memchr(text, 0, length)) {
        free(text);
        errno = EINVAL;
        return NULL;
    }
    text[length] = 0;
    return text;
}

/* ---- JSON: a whole document checked, top-level string values read ---- */

typedef struct {
    const char* p;
    int depth;
} Json;

static void space(Json* j)
{
    while (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r')
        j->p++;
}

static int hex4(const char* p, unsigned* out)
{
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9')
            v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f')
            v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            v |= (unsigned)(c - 'A' + 10);
        else
            return -1;
    }
    *out = v;
    return 0;
}

/* A string at j->p (its opening quote), decoded into out (NULL: only
 * checked). 0, -1 bad JSON, -2 longer than size - 1. */
static int string(Json* j, char* out, size_t size, size_t* outLength)
{
    if (*j->p != '"')
        return -1;
    j->p++;
    size_t n = 0;
    int tooLong = 0;
#define PUT(c) do { if (out && n + 1 < size) out[n] = (char)(c); else if (out) tooLong = 1; n++; } while (0)
    for (;;) {
        unsigned char c = (unsigned char)*j->p;
        if (!c || c < 0x20)
            return -1;
        if (c == '"') {
            j->p++;
            break;
        }
        if (c != '\\') {
            PUT(c);
            j->p++;
            continue;
        }
        char e = j->p[1];
        j->p += 2;
        switch (e) {
        case '"': PUT('"'); break;
        case '\\': PUT('\\'); break;
        case '/': PUT('/'); break;
        case 'b': PUT('\b'); break;
        case 'f': PUT('\f'); break;
        case 'n': PUT('\n'); break;
        case 'r': PUT('\r'); break;
        case 't': PUT('\t'); break;
        case 'u': {
            unsigned u;
            if (hex4(j->p, &u))
                return -1;
            j->p += 4;
            if (u >= 0xD800 && u < 0xDC00) {
                unsigned low;
                if (j->p[0] != '\\' || j->p[1] != 'u' || hex4(j->p + 2, &low) || low < 0xDC00 || low >= 0xE000)
                    return -1;
                j->p += 6;
                u = 0x10000 + ((u - 0xD800) << 10) + (low - 0xDC00);
            } else if (u >= 0xDC00 && u < 0xE000)
                return -1;
            if (!u)
                return -1; /* no NUL in a value this reads */
            if (u < 0x80)
                PUT(u);
            else if (u < 0x800) {
                PUT(0xC0 | (u >> 6));
                PUT(0x80 | (u & 0x3F));
            } else if (u < 0x10000) {
                PUT(0xE0 | (u >> 12));
                PUT(0x80 | ((u >> 6) & 0x3F));
                PUT(0x80 | (u & 0x3F));
            } else {
                PUT(0xF0 | (u >> 18));
                PUT(0x80 | ((u >> 12) & 0x3F));
                PUT(0x80 | ((u >> 6) & 0x3F));
                PUT(0x80 | (u & 0x3F));
            }
            break;
        }
        default:
            return -1;
        }
    }
#undef PUT
    if (out && size)
        out[n < size ? n : size - 1] = 0;
    if (outLength)
        *outLength = n;
    return tooLong ? -2 : 0;
}

static int value(Json* j);

static int digits(Json* j)
{
    if (*j->p < '0' || *j->p > '9')
        return -1;
    while (*j->p >= '0' && *j->p <= '9')
        j->p++;
    return 0;
}

static int number(Json* j)
{
    if (*j->p == '-')
        j->p++;
    if (*j->p == '0')
        j->p++;
    else if (digits(j))
        return -1;
    if (*j->p == '.') {
        j->p++;
        if (digits(j))
            return -1;
    }
    if (*j->p == 'e' || *j->p == 'E') {
        j->p++;
        if (*j->p == '+' || *j->p == '-')
            j->p++;
        if (digits(j))
            return -1;
    }
    return 0;
}

static int literal(Json* j, const char* word)
{
    size_t n = strlen(word);
    if (strncmp(j->p, word, n))
        return -1;
    j->p += n;
    return 0;
}

static int container(Json* j, char close)
{
    if (++j->depth > JSON_DEPTH)
        return -1;
    j->p++;
    space(j);
    if (*j->p == close) {
        j->p++;
        j->depth--;
        return 0;
    }
    for (;;) {
        space(j);
        if (close == '}') {
            if (string(j, NULL, 0, NULL))
                return -1;
            space(j);
            if (*j->p++ != ':')
                return -1;
            space(j);
        }
        if (value(j))
            return -1;
        space(j);
        if (*j->p == ',') {
            j->p++;
            continue;
        }
        if (*j->p++ != close)
            return -1;
        j->depth--;
        return 0;
    }
}

static int value(Json* j)
{
    switch (*j->p) {
    case '{': return container(j, '}');
    case '[': return container(j, ']');
    case '"': return string(j, NULL, 0, NULL) ? -1 : 0;
    case 't': return literal(j, "true");
    case 'f': return literal(j, "false");
    case 'n': return literal(j, "null");
    default: return number(j);
    }
}

/* text is one valid JSON value. */
static int jsonValid(const char* text)
{
    Json j = { text, 0 };
    space(&j);
    if (value(&j))
        return 0;
    space(&j);
    return !*j.p;
}

int ps5update_json_string(const char* text, const char* key, char* out, size_t size)
{
    if (!text || !out || !size)
        return -1;
    Json j = { text, 1 };
    space(&j);
    if (*j.p != '{')
        return -1;
    j.p++;
    int found = 0;
    space(&j);
    if (*j.p == '}')
        j.p++;
    else {
        for (;;) {
            space(&j);
            char name[128];
            size_t nameLength = 0;
            int r = string(&j, name, sizeof name, &nameLength);
            if (r == -1)
                return -1;
            int match = !r && nameLength == strlen(key) && !strcmp(name, key);
            space(&j);
            if (*j.p++ != ':')
                return -1;
            space(&j);
            if (match) {
                if (found)
                    return -1; /* twice: which one counts is anyone's guess */
                found = 1;
                if (*j.p != '"')
                    return -1;
                if (string(&j, out, size, NULL))
                    return -1;
            } else if (value(&j))
                return -1;
            space(&j);
            if (*j.p == ',') {
                j.p++;
                continue;
            }
            if (*j.p++ != '}')
                return -1;
            break;
        }
    }
    space(&j);
    if (*j.p)
        return -1;
    if (!found)
        out[0] = 0;
    return found;
}

int ps5update_line_value(const char* text, const char* key, char* out, size_t size)
{
    size_t length = strlen(key);
    for (const char* line = text; line && *line;) {
        if (!strncmp(line, key, length) && line[length] == '=') {
            const char* v = line + length + 1;
            size_t n = strcspn(v, "\r\n");
            if (n >= size)
                return -1;
            memcpy(out, v, n);
            out[n] = 0;
            return 0;
        }
        const char* next = strchr(line, '\n');
        line = next ? next + 1 : NULL;
    }
    return -1;
}

int ps5update_param(const char* folder, char* title, size_t titleSize, char* version, size_t versionSize)
{
    char path[1024];
    if (snprintf(path, sizeof path, "%s/sce_sys/param.json", folder) >= (int)sizeof path)
        return -1;
    char* text = ps5update_read(path, 64 * 1024);
    int ok = text && ps5update_json_string(text, "titleId", title, titleSize) == 1
        && ps5update_json_string(text, "contentVersion", version, versionSize) == 1 && *title && *version;
    free(text);
    return ok ? 0 : -1;
}

/* ---- ProsperoStore ---- */

/* "/a//b/" -> "/a/b". */
static void tidyPath(const char* in, char* out, size_t size)
{
    size_t n = 0;
    for (const char* p = in; *p && n + 1 < size; p++) {
        if (*p == '/' && n && out[n - 1] == '/')
            continue;
        out[n++] = *p;
    }
    while (n > 1 && out[n - 1] == '/')
        n--;
    out[n] = 0;
}

static int sameFolder(const char* a, const char* b)
{
    char ta[1024], tb[1024];
    tidyPath(a, ta, sizeof ta);
    tidyPath(b, tb, sizeof tb);
    if (!strcmp(ta, tb))
        return 1;
    char ra[PATH_MAX], rb[PATH_MAX];
    if (realpath(a, ra) && realpath(b, rb) && !strcmp(ra, rb))
        return 1;
    struct stat sa, sb;
    return !stat(a, &sa) && !stat(b, &sb) && sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}

static void note(ps5update_store* out, const char* format, const char* a, const char* b)
{
    size_t used = strlen(out->note);
    if (used + 2 >= sizeof out->note)
        return;
    if (used) {
        memcpy(out->note + used, "; ", 3);
        used += 2;
    }
    snprintf(out->note + used, sizeof out->note - used, format, a ? a : "", b ? b : "");
}

void ps5update_store_check(const char* root, const char* title, const char* folder, ps5update_store* out)
{
    memset(out, 0, sizeof *out);
    char path[1024], why[64];
    struct stat st;

    snprintf(path, sizeof path, "%s/receipts/%s.json", root, title);
    if (lstat(path, &st)) {
        if (errno == ENOENT)
            note(out, "no receipt", NULL, NULL);
        else {
            out->managed = 1;
            snprintf(why, sizeof why, "errno %d", errno);
            note(out, "the receipt %s can't be looked at (%s): taken as the store's", path, why);
        }
    } else {
        char* text = ps5update_read(path, RECEIPT_LIMIT);
        char id[64], location[1024], version[64];
        int idFound = text ? ps5update_json_string(text, "titleId", id, sizeof id) : -1;
        int locationFound = idFound >= 0 ? ps5update_json_string(text, "location", location, sizeof location) : -1;
        int versionFound = locationFound >= 0 ? ps5update_json_string(text, "contentVersion", version, sizeof version) : -1;
        if (!text) {
            out->managed = 1;
            snprintf(why, sizeof why, "errno %d", errno);
            note(out, "the receipt %s can't be read (%s): taken as the store's", path, why);
        } else if (idFound < 0 || locationFound < 0) {
            out->managed = 1;
            note(out, "the receipt %s isn't understood: taken as the store's", path, NULL);
        } else if (idFound == 0 || strcmp(id, title)) {
            out->managed = 1;
            note(out, "the receipt %s is for %s: taken as the store's", path, idFound ? id : "no title");
        } else if (locationFound == 0 || !*location) {
            out->managed = 1;
            note(out, "the receipt %s names no folder: taken as the store's", path, NULL);
        } else {
            char target[1200];
            snprintf(target, sizeof target, "%s/%s", location, title);
            if (sameFolder(target, folder)) {
                out->managed = 1;
                note(out, "the receipt names this folder (%s, version %s): the store's", target, versionFound == 1 ? version : "?");
            } else
                note(out, "the receipt names another folder (%s, not %s)", target, folder);
        }
        free(text);
    }

    snprintf(path, sizeof path, "%s/journal.json", root);
    if (lstat(path, &st)) {
        if (errno != ENOENT) {
            out->busy = 1;
            snprintf(why, sizeof why, "errno %d", errno);
            note(out, "the journal %s can't be looked at (%s): an operation taken as in progress", path, why);
        }
        return;
    }
    char* text = ps5update_read(path, RECEIPT_LIMIT);
    char quoted[80];
    snprintf(quoted, sizeof quoted, "\"%s\"", title);
    if (!text) {
        out->busy = 1;
        snprintf(why, sizeof why, "errno %d", errno);
        note(out, "the journal %s can't be read (%s): an operation taken as in progress", path, why);
    } else if (!jsonValid(text)) {
        out->busy = 1;
        note(out, "the journal %s isn't understood: an operation taken as in progress", path, NULL);
    } else if (strstr(text, quoted)) {
        out->busy = 1;
        note(out, "the journal names %s: an operation of the store's on it is in progress", title, NULL);
    } else
        note(out, "the journal is about another app", NULL, NULL);
    free(text);
}

int ps5update_remove_tree(const char* path)
{
    struct stat st;
    if (lstat(path, &st))
        return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(st.st_mode))
        return unlink(path);
    DIR* dir = opendir(path);
    if (!dir)
        return -1;
    int result = 0;
    struct dirent* d;
    while ((d = readdir(dir))) {
        if (!strcmp(d->d_name, ".") || !strcmp(d->d_name, ".."))
            continue;
        char child[2048];
        int n = snprintf(child, sizeof child, "%s/%s", path, d->d_name);
        if (n < 0 || (size_t)n >= sizeof child || ps5update_remove_tree(child))
            result = -1;
    }
    closedir(dir);
    if (rmdir(path))
        result = -1;
    return result;
}
