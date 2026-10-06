/* PocketWiki HTTP server (esp_http_server). */
#pragma once

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t web_server_init(void);

/* Catalogue pack ids the browser flasher queued and that have not installed
 * yet. Non-zero means the device is waiting on the station uplink or is
 * mid-install; the OLED status loop shows it. */
uint32_t web_server_pack_queue_pending(void);

#ifdef __cplusplus
}
#endif
