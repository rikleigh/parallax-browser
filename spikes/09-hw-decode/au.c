/* au - see au.h. */
#include "au.h"

#include <string.h>

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)(p[0] << 8 | p[1]);
}
static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* -- Annex-B ----------------------------------------------------------------- */
/* Offset of the next start code (its first zero byte) at or after `from`. */
static size_t next_start(const uint8_t *d, size_t n, size_t from)
{
    for (size_t i = from; i + 3 <= n; i++)
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1)
            return (i > from && d[i - 1] == 0) ? i - 1 : i;
    return n;
}
static size_t payload_of(const uint8_t *d, size_t n, size_t sc)
{
    while (sc < n && d[sc] == 0)
        sc++;
    return sc + 1; /* past the 0x01 */
}

/* Does the NAL unit at `nal` start a new access unit, given that the
 * current one already has a VCL NAL unit? Sets *vcl and *key. */
static int nal_starts_au(int codec, const uint8_t *p, size_t len, int *vcl, int *key)
{
    *vcl = 0;
    if (len < 3)
        return 0;
    if (codec == AU_H264) {
        const int t = p[0] & 0x1f;
        if (t == 1 || t == 5) {
            *vcl = 1;
            *key = *key || t == 5;
            return (p[1] & 0x80) != 0; /* first_mb_in_slice == 0 */
        }
        return t == 9 || t == 7 || t == 8 || t == 6 || (t >= 14 && t <= 18);
    }
    const int t = (p[0] >> 1) & 0x3f;
    if (t < 32) {
        *vcl = 1;
        *key = *key || (t >= 16 && t <= 21);
        return (p[2] & 0x80) != 0; /* first_slice_segment_in_pic_flag */
    }
    return (t >= 32 && t <= 35) || t == 39 || (t >= 41 && t <= 44) || (t >= 48 && t <= 55);
}

static long annexb_next(au_src_t *s, uint8_t *dst, size_t cap, int *key)
{
    const uint8_t *d = s->data;
    const size_t n = s->size;
    size_t start = next_start(d, n, s->pos);
    if (start >= n)
        return 0;
    size_t at = start;
    int seen_vcl = 0;
    *key = 0;
    for (;;) {
        const size_t pl = payload_of(d, n, at);
        const size_t end = next_start(d, n, pl);
        int vcl = 0, k = *key;
        const int starts = nal_starts_au(s->codec, d + pl, end - pl, &vcl, &k);
        if (seen_vcl && starts)
            break; /* this NAL unit begins the next AU */
        *key = k;
        seen_vcl |= vcl;
        at = end;
        if (at >= n)
            break;
    }
    const size_t len = at - start;
    if (len > cap)
        return -1;
    memcpy(dst, d + start, len);
    s->pos = at;
    return (long)len;
}

/* -- VP9 frames -------------------------------------------------------------- */
/* Split a VP9 superframe (annex B of the VP9 spec) into its frames. */
static int vp9_split(au_src_t *s, const uint8_t *p, uint32_t n)
{
    s->nsub = s->isub = 0;
    if (n > 0) {
        const uint8_t m = p[n - 1];
        if ((m & 0xe0) == 0xc0) {
            const int frames = (m & 7) + 1, mag = ((m >> 3) & 3) + 1;
            const uint32_t idx = 2 + (uint32_t)(mag * frames);
            if (n >= idx && p[n - idx] == m) {
                const uint8_t *q = p + n - idx + 1;
                uint32_t off = 0;
                for (int f = 0; f < frames; f++) {
                    uint32_t sz = 0;
                    for (int b = 0; b < mag; b++)
                        sz |= (uint32_t)*q++ << (8 * b);
                    if (off + sz > n - idx)
                        return -1;
                    s->sub[s->nsub] = p + off;
                    s->sub_size[s->nsub++] = sz;
                    off += sz;
                }
                return 0;
            }
        }
    }
    s->sub[0] = p;
    s->sub_size[0] = n;
    s->nsub = 1;
    return 0;
}

