/* Streaming catalogue reader.
 *
 * The pack catalogue is JSON, and it grows with the catalogue: the device must
 * read it without holding either the text or a parsed tree, and without a fixed
 * limit on how many packs a release may carry. The ESP32-C3 cannot afford the
 * document plus a node per field, and cannot afford an entry per pack either,
 * so this reader keeps exactly one pack entry in fixed storage and answers two
 * questions in a single pass:
 *
 *   - pw_catalog_find:  the pack with this id or url, for install verification
 *                       and for one pack's display name;
 *   - pw_catalog_names: the display names of several installed packs at once,
 *                       so a dashboard render costs one pass, not one per pack.
 *
 * Nothing here allocates. Field caps are compile-time constants, and a value
 * that does not fit its cap is an error rather than a truncation.
 *
 * The active catalogue is the synced file when it is newer than the catalogue
 * embedded in the firmware image, by catalog_version and then generated_at.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PW_CATALOG_ID_CAP 48
#define PW_CATALOG_NAME_CAP 48
#define PW_CATALOG_URL_CAP 96
#define PW_CATALOG_SHA_CAP 65
#define PW_CATALOG_STAMP_CAP 32

typedef struct {
    char id[PW_CATALOG_ID_CAP];
    char name[PW_CATALOG_NAME_CAP];
    char url[PW_CATALOG_URL_CAP];
    char sha256[PW_CATALOG_SHA_CAP];
    int64_t bytes;
    int version;
    int articles;
    bool complete;          /* id, url, sha256 and bytes were all present */
} pw_catalog_pack_t;

typedef struct {
    bool ok;                                     /* schema 1, fields readable */
    int version;
    char generated_at[PW_CATALOG_STAMP_CAP];
} pw_catalog_header_t;

/* One display name to fill: `id` names the pack, `out` receives its name and is
 * left untouched when the catalogue has no such pack. */
typedef struct {
    const char *id;
    char *out;
    size_t cap;
} pw_catalog_name_slot_t;

/* Byte source: returns bytes read (0 at end of document) or -1 on error. */
typedef int (*pw_catalog_read_fn)(void *ctx, char *buf, size_t cap);

/* Read only schema/catalog_version/generated_at, stopping at the packs array. */
bool pw_catalog_read_header(pw_catalog_read_fn read_fn, void *ctx, pw_catalog_header_t *out);

/* Find one pack by id or by url (either may be NULL, at least one must be set).
 * Returns false when the document is unusable or the pack is absent; a document
 * that is not schema 1, has no packs array, or holds an entry missing a required
 * field is unusable, and *out is then unspecified. */
bool pw_catalog_find(pw_catalog_read_fn read_fn, void *ctx,
                     const char *id, const char *url, pw_catalog_pack_t *out);

/* Fill display names for up to `count` packs in one pass. Returns the number of
 * names filled; a document that is unusable fills none. */
int pw_catalog_names(pw_catalog_read_fn read_fn, void *ctx,
                     pw_catalog_name_slot_t *slots, int count);

/* Validate a document without keeping it: schema 1, a packs array, and at least
 * one complete entry. Used on a catalogue the device has just downloaded, so a
 * malformed release is rejected before it replaces the working copy. */
bool pw_catalog_validate(pw_catalog_read_fn read_fn, void *ctx);

#ifdef __cplusplus
}
#endif
