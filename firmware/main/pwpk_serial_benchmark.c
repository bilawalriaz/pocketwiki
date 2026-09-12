#include "pwpk_serial_benchmark.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_rom_crc.h"
#include "esp_ota_ops.h"
#include "spi_flash_mmap.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "zstd.h"
#include <string.h>
#include <unistd.h>
#include "pack_store.h"

static const char *TAG = "pwpk_bench";

static void storage_benchmark(void)
{
    const pwpk_serial_fixture_t *f =
        &pwpk_serial_fixtures[pwpk_serial_fixture_count - 1];
    const char *path = PW_PACK_DIR "/.pwpk-bench.tmp";
    bool file_ready = false;
    if (pack_store_installable_bytes() > f->compressed) {
        FILE *w = fopen(path, "wb");
        if (w != NULL) {
            file_ready = fwrite(f->data, 1, f->compressed, w) == f->compressed;
            file_ready = fflush(w) == 0 && file_ready;
            fclose(w);
        }
    }
    const esp_partition_t *app = esp_ota_get_running_partition();
    size_t phys = spi_flash_cache2phys(f->data);
    bool raw_ready = app && phys != SPI_FLASH_CACHE2PHYS_FAIL &&
                     phys >= app->address &&
                     phys - app->address + f->compressed <= app->size;
    for (unsigned mode = 0; mode < 2; mode++) {
        for (size_t size = 1024; size <= 16384; size *= 2) {
            size_t before = esp_get_free_heap_size();
            uint8_t *buf = malloc(size);
            if (buf == NULL || !(mode ? raw_ready : file_ready)) {
                ESP_LOGI(TAG,
                         "{\"storage\":\"%s\",\"buffer\":%u,\"ok\":false,\"reason\":\"%s\"}",
                         mode ? "raw-app" : "fat", (unsigned)size,
                         buf == NULL ? "malloc" : "source-unavailable");
                free(buf);
                continue;
            }
            int64_t start = esp_timer_get_time();
            size_t total = 0;
            bool ok = true;
            for (unsigned rep = 0; rep < 100 && ok; rep++) {
                FILE *r = mode ? NULL : fopen(path, "rb");
                if (!mode && r == NULL) {
                    ok = false;
                    break;
                }
                for (size_t pos = 0; pos < f->compressed; pos += size) {
                    size_t n = f->compressed - pos;
                    if (n > size) n = size;
                    if (mode) {
                        ok = esp_partition_read(app, phys - app->address + pos,
                                                buf, n) == ESP_OK;
                    } else {
                        ok = fread(buf, 1, n, r) == n;
                    }
                    if (!ok || memcmp(buf, f->data + pos, n) != 0) {
                        ok = false;
                        break;
                    }
                    total += n;
                }
                if (r != NULL) fclose(r);
            }
            int64_t us = esp_timer_get_time() - start;
            ESP_LOGI(TAG,
                     "{\"storage\":\"%s\",\"buffer\":%u,\"bytes\":%u,\"read_us\":%lld,\"heap_before\":%u,\"heap_during\":%u,\"ok\":%s}",
                     mode ? "raw-app" : "fat", (unsigned)size,
                     (unsigned)total, (long long)us, (unsigned)before,
                     (unsigned)esp_get_free_heap_size(), ok ? "true" : "false");
            free(buf);
        }
    }
    unlink(path);
    size_t total = 0, used = 0;
    pack_store_usage(&total, &used);
    ESP_LOGI(TAG, "{\"pack_store_total\":%u,\"pack_store_used\":%u}",
             (unsigned)total, (unsigned)used);
}

static void benchmark_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    ESP_LOGI(TAG,
             "{\"board\":true,\"idf\":\"%s\",\"cores\":%u,\"revision\":%u,\"cpu_mhz\":%u,\"psram_bytes\":%u,\"heap_total\":%u,\"benchmark_stack_bytes\":8192}",
             esp_get_idf_version(), chip.cores, chip.revision,
             CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
             (unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_total_size(MALLOC_CAP_8BIT));
    for (size_t i = 0; i < pwpk_serial_fixture_count; i++) {
        const pwpk_serial_fixture_t *f = &pwpk_serial_fixtures[i];
        size_t before = esp_get_free_heap_size();
        size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
        int64_t start = esp_timer_get_time();
        ZSTD_DCtx *ctx = ZSTD_createDCtx();
        uint8_t input[1024], output[2048];
        size_t pos = 0, total = 0, rc = 1;
        uint32_t crc = 0;
        bool ok = false;
        const char *error = ctx ? "incomplete frame" : "context allocation failed";
        if (ctx != NULL && f->dictionary_size != 0) {
            rc = ZSTD_DCtx_loadDictionary(ctx, f->dictionary, f->dictionary_size);
            if (ZSTD_isError(rc)) error = ZSTD_getErrorName(rc);
        }
        if (ctx != NULL && !ZSTD_isError(rc)) {
            ZSTD_inBuffer in = { input, 0, 0 };
            for (;;) {
                if (in.pos == in.size && pos < f->compressed) {
                    size_t n = f->compressed - pos;
                    if (n > sizeof input) n = sizeof input;
                    memcpy(input, f->data + pos, n);
                    pos += n;
                    in.size = n;
                    in.pos = 0;
                }
                ZSTD_outBuffer out = { output, sizeof output, 0 };
                size_t prior = in.pos;
                rc = ZSTD_decompressStream(ctx, &out, &in);
                if (ZSTD_isError(rc)) {
                    error = ZSTD_getErrorName(rc);
                    break;
                }
                total += out.pos;
                crc = esp_rom_crc32_le(crc, output, out.pos);
                if (rc == 0) {
                    ok = total == f->raw && crc == f->crc32 &&
                         pos == f->compressed && in.pos == in.size;
                    error = ok ? "" : "length/CRC mismatch";
                    break;
                }
                if (total > f->raw ||
                    (prior == in.pos && out.pos == 0 && pos == f->compressed)) {
                    break;
                }
            }
        }
        int64_t us = esp_timer_get_time() - start;
        ESP_LOGI(TAG,
                 "{\"profile\":\"%s\",\"compressed\":%u,\"raw\":%u,\"dictionary_bytes\":%u,\"decoded\":%u,\"decode_us\":%lld,\"heap_before\":%u,\"heap_after\":%u,\"heap_min\":%u,\"largest_before\":%u,\"decoder_bytes\":%u,\"ok\":%s,\"error\":\"%s\"}",
                 f->name, (unsigned)f->compressed, (unsigned)f->raw,
                 (unsigned)f->dictionary_size, (unsigned)total, (long long)us,
                 (unsigned)before, (unsigned)esp_get_free_heap_size(),
                 (unsigned)esp_get_minimum_free_heap_size(), (unsigned)largest,
                 (unsigned)(ctx ? ZSTD_sizeof_DCtx(ctx) : 0),
                 ok ? "true" : "false", error);
        if (ctx != NULL) ZSTD_freeDCtx(ctx);
        vTaskDelay(1);
    }
    storage_benchmark();
    ESP_LOGI(TAG, "{\"benchmark_complete\":true}");
    vTaskDelete(NULL);
}

void pwpk_serial_benchmark_start(void)
{
    xTaskCreate(benchmark_task, "pwpk_bench", 8192, NULL,
                tskIDLE_PRIORITY + 1, NULL);
}
