/* esp_http_client on libcurl.
 *
 * open() performs the whole transfer into memory, then read() hands the body
 * out in the caller's chunk size, which is all the firmware's download loops
 * need. Two environment variables make the device paths testable offline:
 *
 *   PW_SIM_URL_MAP="https://packs.educated.space=http://127.0.0.1:9000;..."
 *       rewrites URL prefixes, so the firmware's hard-coded HTTPS catalogue
 *       and pack URLs can be served by a local test server;
 *   PW_SIM_HTTP_DROP_AT=<bytes>
 *       makes the first read that would cross that offset fail once, to
 *       exercise the installer's resume and restart paths.
 */
#include "esp_http_client.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"

static const char *TAG = "sim-http";

#define MAX_HDRS 8
#define BODY_CAP (64u * 1024u * 1024u)

struct esp_http_client {
    char url[1024];
    int timeout_ms;
    char *hdr_key[MAX_HDRS];
    char *hdr_val[MAX_HDRS];
    uint8_t *body;
    size_t len;
    size_t cap;
    size_t pos;
    int status;
    int64_t content_length;
    bool open;
    bool overflow;
};

static long s_drop_at = -1;
static bool s_dropped;

static void map_url(const char *in, char *out, size_t cap)
{
    snprintf(out, cap, "%s", in);
    const char *map = getenv("PW_SIM_URL_MAP");
    if (map == NULL) return;
    char *copy = strdup(map);
    for (char *rule = strtok(copy, ";"); rule != NULL; rule = strtok(NULL, ";")) {
        char *eq = strchr(rule, '=');
        if (eq == NULL) continue;
        *eq = '\0';
        size_t n = strlen(rule);
        if (strncmp(in, rule, n) == 0) {
            snprintf(out, cap, "%s%s", eq + 1, in + n);
            break;
        }
    }
    free(copy);
}

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config)
{
    static bool curl_ready;
    if (!curl_ready) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        const char *drop = getenv("PW_SIM_HTTP_DROP_AT");
        if (drop != NULL) s_drop_at = strtol(drop, NULL, 10);
        curl_ready = true;
    }
    struct esp_http_client *c = calloc(1, sizeof *c);
    if (c == NULL || config->url == NULL) {
        free(c);
        return NULL;
    }
    snprintf(c->url, sizeof c->url, "%s", config->url);
    c->timeout_ms = config->timeout_ms > 0 ? config->timeout_ms : 5000;
    return c;
}

esp_err_t esp_http_client_set_header(esp_http_client_handle_t c, const char *key, const char *value)
{
    for (int i = 0; i < MAX_HDRS; i++) {
        if (c->hdr_key[i] && strcasecmp(c->hdr_key[i], key) == 0) {
            free(c->hdr_val[i]);
            c->hdr_val[i] = strdup(value);
            return ESP_OK;
        }
    }
    for (int i = 0; i < MAX_HDRS; i++) {
        if (c->hdr_key[i] == NULL) {
            c->hdr_key[i] = strdup(key);
            c->hdr_val[i] = strdup(value);
            return ESP_OK;
        }
    }
    return ESP_ERR_NO_MEM;
}

esp_err_t esp_http_client_delete_header(esp_http_client_handle_t c, const char *key)
{
    for (int i = 0; i < MAX_HDRS; i++) {
        if (c->hdr_key[i] && strcasecmp(c->hdr_key[i], key) == 0) {
            free(c->hdr_key[i]);
            free(c->hdr_val[i]);
            c->hdr_key[i] = c->hdr_val[i] = NULL;
        }
    }
    return ESP_OK;
}

static size_t on_body(char *ptr, size_t size, size_t nmemb, void *ud)
{
    struct esp_http_client *c = ud;
    size_t n = size * nmemb;
    if (c->len + n > BODY_CAP) {
        c->overflow = true;
        return 0;
    }
    if (c->len + n > c->cap) {
        size_t cap = c->cap ? c->cap : 65536;
        while (cap < c->len + n) cap *= 2;
        uint8_t *grown = realloc(c->body, cap);
        if (grown == NULL) return 0;
        c->body = grown;
        c->cap = cap;
    }
    memcpy(c->body + c->len, ptr, n);
    c->len += n;
    return n;
}

esp_err_t esp_http_client_open(esp_http_client_handle_t c, int write_len)
{
    (void)write_len;
    c->len = c->pos = 0;
    c->status = 0;
    c->content_length = -1;
    c->open = false;
    char url[1100];
    map_url(c->url, url, sizeof url);

    CURL *curl = curl_easy_init();
    if (curl == NULL) return ESP_FAIL;
    struct curl_slist *hdrs = NULL;
    for (int i = 0; i < MAX_HDRS; i++) {
        if (c->hdr_key[i] == NULL) continue;
        char line[512];
        snprintf(line, sizeof line, "%s: %s", c->hdr_key[i], c->hdr_val[i]);
        hdrs = curl_slist_append(hdrs, line);
    }
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 4L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)c->timeout_ms);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "PocketWiki/1.0 (sim)");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, c);
    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_off_t cl = -1;
    curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &cl);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK) {
        ESP_LOGW(TAG, "GET %s failed: %s", url, curl_easy_strerror(rc));
        return ESP_FAIL;
    }
    c->status = (int)status;
    c->content_length = cl >= 0 ? (int64_t)cl : (int64_t)c->len;
    c->open = true;
    ESP_LOGI(TAG, "GET %s -> %d (%zu bytes)", url, c->status, c->len);
    return ESP_OK;
}

int64_t esp_http_client_fetch_headers(esp_http_client_handle_t c)
{
    return c->open ? c->content_length : -1;
}

int esp_http_client_get_status_code(esp_http_client_handle_t c) { return c->status; }
int64_t esp_http_client_get_content_length(esp_http_client_handle_t c) { return c->content_length; }

int esp_http_client_read(esp_http_client_handle_t c, char *buf, int len)
{
    if (!c->open || len <= 0) return -1;
    size_t n = c->len - c->pos;
    if ((size_t)len < n) n = (size_t)len;
    if (s_drop_at >= 0 && !s_dropped && c->pos + n > (size_t)s_drop_at) {
        s_dropped = true;
        ESP_LOGW(TAG, "injected read failure at byte %zu", c->pos);
        c->open = false;
        return -1;
    }
    memcpy(buf, c->body + c->pos, n);
    c->pos += n;
    return (int)n;
}

bool esp_http_client_is_complete_data_received(esp_http_client_handle_t c)
{
    return c->open && c->pos == c->len;
}

esp_err_t esp_http_client_close(esp_http_client_handle_t c)
{
    c->open = false;
    return ESP_OK;
}

esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c)
{
    if (c == NULL) return ESP_OK;
    for (int i = 0; i < MAX_HDRS; i++) {
        free(c->hdr_key[i]);
        free(c->hdr_val[i]);
    }
    free(c->body);
    free(c);
    return ESP_OK;
}

int esp_http_client_get_errno(esp_http_client_handle_t c) { (void)c; return 0; }

esp_err_t esp_http_client_get_and_clear_last_tls_error(esp_http_client_handle_t c,
                                                       int *code, int *flags)
{
    (void)c;
    if (code) *code = 0;
    if (flags) *flags = 0;
    return ESP_OK;
}
