/* PocketWiki HTTP server (esp_http_server). */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t web_server_init(void);

/* Release decoder state before the active archive's dictionary/file is
 * replaced. Safe to call before web_server_init(). */
void web_server_archive_about_to_change(void);

#ifdef __cplusplus
}
#endif