/* From the uncompressed header: key frame? shown? */
static void vp9_flags(const uint8_t *p, uint32_t n, int *key, int *hidden)
{
    *key = 0;
    *hidden = 0;
    if (n < 1 || (p[0] >> 6) != 2)
        return;
    const int profile = ((p[0] >> 5) & 1) | (((p[0] >> 4) & 1) << 1);
    int bit = profile == 3 ? 2 : 3; /* bit index in p[0], MSB = 7 */
    if ((p[0] >> bit) & 1)
        return; /* show_existing_frame: shown, not a key frame */
    bit--;
    const int frame_type = bit >= 0 ? (p[0] >> bit) & 1 : (p[1] >> (8 + bit)) & 1;
    bit--;
    const int show = bit >= 0 ? (p[0] >> bit) & 1 : n > 1 ? (p[1] >> (8 + bit)) & 1 : 1;
    *key = frame_type == 0;
    *hidden = !show;
}

static long vp9_emit(au_src_t *s, uint8_t *dst, size_t cap, int *key, int *hidden)
{
    const uint8_t *p = s->sub[s->isub];
    const uint32_t n = s->sub_size[s->isub++];
    if (n > cap)
        return -1;
    memcpy(dst, p, n);
    vp9_flags(p, n, key, hidden);
    return (long)n;
}

static long ivf_next(au_src_t *s, uint8_t *dst, size_t cap, int *key, int *hidden)
{
    if (s->isub < s->nsub)
        return vp9_emit(s, dst, cap, key, hidden);
    if (s->pos + 12 > s->size)
        return 0;
    const uint32_t n = le32(s->data + s->pos);
    if (s->pos + 12 + n > s->size)
        return 0;
    const uint8_t *p = s->data + s->pos + 12;
    s->pos += 12 + n;
    if (vp9_split(s, p, n) != 0)
        return -1;
    return vp9_emit(s, dst, cap, key, hidden);
}

/* -- fragmented MP4 ------------------------------------------------------------ */
typedef struct { size_t at, body, end; uint32_t type; } box_t;

static int box_at(const uint8_t *d, size_t lim, size_t at, box_t *b)
{
    if (at + 8 > lim)
        return 0;
    uint64_t size = be32(d + at);
    size_t hdr = 8;
    if (size == 1) {
        if (at + 16 > lim)
            return 0;
        size = (uint64_t)be32(d + at + 8) << 32 | be32(d + at + 12);
        hdr = 16;
    } else if (size == 0) {
        size = lim - at;
    }
    if (size < hdr || at + size > lim)
        return 0;
    b->at = at;
    b->body = at + hdr;
    b->end = at + (size_t)size;
    b->type = be32(d + at + 4);
    return 1;
}
#define FOURCC(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

static int find_child(const uint8_t *d, size_t from, size_t to, uint32_t type, box_t *out)
{
    box_t b;
    for (size_t at = from; box_at(d, to, at, &b); at = b.end)
        if (b.type == type) {
            *out = b;
            return 1;
        }
    return 0;
}

static void ps_add(au_src_t *s, const uint8_t *nal, size_t len)
{
    if (s->ps_len + 4 + len > sizeof s->ps)
        return;
    memcpy(s->ps + s->ps_len, "\0\0\0\1", 4);
    memcpy(s->ps + s->ps_len + 4, nal, len);
    s->ps_len += 4 + len;
}

static int parse_sample_entry(au_src_t *s, const uint8_t *d, box_t *e)
{
    /* VisualSampleEntry: 8 header, 6 reserved, 2 dri, 16 predefined, w, h ... 78 bytes */
    if (e->body + 78 > e->end)
        return -1;
    s->width = be16(d + e->body + 24);
    s->height = be16(d + e->body + 26);
    box_t c;
    const size_t kids = e->body + 78;
    if (e->type == FOURCC('a', 'v', 'c', '1') || e->type == FOURCC('a', 'v', 'c', '3')) {
        s->codec = AU_H264;
        if (!find_child(d, kids, e->end, FOURCC('a', 'v', 'c', 'C'), &c) || c.body + 6 > c.end)
            return e->type == FOURCC('a', 'v', 'c', '3') ? 0 : -1;
        const uint8_t *p = d + c.body, *end = d + c.end;
        s->nal_len = (p[4] & 3) + 1;
        p += 5;
        for (int list = 0; list < 2 && p < end; list++) {
            int count = list == 0 ? (*p++ & 0x1f) : *p++;
            while (count-- > 0 && p + 2 <= end) {
                const size_t len = be16(p);
                if (p + 2 + len > end)
                    return -1;
                ps_add(s, p + 2, len);
                p += 2 + len;
            }
        }
        return 0;
    }
    if (e->type == FOURCC('h', 'v', 'c', '1') || e->type == FOURCC('h', 'e', 'v', '1')) {
        s->codec = AU_HEVC;
        if (!find_child(d, kids, e->end, FOURCC('h', 'v', 'c', 'C'), &c) || c.body + 23 > c.end)
            return e->type == FOURCC('h', 'e', 'v', '1') ? 0 : -1;
        const uint8_t *p = d + c.body, *end = d + c.end;
        s->nal_len = (p[21] & 3) + 1;
        int arrays = p[22];
        p += 23;
        while (arrays-- > 0 && p + 3 <= end) {
            int count = be16(p + 1);
            p += 3;
            while (count-- > 0 && p + 2 <= end) {
                const size_t len = be16(p);
                if (p + 2 + len > end)
                    return -1;
                ps_add(s, p + 2, len);
                p += 2 + len;
            }
        }
        return 0;
    }
    if (e->type == FOURCC('v', 'p', '0', '9')) {
        s->codec = AU_VP9;
        return 0;
    }
    return -1;
}

