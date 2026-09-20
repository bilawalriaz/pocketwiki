/* Host-side harness for the firmware's title_lookup.c.
 *
 * Usage: c_tl_harness <index.bin>
 *   stdin lines:
 *     n <hex>   -> normalized hex of <hex>
 *     e <hex>   -> entry id of exact match, or -
 *     p <hex>   -> space-separated prefix-match ids, or -
 *
 * The index file is loaded into RAM and served through the same reader
 * callback interface the firmware uses, so the exact firmware code paths
 * (binary search, title reads) are exercised.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "title_lookup.h"

static uint8_t *s_data;
static size_t s_len;

static int ram_read(void *ctx, uint32_t offset, uint8_t *buf, size_t len)
{
    (void)ctx;
    if (offset > s_len || len > s_len - offset) return -1;
    memcpy(buf, s_data + offset, len);
    return 0;
}

static int hex_to_bytes(const char *hex, uint8_t *out, size_t cap, size_t *out_len)
{
    size_t n = strlen(hex);
    if (n % 2) return -1;
    n /= 2;
    if (n > cap) return -1;
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        if (sscanf(hex + 2 * i, "%2x", &v) != 1) return -1;
        out[i] = (uint8_t)v;
    }
    *out_len = n;
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <index.bin>\n", argv[0]);
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("open"); return 2; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    s_data = malloc((size_t)sz);
    if (!s_data || fread(s_data, 1, (size_t)sz, f) != (size_t)sz) {
        fprintf(stderr, "read failed\n");
        return 2;
    }
    fclose(f);
    s_len = (size_t)sz;

    tl_index_t ix = {
        .entries_off = 32,
        .strings_off = 0,
        .count = 0,
        .entry_size = TL_ENTRY_SIZE,
        .read = ram_read,
        .ctx = NULL,
    };
    /* derive layout from the header like the firmware does */
    ix.strings_off = (uint32_t)(s_data[16] | (s_data[17] << 8) | (s_data[18] << 16) | (s_data[19] << 24));
    ix.count = (uint32_t)(s_data[8] | (s_data[9] << 8) | (s_data[10] << 16) | (s_data[11] << 24));

    char line[1024];
    uint8_t in[512], norm[512];
    while (fgets(line, sizeof line, stdin)) {
        size_t l = strlen(line);
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = '\0';
        if (l < 2 || line[1] != ' ') continue;
        char mode = line[0];
        size_t in_len;
        if (hex_to_bytes(line + 2, in, sizeof in, &in_len) != 0) {
            printf("ERR\n");
            continue;
        }
        if (mode == 'n') {
            size_t olen = tl_normalize(in, in_len, norm);
            for (size_t i = 0; i < olen; i++) printf("%02x", norm[i]);
            printf("\n");
        } else if (mode == 'e') {
            size_t norm_len = tl_normalize(in, in_len, norm);
            uint32_t id;
            if (tl_exact(&ix, norm, norm_len, &id) == 0) printf("%lu\n", (unsigned long)id);
            else printf("-\n");
        } else if (mode == 'p') {
            size_t norm_len = tl_normalize(in, in_len, norm);
            uint32_t ids[20];
            int n = 0;
            if (tl_prefix(&ix, norm, norm_len, ids, 20, &n) == 0 && n > 0) {
                for (int i = 0; i < n; i++) printf("%s%lu", i ? " " : "", (unsigned long)ids[i]);
                printf("\n");
            } else {
                printf("-\n");
            }
        } else {
            printf("ERR\n");
        }
    }
    free(s_data);
    return 0;
}
