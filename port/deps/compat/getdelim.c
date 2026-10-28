/* getdelim(3)/getline(3): missing from the SDK's libraries. */
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>

ssize_t getdelim(char **lineptr, size_t *n, int delim, FILE *stream)
{
    size_t len = 0;
    int c;
    if (!lineptr || !n || !stream) {
        errno = EINVAL;
        return -1;
    }
    if (!*lineptr || *n == 0) {
        *n = 128;
        if (!(*lineptr = malloc(*n)))
            return -1;
    }
    while ((c = getc(stream)) != EOF) {
        if (len + 2 > *n) {
            size_t nn = *n * 2;
            char *p = realloc(*lineptr, nn);
            if (!p)
                return -1;
            *lineptr = p;
            *n = nn;
        }
        (*lineptr)[len++] = (char)c;
        if (c == delim)
            break;
    }
    if (len == 0)
        return -1;
    (*lineptr)[len] = '\0';
    return (ssize_t)len;
}

ssize_t getline(char **lineptr, size_t *n, FILE *stream)
{
    return getdelim(lineptr, n, '\n', stream);
}
