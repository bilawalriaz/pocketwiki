#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "content_archive.h"

static unsigned probes;
static int fail_rename;
static int fail_alloc;
static int test_rename(const char *from, const char *to)
{
    if (fail_rename && strstr(from, ".upload.tmp")) return -1;
    return rename(from, to);
}
static void *test_calloc(size_t n, size_t bytes)
{
    return fail_alloc ? NULL : calloc(n, bytes);
}
/* Keep the harness portable to libc implementations without strlcpy. */
static size_t test_strlcpy(char *out, const char *in, size_t cap)
{
    size_t n = strlen(in);
    if (cap) { size_t take = n < cap - 1 ? n : cap - 1;
        memcpy(out, in, take); out[take] = 0; }
    return n;
}
#define rename test_rename
#define calloc test_calloc
#define strlcpy test_strlcpy
#include "../firmware/main/pack_store.c"
#undef rename
#undef calloc
#undef strlcpy

static content_archive_t builtin = {.valid = true, .count = 100};
static char loaded_path[512];
static int load_ok;                 /* stub switch: does the installed file load? */
static uint32_t installed_articles = 7;
const content_archive_t *ca_get(void) { return &builtin; }
void ca_revert_to_flash(void) { builtin.from_file = false; }
esp_err_t ca_load_pack(const char *path)   /* matches content_archive.h */
{
    snprintf(loaded_path, sizeof loaded_path, "%s", path);
    if (!load_ok) return ESP_FAIL;
    builtin.from_file = true;
    builtin.count = installed_articles;
    return ESP_OK;
}
esp_err_t ca_probe_pack(const char *path, uint32_t *articles)
{
    probes++;
    FILE *f = fopen(path, "rb");
    uint32_t count = 0;
    if (!f) return ESP_FAIL;
    size_t got = fread(&count, sizeof count, 1, f);
    fclose(f);
    if (got != 1 || count == 0) return ESP_FAIL;
    if (articles) *articles = count;
    return ESP_OK;
}
static void put(const char *name, uint32_t count)
{
    char path[512];
    snprintf(path, sizeof path, PW_PACK_DIR "/%s.pwp", name);
    FILE *f = fopen(path, "wb"); assert(f);
    assert(fwrite(&count, sizeof count, 1, f) == 1);
    assert(fclose(f) == 0);
}
static int install(const char *name, uint32_t count)
{
    FILE *f; char temp[512]; uint32_t articles;
    assert(pack_store_begin_upload(name, &f, temp, sizeof temp) == ESP_OK);
    assert(fwrite(&count, sizeof count, 1, f) == 1);
    int err = pack_store_finish_upload(f, temp, name, &articles);
    if (err == ESP_OK) assert(articles == count);
    return err;
}
static void *snapshot_reader(void *unused)
{
    for (int i = 0; i < 100; i++) {
        pack_store_item_t *items = NULL;
        int n = pack_store_list_all(&items);
        assert(n == 1);
        assert(items[0].articles == 9 || items[0].articles == 10);
        free(items);
    }
    return NULL;
}
int main(void)
{
    assert(pack_store_init() == ESP_OK);
    put("alpha", 3); put("broken", 0);
    assert(pack_store_count() == 1);
    unsigned baseline = probes;
    assert(pack_store_total_articles() == 103);
    pack_store_item_t one;
    assert(pack_store_list(&one, 1) == 1 && one.articles == 3);
    pack_store_item_t *snapshot;
    assert(pack_store_list_all(&snapshot) == 1);
    assert(probes == baseline); /* no validation on any warm metadata API */
    fail_alloc = 1;
    pack_store_item_t *failed = (void *)1;
    assert(pack_store_list_all(&failed) == -1 && failed == NULL);
    fail_alloc = 0;
    assert(install("alpha", 9) == ESP_OK); /* same name and byte length */
    assert(pack_store_total_articles() == 109);
    assert(snapshot[0].articles == 3); /* caller owns an immutable snapshot */
    free(snapshot);
    baseline = probes;
    assert(install("alpha", 0) != ESP_OK);
    assert(pack_store_total_articles() == 109);
    assert(probes == baseline + 1); /* rejected staged pack leaves cache intact */
    fail_rename = 1;
    assert(install("alpha", 11) != ESP_OK);
    fail_rename = 0;
    assert(pack_store_total_articles() == 109); /* old file restored */
    pthread_t reader;
    assert(pthread_create(&reader, NULL, snapshot_reader, NULL) == 0);
    for (int i = 0; i < 10; i++) assert(install("alpha", 9 + i % 2) == ESP_OK);
    assert(pthread_join(reader, NULL) == 0);
    assert(pack_store_remove("alpha") == ESP_OK);
    assert(pack_store_count() == 0 && pack_store_total_articles() == 100);
    /* Explicit rescan is the trust boundary for out-of-band writes. */
    put("outside", 4);
    assert(pack_store_count() == 0);
    assert(pack_store_rescan() == ESP_OK);
    assert(pack_store_total_articles() == 104);
    assert(pack_store_remove("outside") == ESP_OK);
    for (int i = 0; i < 35; i++) {
        char name[32]; snprintf(name, sizeof name, "pack-%02d", i); put(name, 1);
    }
    assert(pack_store_rescan() == ESP_OK);
    assert(pack_store_count() == 35);
    assert(pack_store_total_articles() == 135);
    assert(pack_store_list_all(&snapshot) == 35); free(snapshot);
    assert(pack_store_list(&one, 1) == 1); /* overflow fallback is not truncation */
    assert(pack_store_list(NULL, 0) == 0);
    assert(pack_store_list(NULL, 1) == -1);

    /* --- the built-in library and its installed update --------------------
     * Every device carries one library that cannot be removed. Installing a
     * pack under that name updates it, and removing that file restores the
     * archive in flash. */
    assert(strcmp(PW_BUILTIN_PACK_NAME, "pocketwiki") == 0);
    assert(pack_store_safe_name(PW_BUILTIN_PACK_NAME));
    assert(!pack_store_safe_name("starter"));

    builtin.from_file = false;
    assert(pack_store_open_for_read(PW_BUILTIN_PACK_NAME) == ESP_OK);
    assert(!ca_get()->from_file);                     /* nothing installed yet */
    assert(pack_store_open_for_read("starter") == ESP_OK);   /* reserved alias */
    assert(!ca_get()->from_file);

    put(PW_BUILTIN_PACK_NAME, 7);
    assert(pack_store_rescan() == ESP_OK);
    assert(pack_store_count() == 35);                 /* not a library of its own */
    assert(pack_store_total_articles() == 135);
    pack_store_item_t *rows = NULL;
    int row_count = pack_store_list_all(&rows);
    assert(row_count == 35);
    for (int i = 0; i < row_count; i++) {
        assert(strcmp(rows[i].name, PW_BUILTIN_PACK_NAME) != 0);
    }
    free(rows);

    load_ok = 1;
    assert(pack_store_open_for_read(PW_BUILTIN_PACK_NAME) == ESP_OK);
    assert(ca_get()->from_file);                      /* the update is served */
    assert(strstr(loaded_path, PW_BUILTIN_PACK_NAME ".pwp") != NULL);
    assert(ca_get()->count == installed_articles);

    load_ok = 0;                                      /* an unusable update */
    builtin.from_file = false;
    assert(pack_store_open_for_read(PW_BUILTIN_PACK_NAME) == ESP_OK);
    assert(!ca_get()->from_file);                     /* falls back, never fails */

    assert(pack_store_remove(PW_BUILTIN_PACK_NAME) == ESP_OK);
    builtin.from_file = false;
    assert(pack_store_open_for_read(PW_BUILTIN_PACK_NAME) == ESP_OK);
    assert(!ca_get()->from_file);                     /* back to the built-in */
    assert(pack_store_remove(PW_BUILTIN_PACK_NAME) != ESP_OK);  /* nothing left */

    /* Hiding the built-in library only changes the reader's listing: it keeps
     * serving, and the choice outlives a reboot. */
    assert(!pack_store_guide_hidden());
    assert(pack_store_set_guide_hidden(true) == ESP_OK);
    assert(pack_store_guide_hidden());
    assert(pack_store_open_for_read(PW_BUILTIN_PACK_NAME) == ESP_OK);  /* still readable */
    assert(access("./packs/.guide-hidden", F_OK) == 0);
    assert(pack_store_set_guide_hidden(false) == ESP_OK);
    assert(!pack_store_guide_hidden());
    assert(access("./packs/.guide-hidden", F_OK) != 0);

    puts("metadata cache lifecycle passed");
}
