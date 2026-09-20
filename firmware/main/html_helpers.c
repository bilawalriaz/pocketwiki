#include "html_helpers.h"

#include <stdio.h>
#include <string.h>

size_t html_escape(const char *in, size_t in_len, char *out, size_t cap)
{
    size_t o = 0;
    for (size_t i = 0; i < in_len && o + 1 < cap; i++) {
        uint8_t c = (uint8_t)in[i];
        const char *rep = NULL;
        switch (c) {
        case '&': rep = "&amp;"; break;
        case '<': rep = "&lt;"; break;
        case '>': rep = "&gt;"; break;
        case '"': rep = "&quot;"; break;
        case '\'': rep = "&#39;"; break;
        default: break;
        }
        if (rep) {
            size_t rl = strlen(rep);
            if (o + rl >= cap) break;
            memcpy(out + o, rep, rl);
            o += rl;
        } else {
            out[o++] = (char)c;
        }
    }
    out[o] = '\0';
    return o;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int url_decode(const char *in, size_t in_len, char *out, size_t cap, bool plus_is_space)
{
    size_t o = 0;
    for (size_t i = 0; i < in_len; i++) {
        char c = in[i];
        if (c == '%') {
            if (i + 2 >= in_len) return -1;              /* truncated escape */
            int hi = hexval(in[i + 1]);
            int lo = hexval(in[i + 2]);
            if (hi < 0 || lo < 0) return -1;             /* not hex */
            uint8_t b = (uint8_t)((hi << 4) | lo);
            if (b < 0x20 || b == 0x7f) return -1;        /* control bytes */
            if (o >= cap) return -1;
            out[o++] = (char)b;
            i += 2;
        } else if (plus_is_space && c == '+') {
            if (o >= cap) return -1;
            out[o++] = ' ';
        } else if ((uint8_t)c < 0x20 || c == 0x7f) {
            return -1;
        } else {
            if (o >= cap) return -1;
            out[o++] = c;
        }
    }
    if (o >= cap) return -1;
    out[o] = '\0';
    return (int)o;
}

int url_decode_path(const char *in, size_t in_len, char *out, size_t cap)
{
    return url_decode(in, in_len, out, cap, false);
}

int url_decode_query(const char *in, size_t in_len, char *out, size_t cap)
{
    return url_decode(in, in_len, out, cap, true);
}

bool parse_u32_strict(const char *s, size_t len, uint32_t *out)
{
    if (len == 0 || len > 10) return false;
    uint64_t v = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') return false;
        v = v * 10 + (uint64_t)(s[i] - '0');
    }
    if (v > UINT32_MAX) return false;
    *out = (uint32_t)v;
    return true;
}

void etag_from_crc(uint32_t crc, char *out)
{
    snprintf(out, 13, "crc-%08lx", (unsigned long)crc);
}