static int mp4_open(au_src_t *s)
{
    const uint8_t *d = s->data;
    box_t b, trak, x;
    if (!find_child(d, 0, s->size, FOURCC('m', 'o', 'o', 'v'), &b))
        return -1;
    s->first_moof = b.end;
    for (size_t at = b.body; box_at(d, b.end, at, &trak); at = trak.end) {
        if (trak.type != FOURCC('t', 'r', 'a', 'k'))
            continue;
        box_t mdia, hdlr, minf, stbl, stsd;
        if (!find_child(d, trak.body, trak.end, FOURCC('m', 'd', 'i', 'a'), &mdia) ||
            !find_child(d, mdia.body, mdia.end, FOURCC('h', 'd', 'l', 'r'), &hdlr) ||
            hdlr.body + 12 > hdlr.end || be32(d + hdlr.body + 8) != FOURCC('v', 'i', 'd', 'e'))
            continue;
        if (!find_child(d, mdia.body, mdia.end, FOURCC('m', 'i', 'n', 'f'), &minf) ||
            !find_child(d, minf.body, minf.end, FOURCC('s', 't', 'b', 'l'), &stbl) ||
            !find_child(d, stbl.body, stbl.end, FOURCC('s', 't', 's', 'd'), &stsd) ||
            !box_at(d, stsd.end, stsd.body + 8, &x))
            return -1;
        return parse_sample_entry(s, d, &x);
    }
    return -1;
}

/* Parse the next moof (one track) into the sample table. 0 at the end. */
static int mp4_fragment(au_src_t *s)
{
    const uint8_t *d = s->data;
    box_t moof, traf, tfhd, trun;
    for (;;) {
        if (!box_at(d, s->size, s->pos, &moof))
            return 0;
        s->pos = moof.end;
        if (moof.type == FOURCC('m', 'o', 'o', 'f'))
            break;
    }
    if (!find_child(d, moof.body, moof.end, FOURCC('t', 'r', 'a', 'f'), &traf) ||
        !find_child(d, traf.body, traf.end, FOURCC('t', 'f', 'h', 'd'), &tfhd))
        return -1;
    const uint32_t tf = be32(d + tfhd.body) & 0xffffff;
    size_t p = tfhd.body + 8; /* version/flags, track_ID */
    uint64_t base = moof.at;
    if (tf & 0x1) {
        base = (uint64_t)be32(d + p) << 32 | be32(d + p + 4);
        p += 8;
    }
    if (tf & 0x2) p += 4;
    if (tf & 0x8) p += 4;
    uint32_t def_size = 0;
    if (tf & 0x10) def_size = be32(d + p);
    s->nsamples = s->sample = 0;
    for (size_t at = traf.body; box_at(d, traf.end, at, &trun); at = trun.end) {
        if (trun.type != FOURCC('t', 'r', 'u', 'n'))
            continue;
        const uint32_t fl = be32(d + trun.body) & 0xffffff;
        const uint32_t count = be32(d + trun.body + 4);
        size_t q = trun.body + 8;
        int32_t data_off = 0;
        if (fl & 0x1) { data_off = (int32_t)be32(d + q); q += 4; }
        if (fl & 0x4) q += 4;
        uint64_t off = base + (uint64_t)(int64_t)data_off;
        for (uint32_t i = 0; i < count && s->nsamples < AU_MAX_SAMPLES; i++) {
            if (fl & 0x100) q += 4;
            uint32_t sz = def_size;
            if (fl & 0x200) { sz = be32(d + q); q += 4; }
            if (fl & 0x400) q += 4;
            if (fl & 0x800) q += 4;
            if (off + sz > s->size)
                return -1;
            s->sample_off[s->nsamples] = (uint32_t)off;
            s->sample_size[s->nsamples++] = sz;
            off += sz;
        }
    }
    s->fragments++;
    return s->nsamples > 0 ? 1 : -1;
}

