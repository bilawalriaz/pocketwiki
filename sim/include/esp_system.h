#pragma once
#include "esp_err.h"
uint32_t esp_get_free_heap_size(void);
uint32_t esp_get_minimum_free_heap_size(void);
void esp_restart(void);
