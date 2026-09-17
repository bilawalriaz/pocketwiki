#include "title_lookup.h"

#include <string.h>

/* ---- normalization (mirror of tools/archive_format.py) ---- */

static int tl_is_ws(uint8_t b)
{
    return b == ' ' || b == '\t' || b == '\n' || b == '\r' || b == '\v' || b == '\f';
}

size_t tl_normalize(const uint8_t *in, size_t in_len, uint8_t *out)
{
    size_t lo = 0;
    size_t hi = in_len;
    while (lo < hi && tl_is_ws(in[lo])) lo++;
    while (hi > lo && tl_is_ws(in[hi - 1])) hi--;

    size_t o = 0;
    int pending_space = 0;
    for (size_t k = lo; k < hi; k++) {
        uint8_t b = in[k];
        if (tl_is_ws(b)) {
            pending_space = 1;
            continue;
        }
        if (pending_space && o > 0) out[o++] = ' ';
        pending_space = 0;
        out[o++] = (b >= 'A' && b <= 'Z') ? (uint8_t)(b + 0x20) : b;
    }
    return o;
}

/* ---- helpers ---- */

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t rd16(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static int title_cmp(const uint8_t *a, size_t alen, const uint8_t *b, size_t blen)
{
    size_t n = alen < blen ? alen : blen;
    int c = memcmp(a, b, n);
    if (c != 0) return c;
    if (alen < blen) return -1;
    if (alen > blen) return 1;
    return 0;
}

/* Read the normalized title of entry idx into buf. */
static int read_norm_title(const tl_index_t *ix, uint32_t idx, uint8_t *buf,
                           size_t cap, size_t *out_len)
{
    if (idx >= ix->count) return -1;
    uint8_t ent[TL_ENTRY_SIZE];
    if (ix->read(ix->ctx, ix->entries_off + (uint32_t)idx * ix->entry_size,
                 ent, TL_ENTRY_SIZE) != 0) {
        return -1;
    }
    uint32_t noff = rd32(ent + TL_ENTRY_OFF_NORM_OFF);
    uint32_t nlen = rd16(ent + TL_ENTRY_OFF_NORM_LEN);
    if (nlen > cap) return -1;
    /* noff is an ABSOLUTE file offset (archive format v1) */
    if (ix->read(ix->ctx, noff, buf, nlen) != 0) return -1;
    *out_len = nlen;
    return 0;
}

/* Lower bound: first entry whose normalized title >= needle. */
static int lower_bound(const tl_index_t *ix, const uint8_t *norm, size_t norm_len,
                       uint32_t *out_lo)
{
    uint8_t buf[256];   /* normalized titles are capped at 255 bytes */
    uint32_t lo = 0, hi = ix->count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        size_t tlen;
        if (read_norm_title(ix, mid, buf, sizeof buf, &tlen) != 0) return -1;
        if (title_cmp(buf, tlen, norm, norm_len) < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    *out_lo = lo;
    return 0;
}

/* ---- public API ---- */

int tl_exact(const tl_index_t *ix, const uint8_t *norm, size_t norm_len,
             uint32_t *out_id)
{
    if (norm_len == 0 || norm_len > 255) return -1;
    uint32_t lo;
    if (lower_bound(ix, norm, norm_len, &lo) != 0) return -1;
    if (lo >= ix->count) return -1;

    uint8_t buf[256];
    size_t tlen;
    if (read_norm_title(ix, lo, buf, sizeof buf, &tlen) != 0) return -1;
    if (tlen == norm_len && memcmp(buf, norm, norm_len) == 0) {
        *out_id = lo;   /* ids are sequential == entry position (format v1) */
        return 0;
    }
    return -1;
}

int tl_prefix(const tl_index_t *ix, const uint8_t *norm, size_t norm_len,
              uint32_t *out_ids, int max_ids, int *out_count)
{
    *out_count = 0;
    if (norm_len == 0 || norm_len > 255 || max_ids <= 0) return 0;

    uint32_t lo;
    if (lower_bound(ix, norm, norm_len, &lo) != 0) return -1;

    uint8_t buf[256];
    while (lo < ix->count && *out_count < max_ids) {
        size_t tlen;
        if (read_norm_title(ix, lo, buf, sizeof buf, &tlen) != 0) return -1;
        if (tlen < norm_len || memcmp(buf, norm, norm_len) != 0) break;
        out_ids[(*out_count)++] = lo;
        lo++;
    }
    return 0;
}

int tl_read_display_title(const tl_index_t *ix, uint32_t id, uint8_t *buf,
                          size_t cap, uint32_t *out_len)
{
    if (id >= ix->count) return -1;
    uint8_t ent[TL_ENTRY_SIZE];
    if (ix->read(ix->ctx, ix->entries_off + (uint32_t)id * ix->entry_size,
                 ent, TL_ENTRY_SIZE) != 0) {
        return -1;
    }
    uint32_t doff = rd32(ent + TL_ENTRY_OFF_DISP_OFF);
    uint32_t dlen = rd16(ent + TL_ENTRY_OFF_DISP_LEN);
    if (dlen > cap) return -1;
    /* doff is an ABSOLUTE file offset (archive format v1) */
    if (ix->read(ix->ctx, doff, buf, dlen) != 0) return -1;
    *out_len = dlen;
    return 0;
}
