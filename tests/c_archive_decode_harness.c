/* Host harness for the firmware's per-article decode paths.
 *
 * Mirrors serve_article() in firmware/main/web_server.c for both archive
 * generations:
 *   - v2: zstd frames decoded with the pack's shared dictionary through
 *     ZSTD_decompressStream (bounded in/out chunks, the same end conditions);
 *   - v1 (legacy): gzip streams inflated with miniz after a parsed gzip
 *     header, integrity-checked against the index CRC.
 * Links the same libraries the firmware compiles: libzstd from
 * managed_components/rderr__esp-idf-zstd and vendored third_party/miniz.
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
#define ZSTD_STATIC_LINKING_ONLY
#include "zstd.h"

/* Keep these declarations visible when the host's surrounding headers hide
 * optional miniz allocator prototypes behind MINIZ_NO_MALLOC. */
extern tinfl_decompressor *tinfl_decompressor_alloc(void);
extern void tinfl_decompressor_free(tinfl_decompressor *pDecomp);

#define CHUNK 4096

static uint8_t *read_file(const char *path, size_t *out_len)
{
    FILE *fh = fopen(path, "rb");
    if (fh == NULL) return NULL;
    fseek(fh, 0, SEEK_END);
    long sz = ftell(fh);
    fseek(fh, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)sz);
    if (buf == NULL || fread(buf, 1, (size_t)sz, fh) != (size_t)sz) {
        fclose(fh);
        free(buf);
        return NULL;
    }
    fclose(fh);
    *out_len = (size_t)sz;
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

/* v2: the serve_article() zstd loop, verbatim in structure. Mirrors the
 * firmware's per-request dict attach (raw blob + ZSTD_DCtx_loadDictionary). */
static int decode_zstd(const uint8_t *frame, uint32_t cl, uint32_t ul,
                       uint32_t expected_crc, const uint8_t *dict, size_t dict_len)
{
    ZSTD_DStream *ds = ZSTD_createDStream();
    if (ds == NULL) {
        fprintf(stderr, "ZSTD_createDStream failed\n");
        return 1;
    }
    size_t rc = ZSTD_DCtx_reset(ds, ZSTD_reset_session_and_parameters);
    if (!ZSTD_isError(rc)) {
        /* Mirror stream_article_zstd() in firmware/main/web_server.c. This
         * experimental limit rejects frames with blocks larger than 1 KiB;
         * omitting it would make this host test weaker than the device. */
        rc = ZSTD_DCtx_setParameter(ds, ZSTD_d_maxBlockSize, 1024);
    }
    if (!ZSTD_isError(rc) && dict_len > 0) {
        rc = ZSTD_DCtx_loadDictionary(ds, dict, dict_len);
    }
    if (ZSTD_isError(rc)) {
        fprintf(stderr, "init: %s\n", ZSTD_getErrorName(rc));
        return 1;
    }

    uint8_t in[CHUNK], out[CHUNK];
    ZSTD_inBuffer ib = { in, 0, 0 };
    uint32_t sent_in = 0;
    uint32_t total_out = 0;
    uint32_t output_crc = 0;
    uint32_t frame_pos = 0;
    bool frame_done = false;

    for (;;) {
        if (ib.pos == ib.size) {
            if (sent_in >= cl) {
                break;                  /* input exhausted before frame end */
            }
            size_t n = cl - sent_in;
            if (n > sizeof in) n = sizeof in;
            memcpy(in, frame + frame_pos, n);
            frame_pos += (uint32_t)n;
            ib.src = in;
            ib.size = n;
            ib.pos = 0;
            sent_in += (uint32_t)n;
        }
        ZSTD_outBuffer ob = { out, sizeof out, 0 };
        rc = ZSTD_decompressStream(ds, &ob, &ib);
        if (ZSTD_isError(rc)) {
            fprintf(stderr, "decode: %s\n", ZSTD_getErrorName(rc));
            return 1;
        }
        if (ob.pos > 0) {
            total_out += (uint32_t)ob.pos;
            if (total_out > ul) {
                fprintf(stderr, "decode exceeds declared uncomp_len\n");
                return 1;
            }
            output_crc = crc32_buf(output_crc, out, ob.pos);
            fwrite(out, 1, ob.pos, stdout);
        }
        if (rc == 0) {
            frame_done = true;
            break;                      /* frame fully decoded */
        }
    }
    ZSTD_freeDStream(ds);
    if (!frame_done || sent_in != cl || ib.pos != ib.size) {
        fprintf(stderr, "truncated or trailing zstd input\n");
        return 1;
    }
    if (total_out != ul) {
        fprintf(stderr, "truncated decode %u != %u\n", total_out, ul);
        return 1;
    }
    if (output_crc != expected_crc) {
        fprintf(stderr, "zstd length/CRC mismatch\n");
        return 1;
    }
    return 0;
}

