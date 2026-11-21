/*
 * minijson: reading the small JSON documents the download service sends
 * (dl-job.c's "req" replies, ps5peek.h's look inside an archive) into a
 * tree. Objects keep their members in order; a repeated key: the first
 * wins. Strings are UTF-8 (\u escapes decoded, surrogate pairs joined, a
 * lone surrogate becomes U+FFFD). Depth is limited (64).
 */
#pragma once

#include <glib.h>

typedef enum { MJ_NULL, MJ_BOOL, MJ_NUMBER, MJ_STRING, MJ_ARRAY, MJ_OBJECT } MJKind;

typedef struct MJ MJ;

/* NULL if text isn't one JSON value (whitespace around it is fine). */
MJ* mj_parse(const char* text);
void mj_free(MJ* value);

MJKind mj_kind(const MJ* value);
/* An array's length (0 for anything else) and its items. */
unsigned mj_length(const MJ* value);
const MJ* mj_at(const MJ* value, unsigned index);
/* An object's member, or NULL. */
const MJ* mj_get(const MJ* object, const char* key);

/* A member's value when it has that kind, else the fallback (NULL, 0,
 * FALSE). */
const char* mj_string(const MJ* object, const char* key);
double mj_number(const MJ* object, const char* key, double fallback);
gboolean mj_bool(const MJ* object, const char* key);
/* The value itself (not a member). */
const char* mj_text(const MJ* value);
