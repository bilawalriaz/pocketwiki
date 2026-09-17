/* Host harness for the firmware's article decode path.
 *
 * Mirrors stream_article_deflate() in firmware/main/web_server.c: a 32 KiB ring
 * is pre-loaded right-aligned with the pack's trained dictionary, and the raw
 * DEFLATE stream is inflated out of it with the same 1 KiB input chunks and
 * room-to-ring-end output windows the device uses. A divergence in the
 * dictionary convention, the chunking or the trailing-input check fails here
 * instead of on a board.
 *
 * Usage: c_archive_decode_harness content.bin index.bin <entry_id>
 * Prints the decoded article body to stdout; exits non-zero on any failure.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "miniz.h"

/* The firmware's inflate ring: window, dictionary slot and output buffer. */
#define DEFLATE_RING 32768u
#define CHUNK 1024

static uint8_t *read_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)n + 1);
    if (buf == NULL || (n > 0 && fread(buf, 1, (size_t)n, f) != (size_t)n)) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *out_len = (size_t)n;
    return buf;
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t crc32_buf(uint32_t crc, const uint8_t *p, size_t n)
{
    /* zlib-compatible CRC32 (matches the index entries). */
    static uint32_t table[256];
    static int init = 0;
    if (!init) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = 1;
    }
    crc = ~crc;
    for (size_t i = 0; i < n; i++) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

/* The device path: pre-load the ring, then inflate the frame in bounded steps. */
static int decode_deflate(const uint8_t *frame, uint32_t cl, uint32_t ul,
                          uint32_t expected_crc, const uint8_t *dict, size_t dict_len)
{
    if (dict_len > DEFLATE_RING) {
        fprintf(stderr, "dictionary larger than the ring\n");
        return 1;
    }
    static uint8_t ring[DEFLATE_RING];
    memset(ring, 0, sizeof ring);
    if (dict_len) memcpy(ring + DEFLATE_RING - dict_len, dict, dict_len);

    tinfl_decompressor *decomp = tinfl_decompressor_alloc();
    if (decomp == NULL) {
        fprintf(stderr, "tinfl_decompressor_alloc failed\n");
        return 1;
    }
    tinfl_init(decomp);   /* resets m_state only: the pre-loaded ring survives */

    uint8_t in[CHUNK];
    size_t sent_in = 0;
    size_t total_out = 0;
    uint32_t output_crc = 0;
    bool done = false;
    tinfl_status status = TINFL_STATUS_FAILED;

    for (;;) {
        size_t n = cl - sent_in;
        if (n > sizeof in) n = sizeof in;
        if (n > 0) memcpy(in, frame + sent_in, n);
        bool more_input = sent_in + n < cl;
        size_t consumed = n;
        size_t offset = total_out & (DEFLATE_RING - 1);
        size_t room = DEFLATE_RING - offset;
        size_t produced = room;
        status = tinfl_decompress(decomp, in, &consumed, ring, ring + offset,
                                  &produced, more_input ? TINFL_FLAG_HAS_MORE_INPUT : 0);
        sent_in += consumed;
        if (produced > 0) {
            total_out += produced;
            if (total_out > ul) {
                fprintf(stderr, "deflate output exceeds declared length\n");
                tinfl_decompressor_free(decomp);
                return 1;
            }
            output_crc = crc32_buf(output_crc, ring + offset, produced);
            fwrite(ring + offset, 1, produced, stdout);
        }
        if (status == TINFL_STATUS_DONE) {
            done = true;
            break;
        }
        if (status != TINFL_STATUS_HAS_MORE_OUTPUT && status != TINFL_STATUS_NEEDS_MORE_INPUT) {
            break;
        }
        if (status == TINFL_STATUS_NEEDS_MORE_INPUT && consumed == 0 && !more_input) {
            break;
        }
    }
    tinfl_decompressor_free(decomp);

    if (!done || sent_in != cl) {
        fprintf(stderr, "truncated or trailing deflate input (status=%d)\n", (int)status);
        return 1;
    }
    if (total_out != ul) {
        fprintf(stderr, "deflate length mismatch %zu != %u\n", total_out, ul);
        return 1;
    }
    if (output_crc != expected_crc) {
        fprintf(stderr, "deflate CRC mismatch\n");
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "usage: %s content.bin index.bin <entry_id>\n", argv[0]);
        return 2;
    }
    size_t clen, ilen;
    uint8_t *content = read_file(argv[1], &clen);
    uint8_t *index = read_file(argv[2], &ilen);
    if (content == NULL || index == NULL) {
        fprintf(stderr, "read failed\n");
        return 2;
    }
    unsigned long eid = strtoul(argv[3], NULL, 10);

    if (rd32(content) != 0x434B5750u /* 'PWKC' */) {
        fprintf(stderr, "bad content magic\n");
        return 2;
    }
    if (content[4] != 3) {
        fprintf(stderr, "unsupported version %u\n", content[4]);
        return 2;
    }
    uint32_t payload_size = rd32(content + 12);
    uint32_t dict_offset = rd32(content + 16);
    uint32_t dict_len = rd32(content + 20);
    if (dict_len > payload_size || dict_len > DEFLATE_RING ||
            (dict_len && dict_offset != payload_size - dict_len)) {
        fprintf(stderr, "bad dict placement\n");
        return 2;
    }
    const uint8_t *dict = dict_len ? content + 32 + dict_offset : NULL;

    /* entry: id(4) content_offset(4) comp_len(4) uncomp_len(4) crc32(4) ... */
    const uint8_t *ent = index + 32 + eid * 40;
    uint32_t coff = rd32(ent + 4);
    uint32_t cl = rd32(ent + 8);
    uint32_t ul = rd32(ent + 12);
    uint32_t crc = rd32(ent + 16);
    const uint8_t *frame = content + 32 + coff;

    int rc = decode_deflate(frame, cl, ul, crc, dict, dict_len);
    free(content);
    free(index);
    return rc;
}
