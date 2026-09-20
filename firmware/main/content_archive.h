/* PocketWiki content archive: read-only access to the content/index data.
 *
 * The archive can be served from two interchangeable sources:
 *   - the built-in flash partitions ("content" / "index"), always available;
 *   - a portable pack file stored in the internal flash pack partition.
 * All offsets and lengths are bounds-checked against the validated header
 * before any read. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PW_CONTENT_MAGIC       0x434B5750u   /* 'PWKC' little-endian */
#define PW_INDEX_MAGIC         0x494B5750u   /* 'PWKI' little-endian */
#define PW_PACK_MAGIC          0x504B5750u   /* 'PWKP' little-endian */
#define PW_FORMAT_VERSION      3u            /* raw DEFLATE frames + trained dict */
#define PW_CONTENT_HEADER_SIZE 32u
#define PW_INDEX_HEADER_SIZE   32u
#define PW_PACK_HEADER_SIZE    32u
#define PW_MAX_DICT_BYTES      32768u

/* The pack's trained dictionary is stored at the payload tail
 * (dict_offset + dict_len == payload_size) and is never larger than the 32 KiB
 * inflate ring the server loads it into before each article, so a frame's
 * back-references can always be satisfied. */

/* A random-access byte source (flash partition or file region). */
typedef struct {
    esp_err_t (*read)(void *ctx, uint32_t off, uint8_t *buf, size_t len);
    void *ctx;
    uint32_t size;              /* total bytes readable */
} pw_archive_source_t;

typedef struct {
    pw_archive_source_t content;   /* content.bin (header + payload) */
    pw_archive_source_t index;     /* index.bin */
    uint32_t payload_size;      /* bytes of article data in content payload */
    uint32_t dict_offset;       /* payload-relative offset of the trained dict */
    uint32_t dict_len;          /* dict blob size; 0 when the pack has none */
    uint32_t count;             /* article count */
    uint32_t entries_off;       /* entry array offset within index */
    uint32_t strings_off;       /* title string table offset */
    uint32_t index_size;        /* validated index size */
    uint8_t format_version;     /* PW_FORMAT_VERSION */
    bool valid;                 /* archive validated successfully */
    bool from_file;             /* true when served from a pack file */
} content_archive_t;

/* Parsed article metadata (from one index entry). */
typedef struct {
    uint32_t id;
    uint32_t content_offset;    /* offset of the article's frame within the
                                 * content payload */
    uint32_t comp_len;
    uint32_t uncomp_len;
    uint32_t crc32;
} pw_article_meta_t;

/* Validate the built-in flash partitions and make them the active archive.
 * Non-fatal on error: .valid is set false and every accessor fails cleanly. */
esp_err_t ca_init(void);

/* Load a portable pack file (content.bin + index.bin wrapped) and make it the
 * active archive. On success the file stays open for the archive's lifetime;
 * on failure the previously active source is left untouched. */
esp_err_t ca_load_pack(const char *path);

/* Validate a portable pack without changing the active archive. */
esp_err_t ca_probe_pack(const char *path, uint32_t *article_count);

/* Why the most recent pack validation was refused, in words a user can act on
 * ("not a valid pack" when nothing more specific is known). Set by ca_init(),
 * ca_probe_pack() and ca_load_pack(). */
const char *ca_last_reject_reason(void);

/* Revert to the built-in flash archive. */
void ca_revert_to_flash(void);

const content_archive_t *ca_get(void);

/* Monotonically changes whenever the active archive source changes. Decoder
 * users cache this value to know when a dictionary must be rebound. */
uint32_t ca_generation(void);

/* Bounds-checked article metadata lookup by id. ESP_OK or
 * ESP_ERR_NOT_FOUND / ESP_ERR_INVALID_STATE / ESP_ERR_INVALID_ARG. */
esp_err_t ca_get_article(uint32_t id, pw_article_meta_t *out);

/* Bounds-checked read of len bytes at payload offset. */
esp_err_t ca_read_payload(uint32_t payload_offset, uint8_t *buf, size_t len);

/* Reader callback for title_lookup (absolute offsets within the index file). */
int ca_index_read(void *ctx, uint32_t offset, uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif
