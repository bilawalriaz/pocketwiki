/* PocketWiki — offline knowledge server on ESP32-C3.
 *
 * Boot order: NVS -> task watchdog -> OLED welcome -> SoftAP -> archive
 * validation -> HTTP server -> status log + OLED status.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "content_archive.h"
#include "ble_provisioning.h"
#include "boot_button.h"
#include "oled_display.h"
#include "pack_store.h"
#include "web_server.h"
#include "wifi_ap.h"

static const char *TAG = "pocketwiki";

/* The status task walks and validates installed pack files before drawing.
 * That call path includes FAT/VFS frames and the archive probe's bounded I/O
 * buffer, so 3 KiB is not enough on the C3 once optional packs are present. */
#define PW_OLED_STATUS_STACK_BYTES 6144

static uint32_t cpu_pct(void)
{
    static uint32_t prev_idle = 0, prev_total = 0;
    enum { PW_CPU_TASK_CAP = 24 };
    static TaskStatus_t status[PW_CPU_TASK_CAP];
    UBaseType_t num = uxTaskGetNumberOfTasks();
    uint32_t idle = 0, total = 0;
    if (num <= PW_CPU_TASK_CAP) {
        UBaseType_t n = uxTaskGetSystemState(status, PW_CPU_TASK_CAP, &total);
        for (UBaseType_t i = 0; i < n; i++) {
            if (strcmp(status[i].pcTaskName, "IDLE") == 0 ||
                strcmp(status[i].pcTaskName, "IDLE1") == 0) {   /* dual-core S3 */
                idle += (uint32_t)status[i].ulRunTimeCounter;
            }
        }
    }
    uint32_t pct = 100;
    if (prev_total && total > prev_total) {
        uint32_t dt = total - prev_total;
        uint32_t di = idle - prev_idle;   /* both counters wrap together */
        pct = di >= dt ? 0u : 100u - (unsigned)(100u * di / dt);
    }
    prev_idle = idle;
    prev_total = total;
    return pct;
}

static void oled_status_task(void *arg)
{
    (void)arg;
    char ip[16] = "?.?.?.?";
    wifi_ap_get_ip(ip, sizeof ip);
    uint32_t last_count = UINT32_MAX;
    int last_packs = -1;
    size_t last_used = SIZE_MAX;
    bool last_online = false;
    bool last_suppressed = false;

    for (;;) {
        /* A pack transfer or a QR screen owns the display; do not overwrite it.
         * When it is released, force one status redraw so the frame left
         * behind (e.g. after an aborted transfer) is cleared. */
        if (oled_transfer_active() || oled_qr_active()) {
            last_suppressed = true;
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }
        if (last_suppressed) {
            last_suppressed = false;
            last_count = UINT32_MAX;
            last_packs = -1;
            last_used = SIZE_MAX;
            last_online = false;
        }
        int packs = pack_store_count() + 1;
        size_t flash_total = 0, flash_used = 0;
        pack_store_usage(&flash_total, &flash_used);
        uint32_t count = last_count;
        if (last_count == UINT32_MAX || packs != last_packs || flash_used != last_used) {
            count = pack_store_total_articles();
        }
        uint32_t cpu = cpu_pct();
        wifi_station_status_t station;
        wifi_ap_get_station(&station);
        if (count != last_count || packs != last_packs || flash_used != last_used ||
                station.connected != last_online) {
            oled_show_status(station.connected ? station.ssid : CONFIG_POCKETWIKI_AP_SSID,
                             station.connected && station.ip[0] ? station.ip : ip,
                             count, (uint32_t)packs, flash_used, flash_total,
                             station.connected, cpu);
            last_count = count;
            last_packs = packs;
            last_used = flash_used;
            last_online = station.connected;
        }
        static unsigned log_ticks;
        if (++log_ticks % 10 == 0) {
            ESP_LOGI(TAG, "cpu %u%% heap %lu", (unsigned)cpu,
                     (unsigned long)esp_get_free_heap_size());
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static esp_err_t init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

void app_main(void)
{
    ESP_LOGI(TAG, "PocketWiki booting (ESP-IDF %s)", IDF_VER);

    ESP_ERROR_CHECK(init_nvs());
    /* Note: the TWDT is already initialized by IDF startup code
     * (CONFIG_ESP_TASK_WDT_INIT) — do not call esp_task_wdt_init() again here.
     * web_server.c feeds it opportunistically between stream chunks. */

    bool oled_ready = oled_init() == ESP_OK;
    if (oled_ready) {
        oled_show_boot("STARTING");
    }

    ESP_ERROR_CHECK(wifi_ap_init());

    /* Non-fatal: with a missing/corrupt archive the server still boots and
     * serves /health, /api/stats and a clear error page. Failure must be
     * visible in the log and on the OLED, not a silent articles=0 boot. */
    esp_err_t ca_err = ca_init();
    if (ca_err != ESP_OK) {
        ESP_LOGE(TAG, "content archive invalid (%s)", esp_err_to_name(ca_err));
    }

    /* User packs live in a dedicated internal flash filesystem. A missing or
     * damaged optional pack filesystem must never prevent the reader from
     * booting; pack APIs will report no installed packs until it is repaired
     * or formatted on the next successful mount. */
    esp_err_t pack_err = pack_store_init();
    if (pack_err != ESP_OK) {
        ESP_LOGE(TAG, "pack store unavailable (%s); continuing without installed packs",
                 esp_err_to_name(pack_err));
    }
    ESP_ERROR_CHECK(ble_provisioning_init());

    ESP_ERROR_CHECK(web_server_init());

    char ip[16] = "?.?.?.?";
    wifi_ap_get_ip(ip, sizeof ip);

    const content_archive_t *ca = ca_get();
    ESP_LOGI(TAG, "PocketWiki ready: http://%s/  articles=%lu  archive_version=%u  heap=%lu",
             ip, (unsigned long)ca->count, ca->format_version,
             (unsigned long)esp_get_free_heap_size());

    if (ca->valid) {
        size_t flash_total = 0, flash_used = 0;
        pack_store_usage(&flash_total, &flash_used);
        oled_show_status(CONFIG_POCKETWIKI_AP_SSID, ip, pack_store_total_articles(),
                         (uint32_t)(pack_store_count() + 1), flash_used, flash_total, false, 0);
        if (oled_ready) {
            BaseType_t created = xTaskCreate(oled_status_task, "oled_status",
                                             PW_OLED_STATUS_STACK_BYTES,
                                             NULL, 3, NULL);
            if (created != pdPASS) {
                ESP_LOGW(TAG, "could not start OLED status task");
            } else {
                /* The cycle returns to the status screen, so the button only
                 * makes sense when that task is drawing it. */
                esp_err_t err = boot_button_init();
                if (err != ESP_OK && err != ESP_ERR_NOT_SUPPORTED) {
                    ESP_LOGW(TAG, "boot button unavailable (%s)", esp_err_to_name(err));
                }
            }
        }
    } else {
        oled_show_boot("LIBRARY ERROR");
    }
}
