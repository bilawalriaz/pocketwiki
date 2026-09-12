#include "pack_store.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "content_archive.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "wear_levelling.h"
#include "web_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "packs";
static uint32_t s_builtin_articles;
static wl_handle_t s_wl_handle = WL_INVALID_HANDLE;
static char s_active_name[PW_PACK_NAME_MAX + 1];

/* Bounded metadata only: no files, indexes, or dictionaries are retained.
 * Larger libraries use the uncached path without hiding any installed packs.
 * This mutex protects metadata snapshots and final filesystem mutations; it
 * does NOT protect the active archive for the lifetime of an HTTP response. */
#define PW_METADATA_CACHE_ITEMS 32
static pack_store_item_t s_metadata[PW_METADATA_CACHE_ITEMS];
static int s_metadata_count;
static uint32_t s_metadata_articles;
static bool s_metadata_valid;
static SemaphoreHandle_t s_metadata_mutex;

static bool metadata_lock(void)
{
    return s_metadata_mutex != NULL &&
           xSemaphoreTake(s_metadata_mutex, portMAX_DELAY) == pdTRUE;
}

static void metadata_unlock(void)
{
    xSemaphoreGive(s_metadata_mutex);
}

bool pack_store_safe_name(const char *name)
{
    size_t n = name == NULL ? 0 : strlen(name);
    if (n < 1 || n > PW_PACK_NAME_MAX) return false;
    if (strcasecmp(name, PW_BUILTIN_PACK_NAME) == 0 || strcasecmp(name, "starter") == 0) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
    }
    return true;
}

static void path_for(const char *name, char *out, size_t cap)
{
    snprintf(out, cap, PW_PACK_DIR "/%s.pwp", name);
}

esp_err_t pack_store_init(void)
{
    if (s_metadata_mutex == NULL) s_metadata_mutex = xSemaphoreCreateMutex();
    if (s_metadata_mutex == NULL) return ESP_ERR_NO_MEM;
    s_metadata_valid = false;
    esp_vfs_fat_mount_config_t conf = {
        .format_if_mount_failed = true,
        .max_files = 8,
        .allocation_unit_size = 4096,
        .disk_status_check_enable = false,
        .use_one_fat = false,
    };
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(
        PW_PACK_DIR, "packs", &conf, &s_wl_handle);
    if (err != ESP_OK) return err;
    /* A power loss can leave the staging file behind. It is never a valid
     * installed pack, so reclaim it before reporting available space. */
    unlink(PW_PACK_DIR "/.upload.tmp");

    /* Installed packs are all enabled. Keep the recoverable built-in archive
     * selected between requests; handlers open pack files only when needed. */
    ca_revert_to_flash();
    strlcpy(s_active_name, PW_BUILTIN_PACK_NAME, sizeof s_active_name);
    s_builtin_articles = ca_get()->valid ? ca_get()->count : 0;
    size_t total = 0, used = 0;
    pack_store_usage(&total, &used);
    ESP_LOGI(TAG, "pack store ready: %u/%u bytes used", (unsigned)used, (unsigned)total);
    return ESP_OK;
}

void pack_store_usage(size_t *total, size_t *used)
{
    uint64_t t = 0, u = 0;
    esp_err_t err = esp_vfs_fat_info(PW_PACK_DIR, &t, &u);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not read pack storage usage: %s", esp_err_to_name(err));
    }
    if (total) *total = (size_t)t;
    if (used) *used = (size_t)(t > u ? t - u : 0);
}

size_t pack_store_installable_bytes(void)
{
    size_t total = 0, used = 0;
    pack_store_usage(&total, &used);
    size_t free_bytes = total > used ? total - used : 0;
    return free_bytes > PW_PACK_UPLOAD_RESERVE ? free_bytes - PW_PACK_UPLOAD_RESERVE : 0;
}

/* Caller holds metadata mutex. Always scan to the end, even if the caller's
 * output is full, so the cache limit cannot become a pack-count limit. */