/* v1: parse the gzip header, inflate with miniz, verify the index CRC. */
static int decode_gzip(const uint8_t *stream, uint32_t clen, uint32_t ul,
                       uint32_t expected_crc)
{
    if (clen < 18 || stream[0] != 0x1f || stream[1] != 0x8b || stream[2] != 8) {
        fprintf(stderr, "not a gzip stream\n");
        return 1;
    }
    uint8_t flg = stream[3];
    size_t pos = 10;
    if (flg & 0x04) {
        if (pos + 2 > clen) { fprintf(stderr, "gzip header overrun\n"); return 1; }
        size_t xlen = (size_t)stream[pos] | ((size_t)stream[pos + 1] << 8);
        pos += 2 + xlen;
    }
    if (flg & 0x08) {
        while (pos < clen && stream[pos] != 0) pos++;
        pos++;
    }
    if (flg & 0x10) {
        while (pos < clen && stream[pos] != 0) pos++;
        pos++;
    }
    if (flg & 0x02) pos += 2;
    if (pos >= clen) {
        fprintf(stderr, "gzip header overrun\n");
        return 1;
    }

    /* Match firmware: bounded output with a guard region, and the raw
     * DEFLATE body only. Decoder state is heap-backed here because this host
     * harness is not an HTTP task; firmware keeps the same state static. */
    tinfl_decompressor *decomp = tinfl_decompressor_alloc();
    const size_t guard_len = 64;
    size_t out_capacity = (size_t)ul + guard_len;
    void *out = malloc(out_capacity);
    size_t out_len = out_capacity;
    if (out != NULL) memset((uint8_t *)out + ul, 0xA5, guard_len);
    size_t deflate_len = clen - pos - 8;
    tinfl_status status = decomp != NULL && out != NULL
        ? tinfl_decompress(decomp, stream + pos, &deflate_len, out, out, &out_len,
                           TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF)
        : TINFL_STATUS_FAILED;
    tinfl_decompressor_free(decomp);
    if (status != TINFL_STATUS_DONE) {
        fprintf(stderr, "gzip inflate failed (%d)\n", (int)status);
        free(out);
        return 1;
    }
    bool guard_ok = true;
    for (size_t i = 0; i < guard_len; i++) {
        if (((uint8_t *)out)[ul + i] != 0xA5) {
            guard_ok = false;
            break;
        }
    }
    if (out_len != ul || !guard_ok || crc32_buf(0, out, out_len) != expected_crc) {
        fprintf(stderr, "gzip length/CRC mismatch\n");
        free(out);
        return 1;
    }
    fwrite(out, 1, out_len, stdout);
    free(out);
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
    uint8_t ver = content[4];
    uint32_t header_size;
    const uint8_t *dict = NULL;
    uint32_t dict_len = 0;
    if (ver == 1) {
        header_size = 16;
    } else if (ver == 2) {
        header_size = 32;
        uint32_t payload_size = rd32(content + 12);
        uint32_t dict_offset = rd32(content + 16);
        dict_len = rd32(content + 20);
        if (dict_len > payload_size ||
                (dict_len && dict_offset != payload_size - dict_len)) {
            fprintf(stderr, "bad dict placement\n");
            return 2;
        }
        dict = dict_len ? content + header_size + dict_offset : NULL;
    } else {
        fprintf(stderr, "unsupported version %u\n", ver);
        return 2;
    }

    /* entry: id(4) content_offset(4) comp_len(4) uncomp_len(4) crc32(4) ... */
    const uint8_t *ent = index + 32 + eid * 40;
    uint32_t coff = rd32(ent + 4);
    uint32_t cl = rd32(ent + 8);
    uint32_t ul = rd32(ent + 12);
    uint32_t crc = rd32(ent + 16);
    const uint8_t *frame = content + header_size + coff;

    int rc = (ver == 1) ? decode_gzip(frame, cl, ul, crc)
                        : decode_zstd(frame, cl, ul, crc, dict, dict_len);
    free(content);
    free(index);
    return rc;
}
