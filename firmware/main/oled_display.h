/* SSD1306 OLED status display via esp_lcd. Optional at runtime: every call
 * is a no-op unless oled_init() succeeded. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t oled_init(void);

/* Show live state. online is true when the station-side Wi-Fi connection is
 * active; the setup access point remains available in either state. */
void oled_show_status(const char *ssid, const char *ip, uint32_t article_count,
                      uint32_t pack_count, size_t flash_used, size_t flash_total,
                      bool online, unsigned cpu_pct);

void oled_show_transfer(const char *pack, size_t received, size_t total,
                        size_t flash_used, size_t flash_total);

/* True while a pack transfer screen is being shown. The status loop must
 * leave the display alone until the transfer ends. */
bool oled_transfer_active(void);

/* End the transfer screen. The status loop redraws the normal status. */
void oled_clear_transfer(void);

/* Show a QR symbol for payload, centred and as large as the panel allows.
 * Returns false when the display is unavailable, a pack transfer owns the
 * screen, or the payload would need a symbol too large to scan here. */
bool oled_show_qr(const char *payload);

/* True while a QR screen is shown. The status loop must not overwrite it. */
bool oled_qr_active(void);

/* Dismiss the QR screen. The status loop redraws the normal status. */
void oled_clear_qr(void);

/* Show a one-line boot status (used before Wi-Fi is up). */
void oled_show_boot(const char *line);

#ifdef __cplusplus
}
#endif
