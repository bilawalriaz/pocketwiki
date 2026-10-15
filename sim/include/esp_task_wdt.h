#pragma once
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
esp_err_t esp_task_wdt_status(TaskHandle_t task);
esp_err_t esp_task_wdt_reset(void);
esp_err_t esp_task_wdt_add(TaskHandle_t task);
esp_err_t esp_task_wdt_delete(TaskHandle_t task);
