/* PocketWiki title normalization + lookup.
 *
 * Pure C, no ESP-IDF dependencies: the flash is accessed through a reader
 * callback, so this file compiles unchanged on the host for cross-language
 * tests against the Python packer.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Read len bytes at absolute offset into buf. Return 0 on success, -1 on
 * out-of-range or read error. */
typedef int (*tl_read_fn)(void *ctx, uint32_t offset, uint8_t *buf, size_t len);

/* A sorted index over fixed-width entries. See docs/ARCHIVE_FORMAT.md. */
typedef struct {
    uint32_t entries_off;   /* absolute offset of the first entry */
    uint32_t strings_off;   /* absolute offset of the title string table */
    uint32_t count;
    uint32_t entry_size;    /* must be 40 */
    tl_read_fn read;
    void *ctx;
} tl_index_t;

/* Entry field offsets (archive format v1). */
#define TL_ENTRY_OFF_ID          0
#define TL_ENTRY_OFF_CONTENT     4
#define TL_ENTRY_OFF_COMP_LEN    8
#define TL_ENTRY_OFF_UNCOMP_LEN  12
#define TL_ENTRY_OFF_CRC         16
#define TL_ENTRY_OFF_NORM_OFF    20
#define TL_ENTRY_OFF_NORM_LEN    24   /* u16 */
#define TL_ENTRY_OFF_DISP_OFF    26
#define TL_ENTRY_OFF_DISP_LEN    30   /* u16 */
#define TL_ENTRY_SIZE            40

/* Normalize a title to its canonical search form.
 *
 * Byte-exact contract shared with tools/archive_format.py:
 *   1. trim ASCII whitespace (0x09..0x0D, 0x20) on both ends
 *   2. collapse runs of ASCII whitespace to a single 0x20
 *   3. ASCII case-fold A-Z -> a-z
 * Non-ASCII bytes pass through unchanged. `out` must hold at least in_len
 * bytes; returns the normalized length. */
size_t tl_normalize(const uint8_t *in, size_t in_len, uint8_t *out);

/* Case-insensitive exact match. Returns 0 and sets *out_id (== entry index)
 * on success, -1 if not found or on read error. */
int tl_exact(const tl_index_t *ix, const uint8_t *norm, size_t norm_len,
             uint32_t *out_id);

/* Prefix match over the title-sorted index. Fills out_ids with at most
 * max_ids entries (entry indices). Returns 0 with *out_count set (possibly
 * 0), or -1 on read error. */
int tl_prefix(const tl_index_t *ix, const uint8_t *norm, size_t norm_len,
              uint32_t *out_ids, int max_ids, int *out_count);

/* Display title of entry id (UTF-8, not NUL-terminated). Returns 0 and sets
 * *out_len on success; -1 on bounds error or if longer than cap. */
int tl_read_display_title(const tl_index_t *ix, uint32_t id, uint8_t *buf,
                          size_t cap, uint32_t *out_len);

#ifdef __cplusplus
}
#endif
