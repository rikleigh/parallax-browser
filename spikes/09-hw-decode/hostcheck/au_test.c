/* Host check for au.c: count access units / keyframes / hidden frames in a
 * clip, optionally writing the AUs back out (Annex-B or IVF-less VP9 frames)
 * so ffmpeg can decode what the console would be fed.
 *   cc -O2 -I.. au_test.c ../au.c -o au_test && ./au_test kind file [out]  */
#include "au.h"
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv)
{
    if (argc < 3) return 2;
    FILE *f = fopen(argv[2], "rb");
    if (!f) return 1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc((size_t)n);
    if (fread(d, 1, (size_t)n, f) != (size_t)n) return 1;
    fclose(f);
    FILE *o = argc > 3 ? fopen(argv[3], "wb") : NULL;
    au_src_t s;
    if (au_open(&s, d, (size_t)n, argv[1]) != 0) { printf("open failed\n"); return 1; }
    static uint8_t buf[8 << 20];
    long len, max = 0, total = 0;
    int key, hidden, count = 0, keys = 0, hid = 0;
    while ((len = au_next(&s, buf, sizeof buf, &key, &hidden)) > 0) {
        count++; keys += key; hid += hidden; total += len;
        if (len > max) max = len;
        if (o) fwrite(buf, 1, (size_t)len, o);
    }
    printf("%s: codec %d %dx%d: %d AUs (%d key, %d hidden), largest %ld B, avg %ld B, fragments %d, end %ld\n",
           argv[2], s.codec, s.width, s.height, count, keys, hid, max, count ? total / count : 0, s.fragments, len);
    if (o) fclose(o);
    return len < 0;
}