static long mp4_next(au_src_t *s, uint8_t *dst, size_t cap, int *key, int *hidden)
{
    if (s->codec == AU_VP9 && s->isub < s->nsub)
        return vp9_emit(s, dst, cap, key, hidden);
    if (s->sample >= s->nsamples) {
        const int r = mp4_fragment(s);
        if (r <= 0)
            return r;
    }
    const uint8_t *p = s->data + s->sample_off[s->sample];
    const uint32_t n = s->sample_size[s->sample++];
    if (s->codec == AU_VP9) {
        if (vp9_split(s, p, n) != 0)
            return -1;
        return vp9_emit(s, dst, cap, key, hidden);
    }
    /* Length-prefixed -> Annex-B; note keyframes and in-band SPS. */
    size_t o = 0;
    int has_sps = 0;
    *key = 0;
    for (uint32_t i = 0; i + (uint32_t)s->nal_len <= n;) {
        uint32_t len = 0;
        for (int b = 0; b < s->nal_len; b++)
            len = len << 8 | p[i + b];
        i += (uint32_t)s->nal_len;
        if (len == 0 || i + len > n || o + 4 + len > cap)
            return -1;
        if (s->codec == AU_H264) {
            const int t = p[i] & 0x1f;
            *key |= t == 5;
            has_sps |= t == 7;
        } else {
            const int t = (p[i] >> 1) & 0x3f;
            *key |= t >= 16 && t <= 21;
            has_sps |= t == 33;
        }
        memcpy(dst + o, "\0\0\0\1", 4);
        memcpy(dst + o + 4, p + i, len);
        o += 4 + len;
        i += len;
    }
    if (*key && !has_sps && s->ps_len) {
        if (o + s->ps_len > cap)
            return -1;
        memmove(dst + s->ps_len, dst, o);
        memcpy(dst, s->ps, s->ps_len);
        o += s->ps_len;
    }
    return (long)o;
}

/* -- public ---------------------------------------------------------------------- */
int au_open(au_src_t *s, const uint8_t *data, size_t size, const char *kind)
{
    memset(s, 0, sizeof *s);
    s->data = data;
    s->size = size;
    if (!strcmp(kind, "h264") || !strcmp(kind, "hevc")) {
        s->codec = kind[0] == 'h' && kind[1] == '2' ? AU_H264 : AU_HEVC;
        return 0;
    }
    if (!strcmp(kind, "ivf")) {
        if (size < 32 || memcmp(data, "DKIF", 4) != 0)
            return -1;
        s->codec = AU_VP9;
        s->container = 1;
        s->width = data[12] | data[13] << 8;
        s->height = data[14] | data[15] << 8;
        s->pos = data[6] | data[7] << 8; /* header length */
        return 0;
    }
    if (!strcmp(kind, "mp4")) {
        s->container = 2;
        if (mp4_open(s) != 0)
            return -1;
        s->pos = s->first_moof;
        return 0;
    }
    return -1;
}

long au_next(au_src_t *s, uint8_t *dst, size_t cap, int *key, int *hidden)
{
    *key = *hidden = 0;
    switch (s->container) {
    case 0: return annexb_next(s, dst, cap, key);
    case 1: return ivf_next(s, dst, cap, key, hidden);
    default: return mp4_next(s, dst, cap, key, hidden);
    }
}

void au_rewind(au_src_t *s)
{
    s->nsub = s->isub = 0;
    s->nsamples = s->sample = 0;
    s->pos = s->container == 2 ? s->first_moof : s->container == 1 ? (size_t)(s->data[6] | s->data[7] << 8) : 0;
}