static int scan_metadata(pack_store_item_t *items, size_t cap, uint32_t *articles)
{
    DIR *dir = opendir(PW_PACK_DIR);
    if (dir == NULL) return -1;
    int count = 0;
    uint32_t total = 0;
    for (;;) {
        errno = 0;
        struct dirent *de = readdir(dir);
        if (de == NULL) {
            if (errno != 0) count = -1;
            break;
        }
        size_t len = strlen(de->d_name);
        if (len < 5 || strcmp(de->d_name + len - 4, ".pwp") != 0) continue;
        size_t stem = len - 4;
        if (stem > PW_PACK_NAME_MAX) continue;
        pack_store_item_t item = {0};
        memcpy(item.name, de->d_name, stem);
        if (!pack_store_safe_name(item.name)) continue;
        char path[80];
        path_for(item.name, path, sizeof path);
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) ||
                st.st_size < 0 || (uint64_t)st.st_size > UINT32_MAX) continue;
        if (ca_probe_pack(path, &item.articles) != ESP_OK) continue;
        if (count == INT_MAX || UINT32_MAX - total < item.articles) {
            count = -1;
            break;
        }
        item.bytes = (uint32_t)st.st_size;
        item.enabled = true;
        if ((size_t)count < cap) items[count] = item;
        total += item.articles;
        count++;
    }
    closedir(dir);
    if (articles != NULL) *articles = total;
    return count;
}

static int metadata_refresh_locked(void)
{
    if (s_metadata_valid) return s_metadata_count;
    s_metadata_count = scan_metadata(s_metadata, PW_METADATA_CACHE_ITEMS,
                                     &s_metadata_articles);
    s_metadata_valid = s_metadata_count >= 0 &&
                       s_metadata_count <= PW_METADATA_CACHE_ITEMS;
    return s_metadata_count;
}

esp_err_t pack_store_rescan(void)
{
    if (!metadata_lock()) return ESP_ERR_INVALID_STATE;
    s_metadata_valid = false;
    int count = metadata_refresh_locked();
    metadata_unlock();
    return count < 0 ? ESP_FAIL : ESP_OK;
}

int pack_store_list(pack_store_item_t *items, size_t cap)
{
    if ((items == NULL && cap != 0) || !metadata_lock()) return -1;
    int count = metadata_refresh_locked();
    if (count >= 0) {
        if (s_metadata_valid) {
            size_t n = (size_t)count < cap ? (size_t)count : cap;
            if (n != 0) memcpy(items, s_metadata, n * sizeof *items);
        } else {
            count = scan_metadata(items, cap, NULL);
        }
    }
    metadata_unlock();
    return count < 0 ? -1 : ((size_t)count < cap ? count : (int)cap);
}

int pack_store_count(void)
{
    if (!metadata_lock()) return 0;
    int count = metadata_refresh_locked();
    metadata_unlock();
    return count < 0 ? 0 : count;
}

int pack_store_list_all(pack_store_item_t **items)
{
    if (items == NULL) return -1;
    *items = NULL;
    if (!metadata_lock()) return -1;
    int count = metadata_refresh_locked();
    if (count > 0) {
        pack_store_item_t *result = (size_t)count > SIZE_MAX / sizeof *result
            ? NULL : calloc((size_t)count, sizeof *result);
        if (result == NULL) {
            count = -1;
        } else if (s_metadata_valid) {
            memcpy(result, s_metadata, (size_t)count * sizeof *result);
            *items = result;
        } else {
            int found = scan_metadata(result, (size_t)count, NULL);
            if (found < 0 || found > count) {
                free(result);
                count = -1;
            } else {
                *items = result;
                count = found;
            }
        }
    }
    metadata_unlock();
    return count;
}

uint32_t pack_store_total_articles(void)
{
    if (!metadata_lock()) return s_builtin_articles;
    int count = metadata_refresh_locked();
    uint32_t optional = count < 0 ? 0 : s_metadata_articles;
    uint32_t total = UINT32_MAX - s_builtin_articles < optional
        ? UINT32_MAX : s_builtin_articles + optional;
    metadata_unlock();
    return total;
}

