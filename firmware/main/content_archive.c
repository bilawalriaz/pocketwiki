#include "content_archive.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"
#include "title_lookup.h"
#include "web_server.h"
#include "zstd.h"

static const char *TAG = "archive";

static content_archive_t s_ca;
static uint32_t s_generation;

/* ---- flash source ------------------------------------------------------- */

static esp_err_t flash_read(void *ctx, uint32_t off, uint8_t *buf, size_t len)
{
    const esp_partition_t *part = (const esp_partition_t *)ctx;
    return esp_partition_read(part, off, buf, len);
}

/* ---- file source (portable pack in the internal pack store) ------------ */

typedef struct {
    FILE *fh;
    uint32_t base;              /* absolute offset of this region within the pack */
} pw_file_ctx_t;

static esp_err_t file_read(void *ctx, uint32_t off, uint8_t *buf, size_t len)
{
    const pw_file_ctx_t *fc = (const pw_file_ctx_t *)ctx;
    if (fseek(fc->fh, (long)(fc->base + off), SEEK_SET) != 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (fread(buf, 1, len, fc->fh) != len) {
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t rd16(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static esp_err_t read_exact(const pw_archive_source_t *src, uint32_t off,
                            uint8_t *buf, size_t len)
{
    if (off > src->size || len > (size_t)(src->size - off)) {
        return ESP_ERR_INVALID_ARG;
    }
    return src->read(src->ctx, off, buf, len);
}

/* Validate the content header (magic, version, sizes, dict placement).
 *
 * Accepts both generations: v2 (32-byte header, zstd frames, optional
 * dictionary at the payload tail) and v1 (16-byte header, gzip frames, no
 * dictionary). The active format_version tells the server which decoder to
 * use, so packs built before the zstd cutover keep working after a firmware
 * upgrade — and a rollback to v1 packs never requires a firmware reflash. */
static esp_err_t validate_content(const pw_archive_source_t *src, uint32_t *payload_size,
                                  uint32_t *dict_offset, uint32_t *dict_len,
                                  uint8_t *format_version)
{
    uint8_t hdr[PW_CONTENT_HEADER_SIZE];
    ESP_RETURN_ON_ERROR(read_exact(src, 0, hdr, PW_CONTENT_HEADER_SIZE_LEGACY),
                        TAG, "content header read");

    uint32_t magic = rd32(hdr);
    if (magic != PW_CONTENT_MAGIC) {
        ESP_LOGE(TAG, "content: bad magic 0x%08lx", (unsigned long)magic);
        return ESP_ERR_INVALID_VERSION;
    }
    uint8_t ver = hdr[4];
    if (ver != PW_FORMAT_VERSION && ver != PW_FORMAT_VERSION_LEGACY) {
        ESP_LOGE(TAG, "content: unsupported version %u", ver);
        return ESP_ERR_INVALID_VERSION;
    }
    uint32_t hsize = rd32(hdr + 8);
    uint32_t ps, doff = 0, dlen = 0;
    if (ver == PW_FORMAT_VERSION_LEGACY) {
        if (hsize != PW_CONTENT_HEADER_SIZE_LEGACY) {
            ESP_LOGE(TAG, "content: bad header size %lu", (unsigned long)hsize);
            return ESP_ERR_INVALID_VERSION;
        }
        ps = rd32(hdr + 12);
    } else {
        ESP_RETURN_ON_ERROR(read_exact(src, 0, hdr, sizeof hdr), TAG, "content header read");
        if (hsize != PW_CONTENT_HEADER_SIZE) {
            ESP_LOGE(TAG, "content: bad header size %lu", (unsigned long)hsize);
            return ESP_ERR_INVALID_VERSION;
        }
        ps = rd32(hdr + 12);
        doff = rd32(hdr + 16);
        dlen = rd32(hdr + 20);
    }
    if (hsize > src->size || ps > src->size - hsize) {
        ESP_LOGE(TAG, "content: payload %lu > source %lu",
                 (unsigned long)ps, (unsigned long)src->size);
        return ESP_ERR_INVALID_SIZE;
    }
    if (dlen > ps) {
        ESP_LOGE(TAG, "content: dict %lu > payload %lu", (unsigned long)dlen, (unsigned long)ps);
        return ESP_ERR_INVALID_SIZE;
    }
    if (dlen > PW_MAX_DICT_BYTES) {
        ESP_LOGE(TAG, "content: dict %lu exceeds %u-byte limit",
                 (unsigned long)dlen, (unsigned)PW_MAX_DICT_BYTES);
        return ESP_ERR_INVALID_SIZE;
    }
    if (dlen == 0) {
        if (doff != 0) {
            ESP_LOGE(TAG, "content: dict_offset nonzero but dict_len 0");
            return ESP_ERR_INVALID_SIZE;
        }
    } else if (doff != ps - dlen) {
        /* dictionary is stored at the end of the payload (tail invariant) */
        ESP_LOGE(TAG, "content: dict not at payload tail");
        return ESP_ERR_INVALID_SIZE;
    }
    *payload_size = ps;
    *dict_offset = doff;
    *dict_len = dlen;
    *format_version = ver;
    return ESP_OK;
}

/* Read the dict blob from the payload tail into a caller-owned buffer.
 * Transient memory is exactly one dict_size (no second allocation), which is
 * what keeps archive re-init viable on the C3's ~100 KiB free heap. */
static esp_err_t load_dict(const pw_archive_source_t *src, uint32_t dict_offset,
                           uint32_t dict_len, uint8_t **out)
{
    *out = NULL;
    if (dict_len == 0) return ESP_OK;
    uint8_t *blob = malloc(dict_len);
    if (blob == NULL) {
        ESP_LOGE(TAG, "dict: alloc %lu B failed", (unsigned long)dict_len);
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = read_exact(src, PW_CONTENT_HEADER_SIZE + dict_offset, blob, dict_len);
    if (err != ESP_OK) {
        free(blob);
        return err;
    }
    *out = blob;
    return ESP_OK;
}

/* CRC32 of index bytes [32, index_size), streamed from the source. */
static esp_err_t index_body_crc(const pw_archive_source_t *src, uint32_t index_size,
                                uint32_t *out_crc)
{
    uint8_t buf[512];
    uint32_t crc = 0;
    uint32_t off = PW_INDEX_HEADER_SIZE;
    while (off < index_size) {
        size_t n = index_size - off;
        if (n > sizeof buf) n = sizeof buf;
        ESP_RETURN_ON_ERROR(read_exact(src, off, buf, n), TAG, "index body read");
        crc = esp_rom_crc32_le(crc, buf, n);
        off += (uint32_t)n;
    }
    *out_crc = crc;
    return ESP_OK;
}

/* Validate the index: header fields, whole-file CRC, per-entry bounds.
 * The index version must match the content version (v1 packs carry v1
 * indexes; v2 packs carry v2 indexes). */
static esp_err_t validate_index(const pw_archive_source_t *src, uint32_t *count,
                                uint32_t *entries_off, uint32_t *strings_off,
                                uint32_t *index_size, uint32_t payload_size,
                                uint8_t content_version)
{
    uint8_t hdr[PW_INDEX_HEADER_SIZE];
    ESP_RETURN_ON_ERROR(read_exact(src, 0, hdr, sizeof hdr), TAG, "index header read");

    if (rd32(hdr) != PW_INDEX_MAGIC) {
        ESP_LOGE(TAG, "index: bad magic");
        return ESP_ERR_INVALID_VERSION;
    }
    if (hdr[4] != content_version) {
        ESP_LOGE(TAG, "index: version %u != content version %u", hdr[4], content_version);
        return ESP_ERR_INVALID_VERSION;
    }
    if (hdr[5] != TL_ENTRY_SIZE) {
        ESP_LOGE(TAG, "index: entry_size %u != %d", hdr[5], TL_ENTRY_SIZE);
        return ESP_ERR_INVALID_VERSION;
    }
    uint32_t cnt = rd32(hdr + 8);
    uint32_t eoff = rd32(hdr + 12);
    uint32_t soff = rd32(hdr + 16);
    uint32_t isize = rd32(hdr + 20);
    uint32_t crc_stored = rd32(hdr + 24);

    if (eoff != PW_INDEX_HEADER_SIZE || isize > src->size ||
            isize < PW_INDEX_HEADER_SIZE || soff > isize ||
            cnt > (isize - PW_INDEX_HEADER_SIZE) / TL_ENTRY_SIZE) {
        ESP_LOGE(TAG, "index: inconsistent layout (cnt=%lu isize=%lu soff=%lu)",
                 (unsigned long)cnt, (unsigned long)isize, (unsigned long)soff);
        return ESP_ERR_INVALID_SIZE;
    }
    if (soff < PW_INDEX_HEADER_SIZE + cnt * TL_ENTRY_SIZE) {
        ESP_LOGE(TAG, "index: strings overlap entries");
        return ESP_ERR_INVALID_SIZE;
    }

    uint32_t crc_calc;
    ESP_RETURN_ON_ERROR(index_body_crc(src, isize, &crc_calc), TAG, "index crc");
    if (crc_calc != crc_stored) {
        ESP_LOGE(TAG, "index: CRC mismatch (stored %08lx, computed %08lx)",
                 (unsigned long)crc_stored, (unsigned long)crc_calc);
        return ESP_ERR_INVALID_CRC;
    }

    /* Per-entry bounds + sequential ids. */
    uint8_t ent[TL_ENTRY_SIZE];
    for (uint32_t i = 0; i < cnt; i++) {
        ESP_RETURN_ON_ERROR(read_exact(src, eoff + i * TL_ENTRY_SIZE, ent, sizeof ent),
                            TAG, "entry read");
        if (rd32(ent + TL_ENTRY_OFF_ID) != i) {
            ESP_LOGE(TAG, "index entry %lu: non-sequential id", (unsigned long)i);
            return ESP_ERR_INVALID_SIZE;
        }
        uint32_t coff = rd32(ent + TL_ENTRY_OFF_CONTENT);
        uint32_t clen = rd32(ent + TL_ENTRY_OFF_COMP_LEN);
        uint32_t ulen = rd32(ent + TL_ENTRY_OFF_UNCOMP_LEN);
        if (clen == 0 || ulen == 0 || coff > payload_size || clen > payload_size - coff) {
            ESP_LOGE(TAG, "index entry %lu: content range out of bounds", (unsigned long)i);
            return ESP_ERR_INVALID_SIZE;
        }
        uint32_t noff = rd32(ent + TL_ENTRY_OFF_NORM_OFF);
        uint32_t nlen = rd16(ent + TL_ENTRY_OFF_NORM_LEN);
        uint32_t doff = rd32(ent + TL_ENTRY_OFF_DISP_OFF);
        uint32_t dlen = rd16(ent + TL_ENTRY_OFF_DISP_LEN);
        if (nlen == 0 || dlen == 0 || noff > isize || nlen > isize - noff ||
                doff > isize || dlen > isize - doff) {
            ESP_LOGE(TAG, "index entry %lu: title range out of bounds", (unsigned long)i);
            return ESP_ERR_INVALID_SIZE;
        }
    }

    *count = cnt;
    *entries_off = eoff;
    *strings_off = soff;
    *index_size = isize;
    return ESP_OK;
}

esp_err_t ca_init(void)
{
    memset(&s_ca, 0, sizeof s_ca);

    const esp_partition_t *content =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "content");
    const esp_partition_t *index =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x41, "index");
    if (content == NULL || index == NULL) {
        ESP_LOGE(TAG, "content/index partitions not found in partition table");
        return ESP_ERR_NOT_FOUND;
    }

    pw_archive_source_t csrc = {
        .read = flash_read,
        .ctx = (void *)content,
        .size = content->size,
    };
    pw_archive_source_t isrc = {
        .read = flash_read,
        .ctx = (void *)index,
        .size = index->size,
    };

    uint32_t payload_size, dict_offset, dict_len;
    uint8_t format_version;
    uint32_t count, eoff, soff, isize;
    esp_err_t err = validate_content(&csrc, &payload_size, &dict_offset, &dict_len,
                                     &format_version);
    if (err != ESP_OK) return err;
    err = validate_index(&isrc, &count, &eoff, &soff, &isize, payload_size,
                         format_version);
    if (err != ESP_OK) return err;

    uint8_t *dict_blob = NULL;
    err = load_dict(&csrc, dict_offset, dict_len, &dict_blob);
    if (err != ESP_OK) return err;

    s_ca.content = csrc;
    s_ca.index = isrc;
    s_ca.payload_size = payload_size;
    s_ca.dict_offset = dict_offset;
    s_ca.dict_len = dict_len;
    s_ca.dict = dict_blob;
    s_ca.count = count;
    s_ca.entries_off = eoff;
    s_ca.strings_off = soff;
    s_ca.index_size = isize;
    s_ca.format_version = format_version;
    s_ca.valid = true;
    s_ca.from_file = false;
    s_generation++;

    ESP_LOGI(TAG, "archive OK v%u: %lu articles, payload %lu bytes (dict %lu), index %lu bytes",
             format_version, (unsigned long)count, (unsigned long)payload_size,
             (unsigned long)dict_len, (unsigned long)isize);
    return ESP_OK;
}

static FILE *s_pack_fh = NULL;
static pw_file_ctx_t s_pack_content_ctx;
static pw_file_ctx_t s_pack_index_ctx;

static esp_err_t probe_pack_file(FILE *fh, pw_file_ctx_t *content_ctx,
                                 pw_file_ctx_t *index_ctx, content_archive_t *out)
{
    uint8_t hdr[PW_PACK_HEADER_SIZE];
    if (fseek(fh, 0, SEEK_SET) != 0 || fread(hdr, 1, sizeof hdr, fh) != sizeof hdr) {
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t pack_ver = hdr[4];
    if (rd32(hdr) != PW_PACK_MAGIC ||
            (pack_ver != PW_FORMAT_VERSION && pack_ver != PW_FORMAT_VERSION_LEGACY)) {
        return ESP_ERR_INVALID_VERSION;
    }
    uint32_t content_len = rd32(hdr + 8);
    uint32_t index_len = rd32(hdr + 12);
    uint32_t min_content = (pack_ver == PW_FORMAT_VERSION)
        ? PW_CONTENT_HEADER_SIZE : PW_CONTENT_HEADER_SIZE_LEGACY;
    if (content_len < min_content || index_len < PW_INDEX_HEADER_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (fseek(fh, 0, SEEK_END) != 0) return ESP_ERR_INVALID_SIZE;
    long fsize = ftell(fh);
    uint64_t expected_size = (uint64_t)PW_PACK_HEADER_SIZE + content_len + index_len;
    if (fsize < 0 || expected_size > UINT32_MAX || (uint64_t)fsize != expected_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    /* Validate the portable wrapper before accepting either embedded archive.
     * This catches interrupted/corrupt transfers even when the inner headers
     * and index happen to remain parseable. */
    uint32_t stored_crc = rd32(hdr + 16);
    uint8_t buf[1024];
    uint32_t crc = 0;
    uint32_t remaining = content_len + index_len;
    uint32_t off = PW_PACK_HEADER_SIZE;
    while (remaining > 0) {
        size_t n = remaining > sizeof buf ? sizeof buf : remaining;
        if (fseek(fh, (long)off, SEEK_SET) != 0 || fread(buf, 1, n, fh) != n) {
            return ESP_ERR_INVALID_SIZE;
        }
        crc = esp_rom_crc32_le(crc, buf, n);
        off += (uint32_t)n;
        remaining -= (uint32_t)n;
    }
    if (crc != stored_crc) {
        ESP_LOGE(TAG, "pack: wrapper CRC mismatch (stored %08lx, computed %08lx)",
                 (unsigned long)stored_crc, (unsigned long)crc);
        return ESP_ERR_INVALID_CRC;
    }
    content_ctx->fh = fh;
    content_ctx->base = PW_PACK_HEADER_SIZE;
    index_ctx->fh = fh;
    index_ctx->base = PW_PACK_HEADER_SIZE + content_len;
    pw_archive_source_t csrc = { .read = file_read, .ctx = content_ctx, .size = content_len };
    pw_archive_source_t isrc = { .read = file_read, .ctx = index_ctx, .size = index_len };
    uint32_t payload_size, dict_offset, dict_len;
    uint8_t format_version;
    uint32_t count, eoff, soff, isize;
    ESP_RETURN_ON_ERROR(validate_content(&csrc, &payload_size, &dict_offset, &dict_len,
                                         &format_version),
                        TAG, "pack content");
    if (pack_ver != format_version) {
        ESP_LOGE(TAG, "pack: wrapper version %u != content version %u",
                 pack_ver, format_version);
        return ESP_ERR_INVALID_VERSION;
    }
    ESP_RETURN_ON_ERROR(validate_index(&isrc, &count, &eoff, &soff, &isize, payload_size,
                                       format_version),
                        TAG, "pack index");
    if (out != NULL) {
        *out = (content_archive_t) {
            .content = csrc, .index = isrc, .payload_size = payload_size,
            .dict_offset = dict_offset, .dict_len = dict_len,
            .count = count, .entries_off = eoff, .strings_off = soff,
            .index_size = isize, .format_version = format_version,
            .valid = true, .from_file = true,
        };
    }
    return ESP_OK;
}

esp_err_t ca_probe_pack(const char *path, uint32_t *article_count)
{
    FILE *fh = fopen(path, "rb");
    if (fh == NULL) return ESP_ERR_NOT_FOUND;
    pw_file_ctx_t cc = { 0 }, ic = { 0 };
    content_archive_t candidate = { 0 };
    esp_err_t err = probe_pack_file(fh, &cc, &ic, &candidate);
    if (err == ESP_OK && article_count != NULL) *article_count = candidate.count;
    fclose(fh);
    return err;
}

#if CONFIG_POCKETWIKI_PACK_INDEX_RAM_CACHE
/* RAM copy of the pack's index, so title lookup/search avoid flash VFS seeks. */
static uint8_t *s_index_ram = NULL;
static uint32_t s_index_ram_size = 0;

static esp_err_t mem_read(void *ctx, uint32_t off, uint8_t *buf, size_t len)
{
    const uint8_t *base = (const uint8_t *)ctx;
    if (off + len > s_index_ram_size) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(buf, base + off, len);
    return ESP_OK;
}
#endif

esp_err_t ca_load_pack(const char *path)
{
    FILE *fh = fopen(path, "rb");
    if (fh == NULL) {
        ESP_LOGE(TAG, "pack: cannot open %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    pw_file_ctx_t candidate_content_ctx = { 0 };
    pw_file_ctx_t candidate_index_ctx = { 0 };
    content_archive_t candidate = { 0 };
    esp_err_t err = probe_pack_file(fh, &candidate_content_ctx, &candidate_index_ctx, &candidate);
    if (err != ESP_OK) {
        fclose(fh);
        return err;
    }

    /* The web decoder may still hold a zstd window or a by-reference DDict.
     * Release those allocations before changing dictionary ownership. */
    web_server_archive_about_to_change();

    /* Drop the previous dictionary first so transient memory stays at one
     * dict_size, then load the new one. On failure the previous archive's
     * dictionary is restored so it keeps serving; if even that fails, fall
     * back to the built-in archive. */
    if (s_ca.dict != NULL) {
        free(s_ca.dict);
        s_ca.dict = NULL;
    }
    uint8_t *dict_blob = NULL;
    err = load_dict(&candidate.content, candidate.dict_offset, candidate.dict_len,
                    &dict_blob);
    if (err != ESP_OK) {
        fclose(fh);
        if (s_ca.valid && s_ca.dict_len > 0) {
            if (load_dict(&s_ca.content, s_ca.dict_offset, s_ca.dict_len,
                          &s_ca.dict) != ESP_OK) {
                ESP_LOGE(TAG, "pack: dict restore failed; reverting to built-in");
                ca_revert_to_flash();
            }
        }
        return err;
    }

    /* Swap the singleton: previous pack file (if any) is closed. */
    if (s_pack_fh != NULL) {
        fclose(s_pack_fh);
    }
    s_pack_fh = fh;
    s_pack_content_ctx = candidate_content_ctx;
    s_pack_index_ctx = candidate_index_ctx;
    candidate.content.ctx = &s_pack_content_ctx;
    candidate.index.ctx = &s_pack_index_ctx;
    candidate.dict = dict_blob;
    s_ca = candidate;
    s_generation++;

#if CONFIG_POCKETWIKI_PACK_INDEX_RAM_CACHE
    /* Cache the whole index in RAM so title lookup and search avoid repeated
     * filesystem reads. */
    free(s_index_ram);
    s_index_ram = NULL;
    s_index_ram_size = 0;
    uint32_t isize = s_ca.index_size;
    uint8_t *ram = malloc(isize);
    if (ram == NULL) {
        ESP_LOGW(TAG, "index cache alloc failed (%lu B); index reads stay on storage",
                 (unsigned long)isize);
    } else {
        /* read_exact uses src->read with src->ctx; read the full index via the
         * file source directly so bounds are checked against isize */
        if (s_ca.index.read(s_ca.index.ctx, 0, ram, isize) != ESP_OK) {
            ESP_LOGW(TAG, "index cache read failed; index reads stay on storage");
            free(ram);
        } else {
            s_index_ram = ram;
            s_index_ram_size = isize;
            s_ca.index.read = mem_read;
            s_ca.index.ctx = s_index_ram;
            ESP_LOGI(TAG, "index cached in RAM (%lu B)", (unsigned long)isize);
        }
    }
#endif

    ESP_LOGI(TAG, "pack loaded v%u: %lu articles, payload %lu bytes, index %lu bytes (%s)",
             s_ca.format_version, (unsigned long)s_ca.count, (unsigned long)s_ca.payload_size,
             (unsigned long)s_ca.index_size, path);
    return ESP_OK;
}

void ca_revert_to_flash(void)
{
    if (s_pack_fh != NULL) {
        fclose(s_pack_fh);
        s_pack_fh = NULL;
    }
    if (s_ca.dict != NULL) {
        free(s_ca.dict);
        s_ca.dict = NULL;
    }
#if CONFIG_POCKETWIKI_PACK_INDEX_RAM_CACHE
    free(s_index_ram);
    s_index_ram = NULL;
    s_index_ram_size = 0;
#endif
    ca_init();
}

const content_archive_t *ca_get(void)
{
    return &s_ca;
}

uint32_t ca_generation(void)
{
    return s_generation;
}

const uint8_t *ca_get_dict(uint32_t *len)
{
    if (len != NULL) *len = s_ca.dict_len;
    return s_ca.dict;
}

esp_err_t ca_get_article(uint32_t id, pw_article_meta_t *out)
{
    const content_archive_t *ca = &s_ca;
    if (!ca->valid) return ESP_ERR_INVALID_STATE;
    if (id >= ca->count) return ESP_ERR_NOT_FOUND;

    uint8_t ent[TL_ENTRY_SIZE];
    ESP_RETURN_ON_ERROR(read_exact(&ca->index, ca->entries_off + id * TL_ENTRY_SIZE,
                                   ent, sizeof ent),
                        TAG, "entry read");

    uint32_t coff = rd32(ent + TL_ENTRY_OFF_CONTENT);
    uint32_t clen = rd32(ent + TL_ENTRY_OFF_COMP_LEN);
    if (coff > ca->payload_size || clen > ca->payload_size - coff) {
        ESP_LOGE(TAG, "article %lu: corrupt content range", (unsigned long)id);
        return ESP_ERR_INVALID_SIZE;
    }
    out->id = rd32(ent + TL_ENTRY_OFF_ID);
    out->content_offset = coff;
    out->comp_len = clen;
    out->uncomp_len = rd32(ent + TL_ENTRY_OFF_UNCOMP_LEN);
    out->crc32 = rd32(ent + TL_ENTRY_OFF_CRC);
    return ESP_OK;
}

esp_err_t ca_read_payload(uint32_t payload_offset, uint8_t *buf, size_t len)
{
    const content_archive_t *ca = &s_ca;
    if (!ca->valid) return ESP_ERR_INVALID_STATE;
    if (payload_offset > ca->payload_size || len > (size_t)(ca->payload_size - payload_offset)) {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t header_size = ca->format_version == PW_FORMAT_VERSION_LEGACY
        ? PW_CONTENT_HEADER_SIZE_LEGACY : PW_CONTENT_HEADER_SIZE;
    return ca->content.read(ca->content.ctx, header_size + payload_offset,
                            buf, len);
}

int ca_index_read(void *ctx, uint32_t offset, uint8_t *buf, size_t len)
{
    const content_archive_t *ca = (const content_archive_t *)ctx;
    if (offset > ca->index_size || len > (size_t)(ca->index_size - offset)) {
        return -1;
    }
    return ca->index.read(ca->index.ctx, offset, buf, len) == ESP_OK ? 0 : -1;
}
