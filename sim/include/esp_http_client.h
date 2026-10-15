#pragma once
/* esp_http_client on the host, backed by libcurl. The whole body is fetched
 * on open, then read() hands it out in the caller's chunk size. PW_SIM_URL_MAP
 * ("from=to;from=to") rewrites URL prefixes so tests can point the device's
 * hard-coded https:// catalogue at a local server. */
#include "esp_err.h"

typedef enum { HTTP_METHOD_GET = 0, HTTP_METHOD_POST } esp_http_client_method_t;

typedef struct esp_http_client *esp_http_client_handle_t;

typedef struct {
    const char *url;
    esp_http_client_method_t method;
    int timeout_ms;
    int buffer_size;
    int buffer_size_tx;
    const char *user_agent;
    esp_err_t (*crt_bundle_attach)(void *conf);
    int max_redirection_count;
} esp_http_client_config_t;

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config);
esp_err_t esp_http_client_set_header(esp_http_client_handle_t c, const char *key, const char *value);
esp_err_t esp_http_client_delete_header(esp_http_client_handle_t c, const char *key);
esp_err_t esp_http_client_open(esp_http_client_handle_t c, int write_len);
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t c);
int esp_http_client_get_status_code(esp_http_client_handle_t c);
int64_t esp_http_client_get_content_length(esp_http_client_handle_t c);
int esp_http_client_read(esp_http_client_handle_t c, char *buf, int len);
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t c);
esp_err_t esp_http_client_close(esp_http_client_handle_t c);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c);
int esp_http_client_get_errno(esp_http_client_handle_t c);
esp_err_t esp_http_client_get_and_clear_last_tls_error(esp_http_client_handle_t c,
                                                       int *code, int *flags);