esp_err_t pack_store_open_for_read(const char *name)
{
    if (name == NULL || name[0] == 0 || strcmp(name, PW_BUILTIN_PACK_NAME) == 0 ||
            strcmp(name, "starter") == 0) {
        if (strcmp(s_active_name, PW_BUILTIN_PACK_NAME) == 0 && ca_get()->valid &&
                !ca_get()->from_file) {
            return ESP_OK;
        }
        web_server_archive_about_to_change();
        ca_revert_to_flash();
        if (!ca_get()->valid) return ESP_ERR_INVALID_STATE;
        strlcpy(s_active_name, PW_BUILTIN_PACK_NAME, sizeof s_active_name);
        return ESP_OK;
    }
    if (!pack_store_safe_name(name)) return ESP_ERR_INVALID_ARG;
    if (strcmp(s_active_name, name) == 0 && ca_get()->valid && ca_get()->from_file) {
        return ESP_OK;
    }
    char path[80];
    path_for(name, path, sizeof path);
    esp_err_t err = ca_load_pack(path);
    if (err == ESP_OK) strlcpy(s_active_name, name, sizeof s_active_name);
    return err;
}

esp_err_t pack_store_restore_builtin(void)
{
    return pack_store_open_for_read(PW_BUILTIN_PACK_NAME);
}

esp_err_t pack_store_remove(const char *name)
{
    if (!pack_store_safe_name(name)) return ESP_ERR_INVALID_ARG;
    if (strcmp(s_active_name, name) == 0) {
        esp_err_t err = pack_store_restore_builtin();
        if (err != ESP_OK) return err;
    }
    char path[80];
    path_for(name, path, sizeof path);
    if (!metadata_lock()) return ESP_ERR_INVALID_STATE;
    /* Invalidate even on failure: the filesystem may have changed externally. */
    s_metadata_valid = false;
    esp_err_t err = unlink(path) == 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
    metadata_unlock();
    return err;
}

esp_err_t pack_store_begin_upload(const char *name, FILE **out, char *temp_path, size_t cap)
{
    if (!pack_store_safe_name(name) || out == NULL || cap < 24) return ESP_ERR_INVALID_ARG;
    /* FAT cannot replace an installed pack while its archive file is open. */
    if (strcmp(s_active_name, name) == 0) {
        esp_err_t err = pack_store_restore_builtin();
        if (err != ESP_OK) return err;
    }
    strlcpy(temp_path, PW_PACK_DIR "/.upload.tmp", cap);
    FILE *fh = fopen(temp_path, "wb");
    if (fh == NULL) return ESP_FAIL;
    *out = fh;
    return ESP_OK;
}

esp_err_t pack_store_finish_upload(FILE *fh, const char *temp_path, const char *name,
                                   uint32_t *articles)
{
    if (fh == NULL) return ESP_ERR_INVALID_ARG;
    if (fclose(fh) != 0) { unlink(temp_path); return ESP_FAIL; }
    uint32_t count = 0;
    esp_err_t err = ca_probe_pack(temp_path, &count);
    if (err != ESP_OK) { unlink(temp_path); return err; }
    char final_path[80];
    path_for(name, final_path, sizeof final_path);
    char backup_path[80];
    snprintf(backup_path, sizeof backup_path, PW_PACK_DIR "/.replace.bak");
    if (!metadata_lock()) { unlink(temp_path); return ESP_ERR_INVALID_STATE; }
    /* A failed rollback can also change the directory. Never retain a stale
     * snapshot across any part of the replacement transaction. */
    s_metadata_valid = false;
    unlink(backup_path);
    bool had_old = access(final_path, F_OK) == 0;
    if (had_old && rename(final_path, backup_path) != 0) {
        metadata_unlock();
        unlink(temp_path);
        return ESP_FAIL;
    }
    if (rename(temp_path, final_path) != 0) {
        if (had_old) rename(backup_path, final_path);
        metadata_unlock();
        unlink(temp_path);
        return ESP_FAIL;
    }
    unlink(backup_path);
    metadata_unlock();
    if (articles) *articles = count;
    return ESP_OK;
}

void pack_store_abort_upload(FILE *fh, const char *temp_path)
{
    if (fh != NULL) fclose(fh);
    if (temp_path != NULL) unlink(temp_path);
}
