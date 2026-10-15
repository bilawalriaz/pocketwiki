#pragma once
#include "esp_err.h"
#include "wear_levelling.h"

typedef struct {
    bool format_if_mount_failed;
    int max_files;
    size_t allocation_unit_size;
    bool disk_status_check_enable;
    bool use_one_fat;
} esp_vfs_fat_mount_config_t;

esp_err_t esp_vfs_fat_spiflash_mount_rw_wl(const char *base_path, const char *label,
                                           const esp_vfs_fat_mount_config_t *conf,
                                           wl_handle_t *handle);
esp_err_t esp_vfs_fat_info(const char *base_path, uint64_t *total, uint64_t *free_bytes);
