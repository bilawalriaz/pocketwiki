#pragma once
#include "esp_err.h"

typedef enum { ESP_PARTITION_TYPE_APP = 0, ESP_PARTITION_TYPE_DATA = 1 } esp_partition_type_t;

typedef struct {
    esp_partition_type_t type;
    int subtype;
    uint32_t address;
    uint32_t size;
    char label[17];
    const char *path;          /* simulator: backing file */
} esp_partition_t;

const esp_partition_t *esp_partition_find_first(esp_partition_type_t type, int subtype,
                                                 const char *label);
esp_err_t esp_partition_read(const esp_partition_t *part, size_t off, void *dst, size_t len);
