#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_err.h"

#ifndef PW_PACK_DIR
#define PW_PACK_DIR "/packs"
#endif
#define PW_PACK_NAME_MAX 40
#define PW_PACK_UPLOAD_RESERVE 8192
/* The library every device carries in flash: the PocketWiki guide. It cannot
 * be removed, and an installed pack of the same name (an update) is served in
 * its place. */
#define PW_BUILTIN_PACK_NAME "pocketwiki"

typedef struct {
    char name[PW_PACK_NAME_MAX + 1];
    uint32_t articles;
    uint32_t bytes;
    bool enabled;
} pack_store_item_t;

esp_err_t pack_store_init(void);
/* Revalidate installed-pack metadata after external filesystem changes or for
 * diagnostics. Normal reads validate lazily and cache up to 32 pack records;
 * larger libraries fall back to scanning without imposing a pack limit.
 * This is NOT a validation bypass for ca_load_pack() or an archive read lock. */
esp_err_t pack_store_rescan(void);
int pack_store_list(pack_store_item_t *items, size_t cap);
/* Allocate and return every valid installed pack. Caller owns *items and must
 * free it. There is no pack-count ceiling beyond the flash filesystem. */
int pack_store_list_all(pack_store_item_t **items);
int pack_store_count(void);
uint32_t pack_store_total_articles(void);
/* Archives are opened serially to keep RAM bounded. Every installed pack is
 * enabled; this only selects the archive needed for the current request. */
esp_err_t pack_store_open_for_read(const char *name);
esp_err_t pack_store_restore_builtin(void);

/* The built-in library can be taken out of the reader's listings without
 * changing what can be read: articles stay reachable by link or id, and the
 * manage page always shows it. The state is a marker file in the pack store,
 * so it survives a reboot. */
bool pack_store_guide_hidden(void);
esp_err_t pack_store_set_guide_hidden(bool hidden);
esp_err_t pack_store_remove(const char *name);
esp_err_t pack_store_begin_upload(const char *name, FILE **out, char *temp_path, size_t cap);
esp_err_t pack_store_finish_upload(FILE *fh, const char *temp_path, const char *name,
                                   uint32_t *articles);
void pack_store_abort_upload(FILE *fh, const char *temp_path);
bool pack_store_safe_name(const char *name);
void pack_store_usage(size_t *total, size_t *used);
size_t pack_store_installable_bytes(void);
