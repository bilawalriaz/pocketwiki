/* PocketWiki HTTP server.
 *
 * Design notes:
 *  - Articles are stored as independent v3 frames: raw DEFLATE streams
 *    compressed against the pack's trained dictionary. A bounded streaming
 *    the dictionary is loaded into that same ring before the article is
 *    inflated, so one static buffer decodes every generation of article.
 *    The stored body is a complete sanitized PocketWiki HTML document with
 *    navigation and the first h1, so no uncompressed chrome is mixed into
 *    the response.
 *  - All generated HTML (homepage, search, errors) is sent chunked with
 *    httpd_resp_send_chunk. Shared renderer buffers stay out of the httpd
 *    task stack; esp_http_server runs handlers serially in one task.
 *  - The task watchdog is fed between stream chunks (harmless no-op if the
 *    httpd task is not subscribed).
 */

#include "web_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/select.h>

#include "esp_http_server.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "catalog_index.h"
#include "cJSON.h"
#include "miniz.h"
#include "psa/crypto.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <strings.h>

#include "content_archive.h"
#include "pack_store.h"
#include "html_helpers.h"
#include "oled_display.h"
#include "title_lookup.h"
#include "ble_provisioning.h"
#include "wifi_ap.h"

static const char *TAG = "httpd";

/* Keep response writes bounded while avoiding the old 25 ms sleep on every
 * 256-byte fragment. The IDF httpd helper handles partial socket writes; a
 * 1 KiB application chunk is large enough to amortize framing overhead while
 * remaining small enough for the C3/lwIP send path. */
#define PW_CHUNK_SIZE   1024
#define PW_TITLE_CAP    512          /* display titles are <= 511 bytes */
#define PW_TITLE_ESC    2048         /* 511 bytes * 4 escaping multiplier */
#define PW_RENDER_CAP   (PW_TITLE_ESC + PW_PACK_NAME_MAX + 96)
#define PW_PAGE_SIZE      100          /* titles per home-page library preview */
#define PW_BROWSE_PAGE_SIZE 200          /* titles per infinite-scroll browse fetch */
#define PW_SEARCH_LIMIT 20
#define PW_HTTPD_STACK_BYTES (16u * 1024u)
#define PW_ARTICLE_YIELD_BYTES 4096
/* The 32 KiB inflate ring is both the LZ window and the pack's dictionary
 * slot: miniz's tinfl resolves back-references against the caller's output
 * buffer, so the trained dictionary is loaded right-aligned into the ring
 * before an article is inflated. 32768 is the DEFLATE maximum distance and
 * PW_MAX_DICT_BYTES, so no article frame can address past it. */
#define PW_INFLATE_RING_SIZE 32768
#define SEND_LITERAL(req, literal) \
    send_body((req), (literal), sizeof(literal) - 1)

/* Same, for page sections long enough to need more than one socket write. */
#define SEND_BODY_LITERAL(req, literal) \
    send_body((req), (literal), sizeof(literal) - 1)

/* Close a chunked response by writing the five-byte chunked terminator in one
 * bounded socket write.
 *
 * ESP-IDF's httpd_resp_send_chunk() emits that terminator as two separate
 * tiny writes through its own partial-write retry loop. On the
 * ESP32-C3 that loop can stall the whole httpd task - handlers are serialized,
 * so one stalled response stops the access-point page, the reader, and every
 * other route at once. The terminator never needs chunk framing help: 5 bytes
 * fit any send window, and a short write is detected instead of retried. */
static void retire_client(httpd_req_t *req);

static esp_err_t finish_response(httpd_req_t *req)
{
    static const char terminator[] = "0\r\n\r\n";
    int fd = httpd_req_to_sockfd(req);
    if (fd < 0) return ESP_FAIL;
    int sent = httpd_socket_send(req->handle, fd, terminator,
                                 sizeof(terminator) - 1, 0);
    if (sent != (int)(sizeof(terminator) - 1)) {
        ESP_LOGW(TAG, "response terminator short write (%d)", sent);
        retire_client(req);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* Drop a client whose socket stopped accepting bytes.
 *
 * esp_http_server runs every handler in one task, so a write that stalls on a
 * client that has stopped reading (a phone browser that gave up, a socket left
 * behind by a cancelled page load) blocks the reader, the manager, and every
 * other route until it fails. Anything left on that socket is unrecoverable, so
 * the session is retired on the first failed write and the pool is freed for
 * the next request instead of being retried. */
static void retire_client(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    if (fd < 0) return;
    ESP_LOGW(TAG, "retiring client on fd %d after a failed write", fd);
    httpd_sess_trigger_close(req->handle, fd);
}

/* Send a rendered body with chunked encoding, splitting it into PW_CHUNK_SIZE
 * pieces. Chunk boundaries are arbitrary, and keeping every socket write at or
 * below the chunk size used by the article streams keeps the C3/lwIP send path
 * out of the partial-write retry loop when a client window is momentarily
 * full. The largest single body sent here is the embedded manager script. */
static esp_err_t send_body(httpd_req_t *req, const char *buf, size_t len)
{
    while (len > 0) {
        size_t n = len > PW_CHUNK_SIZE ? (size_t)PW_CHUNK_SIZE : len;
        esp_err_t err = httpd_resp_send_chunk(req, buf, n);
        if (err != ESP_OK) {
            retire_client(req);
            return err;
        }
        buf += n;
        len -= n;
    }
    return ESP_OK;
}

/* One-shot (non-chunked) response, with the same retirement on failure. */
static esp_err_t send_plain(httpd_req_t *req, const char *buf, size_t len)
{
    esp_err_t err = httpd_resp_send(req, buf, (ssize_t)len);
    if (err != ESP_OK) retire_client(req);
    return err;
}

/* HTTP handlers are serialized by esp_http_server. Keep the largest repeated
 * rendering buffers out of the 16 KiB httpd task stack and reuse them between
 * handlers. */
static uint8_t s_render_title[PW_TITLE_CAP];
static char s_render_title_esc[PW_TITLE_ESC];
static char s_render_label_esc[PW_TITLE_ESC];
static char s_render_query_esc[PW_TITLE_ESC];
static char s_render_item[PW_RENDER_CAP];
static char s_download_disposition[64];

static void json_escape(const char *src, char *dst, size_t cap)
{
    size_t out = 0;
    for (size_t i = 0; src[i] && out + 2 < cap; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '\"' || c == '\\') { dst[out++] = '\\'; dst[out++] = (char)c; }
        else if (c >= 0x20) dst[out++] = (char)c;
    }
    dst[out] = 0;
}


extern const uint8_t pocketwiki_style_css[];
extern const size_t pocketwiki_style_css_len;

extern const uint8_t pocketwiki_catalog_json[];
extern const size_t pocketwiki_catalog_json_len;
extern const uint8_t pocketwiki_manager_js[];
extern const size_t pocketwiki_manager_js_len;

/* Pack IDs are stable storage names; their user-facing titles belong to the
 * catalogue. These helpers are defined with the catalogue handlers below but
 * are also used by the home/search/management pages above them. */
static bool catalog_pack_name(const char *id, char *out, size_t cap);
static void pack_display_name(const char *id, char *out, size_t cap);
static char *catalog_labels(const pack_store_item_t *items, int count);
static const char *pack_label(const char *id, const char *labels, int index,
                              char *fallback, size_t cap);
static void catalog_cache_invalidate(void);

/* ---- page scaffolding (chunked) ---- */

static esp_err_t page_open(httpd_req_t *req, const char *title_esc)
{
    char header[768];
    int len = snprintf(header, sizeof header,
                       "<!doctype html><html><head>"
                       "<meta charset=\"utf-8\">"
                       "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
                       "<link rel=\"stylesheet\" href=\"/style.css?v=5\">"
                       "<title>PocketWiki%s</title></head><body>"
                       "<header><div class=\"header-inner\">"
                       "<a class=\"brand\" href=\"/\" aria-label=\"PocketWiki home\">"
                       "<span class=\"brand-mark\" aria-hidden=\"true\">P</span>"
                       "<span>PocketWiki</span></a>"
                       "<nav aria-label=\"Main navigation\"><a href=\"/manage\">Manage library</a></nav>"
                       "<form class=\"search\" action=\"/search\" method=\"get\">"
                       "<label class=\"sr-only\" for=\"site-search\">Search article titles</label>"
                       "<input id=\"site-search\" type=\"search\" name=\"q\" "
                       "placeholder=\"Search the library\" autocomplete=\"off\" maxlength=\"128\">"
                       "<button type=\"submit\">Search</button></form></div></header><main>",
                       title_esc);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    /* The page carries the manager script inline, so a cached copy would keep
     * serving old behaviour after a firmware update. */
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (len < 0 || (size_t)len >= sizeof header) return ESP_FAIL;
    return send_body(req, header, (size_t)len);
}

static esp_err_t page_close(httpd_req_t *req)
{
    char foot[160];
    int libraries = pack_store_count() + 1;
    snprintf(foot, sizeof foot,
             "</main><footer>PocketWiki offline &middot; %d librar%s ready &middot; %lu articles</footer></body></html>",
             libraries, libraries == 1 ? "y" : "ies",
             (unsigned long)pack_store_total_articles());
    send_body(req, foot, strlen(foot));
    esp_err_t result = finish_response(req);
    return result;
}

/* ---- list rendering (escaped titles can be ~2 KB; send in one chunk) ---- */

static esp_err_t send_search_item(httpd_req_t *req, uint32_t id, const char *pack,
                                  const char *esc, bool bold)
{
    int len = snprintf(s_render_item, sizeof s_render_item,
                       bold ? "<a href=\"/a/%lu?pack=%s\"><strong>%s</strong></a>"
                            : "<a href=\"/a/%lu?pack=%s\">%s</a>",
                       (unsigned long)id, pack, esc);
    if (len < 0 || (size_t)len >= sizeof s_render_item) return ESP_FAIL;
    return send_body(req, s_render_item, (size_t)len);
}

/* Search the archive most recently opened by pack_store_open_for_read(). Packs
 * are visited one at a time, keeping RAM bounded while presenting one library. */
static int send_library_search_results(httpd_req_t *req, const char *pack,
                                       const char *label, const uint8_t *norm,
                                       size_t norm_len, int limit)
{
    const content_archive_t *ca = ca_get();
    if (!ca->valid || limit <= 0) return 0;
    tl_index_t ix = { .entries_off = ca->entries_off, .strings_off = ca->strings_off,
                      .count = ca->count, .entry_size = TL_ENTRY_SIZE,
                      .read = ca_index_read, .ctx = (void *)ca };
    uint32_t exact = 0;
    bool have_exact = tl_exact(&ix, norm, norm_len, &exact) == 0;
    uint32_t ids[PW_SEARCH_LIMIT];
    int count = 0;
    if (tl_prefix(&ix, norm, norm_len, ids, limit, &count) != 0) return -1;
    if (!have_exact && count == 0) return 0;

    if (html_escape(label, strlen(label), s_render_label_esc, sizeof s_render_label_esc) == 0) return -1;
    SEND_LITERAL(req, "<section class=\"search-library\"><h2>");
    send_body(req, s_render_label_esc, strlen(s_render_label_esc));
    SEND_LITERAL(req, "</h2><div class=\"list\">");

    int shown = 0;
    if (have_exact && shown < limit) {
        uint32_t tlen = 0;
        if (tl_read_display_title(&ix, exact, s_render_title, sizeof s_render_title, &tlen) == 0 &&
                html_escape((const char *)s_render_title, tlen, s_render_title_esc,
                            sizeof s_render_title_esc) != 0) {
            send_search_item(req, exact, pack, s_render_title_esc, true);
            shown++;
        }
    }
    for (int i = 0; i < count && shown < limit; i++) {
        uint32_t id = ids[i];
        if (have_exact && id == exact) continue;
        uint32_t tlen = 0;
        if (tl_read_display_title(&ix, id, s_render_title, sizeof s_render_title, &tlen) != 0) continue;
        if (html_escape((const char *)s_render_title, tlen, s_render_title_esc,
                        sizeof s_render_title_esc) == 0) continue;
        send_search_item(req, id, pack, s_render_title_esc, false);
        shown++;
    }
    SEND_LITERAL(req, "</div></section>");
    return shown;
}

static esp_err_t send_library_item(httpd_req_t *req, uint32_t id, const char *pack,
                                   const char *esc)
{
    int len = snprintf(s_render_item, sizeof s_render_item,
                       "<a href=\"/a/%lu?pack=%s\">%s</a>",
                       (unsigned long)id, pack, esc);
    if (len < 0 || (size_t)len >= sizeof s_render_item) return ESP_FAIL;
    return send_body(req, s_render_item, (size_t)len);
}

/* Every installed pack is enabled. Archives are still visited serially so the
 * title index for only one pack occupies RAM at a time. */
static esp_err_t send_library_group(httpd_req_t *req, const char *name,
                                    const char *label)
{
    const content_archive_t *ca = ca_get();
    char head[160];
    if (html_escape(label, strlen(label), s_render_label_esc, sizeof s_render_label_esc) == 0) {
        return ESP_FAIL;
    }
    snprintf(head, sizeof head,
             "<details class=\"library-group\"><summary><span><strong>");
    send_body(req, head, strlen(head));
    send_body(req, s_render_label_esc, strlen(s_render_label_esc));
    snprintf(head, sizeof head,
             "</strong><small>%lu article%s</small></span><span class=\"active-label\">Ready</span>"
             "</summary><div class=\"library-articles\">",
             (unsigned long)ca->count, ca->count == 1 ? "" : "s");
    send_body(req, head, strlen(head));

    tl_index_t ix = { .entries_off = ca->entries_off, .strings_off = ca->strings_off,
                      .count = ca->count, .entry_size = TL_ENTRY_SIZE,
                      .read = ca_index_read, .ctx = (void *)ca };
    uint32_t end = ca->count < PW_PAGE_SIZE ? ca->count : PW_PAGE_SIZE;
    for (uint32_t id = 0; id < end; id++) {
        uint32_t tlen = 0;
        if (tl_read_display_title(&ix, id, s_render_title, sizeof s_render_title, &tlen) != 0) continue;
        if (html_escape((const char *)s_render_title, tlen, s_render_title_esc,
                        sizeof s_render_title_esc) == 0) continue;
        send_library_item(req, id, name, s_render_title_esc);
    }
    if (ca->count > end) {
        snprintf(head, sizeof head,
                 "<p class=\"library-note\">Showing first %lu of %lu articles. "
                 "<a href=\"/browse?pack=%s\">Browse all &rarr;</a></p>",
                 (unsigned long)end, (unsigned long)ca->count, name);
        send_body(req, head, strlen(head));
    }
    return SEND_LITERAL(req, "</div></details>");
}

/* ---- error pages ---- */

static esp_err_t send_error(httpd_req_t *req, int status, const char *code,
                            const char *title, const char *detail)
{
    httpd_resp_set_status(req, code);
    if (page_open(req, " &middot; error") != ESP_OK) return ESP_FAIL;
    int len = snprintf(s_render_item, sizeof s_render_item,
                       "<div class=\"err\"><h1>%s</h1><p>%s</p>"
                       "<p><a href=\"/\">Back to your libraries</a></p></div>",
                       title, detail);
    if (len < 0 || (size_t)len >= sizeof s_render_item) return ESP_FAIL;
    send_body(req, s_render_item, (size_t)len);
    return page_close(req);
}

/* ---- / handlers ---- */

static esp_err_t handle_root(httpd_req_t *req)
{
    const content_archive_t *ca = ca_get();
    if (!ca->valid) {
        return send_error(req, 500, "500 Internal Server Error",
                          "Content archive not loaded",
                          "The content partition is missing or invalid. "
                          "See the device log for details.");
    }
    page_open(req, " &middot; article list");

    pack_store_item_t *items = NULL;
    int count = pack_store_list_all(&items);
    if (count < 0) count = 0;
    char hdr[160];
    snprintf(hdr, sizeof hdr,
             "<h1>Your libraries</h1><p class=\"muted\">%d librar%s ready to read. Open one to browse its articles.</p>"
             "<div class=\"library-shelf\">",
             count + 1, count == 0 ? "y" : "ies");
    send_body(req, hdr, strlen(hdr));

    if (!pack_store_guide_hidden() &&
            pack_store_open_for_read(PW_BUILTIN_PACK_NAME) == ESP_OK) {
        char label[PW_TITLE_CAP];
        pack_display_name(PW_BUILTIN_PACK_NAME, label, sizeof label);
        send_library_group(req, PW_BUILTIN_PACK_NAME, label);
    }
    char *labels = catalog_labels(items, count);
    for (int i = 0; i < count; i++) {
        if (pack_store_open_for_read(items[i].name) == ESP_OK) {
            char fallback[PW_TITLE_CAP];
            const char *label = pack_label(items[i].name, labels, i, fallback, sizeof fallback);
            send_library_group(req, items[i].name, label);
        }
    }
    free(labels);
    free(items);
    SEND_LITERAL(req, "</div>");
    if (pack_store_restore_builtin() != ESP_OK) {
        ESP_LOGE(TAG, "could not restore built-in library after shelf render");
    }

    return page_close(req);
}

static esp_err_t handle_search(httpd_req_t *req)
{
    char raw[512];
    size_t rlen = httpd_req_get_url_query_len(req);
    if (rlen == 0 || rlen >= sizeof raw) {
        return send_error(req, 400, "400 Bad Request", "Missing query",
                          "Search needs a q= parameter (max 128 characters).");
    }
    httpd_req_get_url_query_str(req, raw, sizeof raw);

    char encoded[385];
    if (httpd_query_key_value(raw, "q", encoded, sizeof encoded) != ESP_OK) {
        return send_error(req, 400, "400 Bad Request", "Missing query",
                          "Search needs a q= parameter (max 128 characters).");
    }
    char q[129];
    int qlen = url_decode_query(encoded, strlen(encoded), q, sizeof q);
    if (qlen < 0) {
        return send_error(req, 400, "400 Bad Request", "Bad query",
                          "The search query could not be decoded.");
    }
    q[qlen] = '\0';

    uint8_t norm[256];
    size_t norm_len = tl_normalize((const uint8_t *)q, (size_t)qlen, norm);

    page_open(req, " &middot; search");
    SEND_LITERAL(req, "<h1>Search</h1>");

    html_escape(q, (size_t)qlen, s_render_query_esc, sizeof s_render_query_esc);
    SEND_LITERAL(req, "<p class=\"muted\">Results for &ldquo;");
    send_body(req, s_render_query_esc, strlen(s_render_query_esc));
    SEND_LITERAL(req, "&rdquo;</p>");

    if (norm_len == 0) {
        SEND_LITERAL(req, "<p>Empty search term. Try a word from a title.</p>");
        return page_close(req);
    }

    int shown = 0;
    bool failed = false;
    if (pack_store_open_for_read(PW_BUILTIN_PACK_NAME) == ESP_OK) {
        char label[PW_TITLE_CAP];
        pack_display_name(PW_BUILTIN_PACK_NAME, label, sizeof label);
        int n = send_library_search_results(req, PW_BUILTIN_PACK_NAME,
                                            label,
                                            norm, norm_len, PW_SEARCH_LIMIT);
        if (n < 0) failed = true; else shown += n;
    }
    pack_store_item_t *items = NULL;
    int pack_count = pack_store_list_all(&items);
    char *labels = catalog_labels(items, pack_count);
    for (int i = 0; i < pack_count && shown < PW_SEARCH_LIMIT && !failed; i++) {
        if (pack_store_open_for_read(items[i].name) != ESP_OK) continue;
        char fallback[PW_TITLE_CAP];
        const char *label = pack_label(items[i].name, labels, i, fallback, sizeof fallback);
        int n = send_library_search_results(req, items[i].name, label, norm, norm_len,
                                            PW_SEARCH_LIMIT - shown);
        if (n < 0) failed = true; else shown += n;
    }
    free(labels);
    free(items);
    if (pack_store_restore_builtin() != ESP_OK) {
        ESP_LOGE(TAG, "could not restore built-in library after search");
    }
    if (failed) {
        SEND_LITERAL(req, "<p>One library could not be searched.</p>");
    } else if (shown == 0) {
        SEND_LITERAL(req, "<p>No articles match.</p>");
    }
    if (shown >= PW_SEARCH_LIMIT) {
        SEND_LITERAL(req, "<p class=\"muted\">First 20 matches shown across all libraries.</p>");
    } else if (shown > 0) {
        char summary[96];
        snprintf(summary, sizeof summary,
                 "<p class=\"muted\">%d result%s across all libraries.</p>",
                 shown, shown == 1 ? "" : "s");
        send_body(req, summary, strlen(summary));
    }
    return page_close(req);
}

/* ---- article streaming ---- */

static esp_err_t stream_article_deflate(httpd_req_t *req, uint32_t id,
                                        const pw_article_meta_t *meta,
                                        const char *etag, uint32_t dict_len);

static esp_err_t finish_decode_error(httpd_req_t *req, bool any_sent,
                                     const char *message)
{
    if (!any_sent) {
        char body[192];
        int len = snprintf(body, sizeof body,
                           "<html><body><p>%s</p></body></html>", message);
        if (len < 0 || (size_t)len >= sizeof body) return ESP_FAIL;
        send_body(req, body, (size_t)len);
    } else {
        SEND_LITERAL(req, "</body></html>");
    }
    return finish_response(req);
}

/* ---- decoder workspace ----
 *
 * One static buffer holds the whole decoder: the inflater state, its LZ ring
 * (which is also where a pack's trained dictionary is loaded) and the 1 KiB
 * input chunk read from flash. HTTP handlers are serialized by
 * esp_http_server, so one decoder serves every request with no per-request
 * allocation — the device cannot allocate one once Wi-Fi and Bluetooth are up.
 */

static struct {
    tinfl_decompressor decomp;
    uint8_t window[PW_INFLATE_RING_SIZE];
    uint8_t in[PW_CHUNK_SIZE];
} s_decoder __attribute__((aligned(8)));

static esp_err_t serve_article(httpd_req_t *req, uint32_t id, bool download)
{
    pw_article_meta_t meta;
    esp_err_t err = ca_get_article(id, &meta);
    if (err == ESP_ERR_NOT_FOUND) {
        return send_error(req, 404, "404 Not Found", "Article not found",
                          "No article has this identifier.");
    }
    if (err != ESP_OK) {
        return send_error(req, 500, "500 Internal Server Error", "Corrupt index",
                          "The article index entry could not be read safely.");
    }

    char etag[16];
    etag_from_crc(meta.crc32, etag);

    size_t hlen = httpd_req_get_hdr_value_len(req, "If-None-Match");
    if (!download && hlen > 0) {
        char inm[64];
        if (hlen < sizeof inm &&
                httpd_req_get_hdr_value_str(req, "If-None-Match", inm, sizeof inm) == ESP_OK &&
                strcmp(inm, etag) == 0) {
            httpd_resp_set_status(req, "304 Not Modified");
            httpd_resp_set_hdr(req, "ETag", etag);
            return send_plain(req, NULL, 0);
        }
    }

    httpd_resp_set_status(req, "200 OK");
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    if (!download) {
        httpd_resp_set_hdr(req, "ETag", etag);
        httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=86400");
    } else {
        int n = snprintf(s_download_disposition, sizeof s_download_disposition,
                         "attachment; filename=\"pocketwiki-%lu.html\"",
                         (unsigned long)id);
        if (n < 0 || (size_t)n >= sizeof s_download_disposition) return ESP_FAIL;
        httpd_resp_set_hdr(req, "Content-Disposition", s_download_disposition);
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    }

    /* Decode the stored frame on-device and stream identity HTML in small,
     * bounded chunks — clients never see the storage codec. The article is a raw
     * DEFLATE stream whose pack dictionary is loaded into the inflate ring for
     * the duration of this response. */
    return stream_article_deflate(req, id, &meta, etag, ca_get()->dict_len);
}

/* Bounds-checked read of one article's compressed bytes from the archive. */
static bool article_read_range(const pw_article_meta_t *meta, size_t offset,
                               uint8_t *buf, size_t len)
{
    if (offset > meta->comp_len || len > (size_t)meta->comp_len - offset) return false;
    return ca_read_payload(meta->content_offset + (uint32_t)offset, buf, len) == ESP_OK;
}

/* Stream one article out of the inflate ring.
 *
 * A v3 frame is a raw DEFLATE stream compressed against the pack's trained
 * dictionary. The dictionary is read from the payload tail straight into the
 * ring, right-aligned: a back-reference of distance d at ring position p reads
 * ring[(p - d) mod 32768], and the stream starts at ring offset 0, so
 * distances reach into the dictionary exactly as zlib's deflateSetDictionary
 * encoder intended. Nothing is allocated — the ring is the static arena above.
 */
static esp_err_t stream_article_deflate(httpd_req_t *req, uint32_t id,
                                        const pw_article_meta_t *meta,
                                        const char *etag, uint32_t dict_len)
{
    if (dict_len > PW_INFLATE_RING_SIZE) {
        ESP_LOGE(TAG, "article %lu: dictionary %lu B exceeds the %u-byte ring",
                 (unsigned long)id, (unsigned long)dict_len,
                 (unsigned)PW_INFLATE_RING_SIZE);
        return finish_decode_error(req, false, "Dictionary unavailable -- reinstall the pack.");
    }
    if (dict_len > 0) {
        const content_archive_t *ar = ca_get();
        if (ca_read_payload(ar->dict_offset,
                            s_decoder.window + (PW_INFLATE_RING_SIZE - dict_len),
                            dict_len) != ESP_OK) {
            ESP_LOGE(TAG, "article %lu: dictionary read failed (%lu B at %lu)",
                     (unsigned long)id, (unsigned long)dict_len,
                     (unsigned long)ar->dict_offset);
            return finish_decode_error(req, false, "Dictionary unavailable -- reinstall the pack.");
        }
    }

    size_t compressed_len = meta->comp_len;
    size_t input_offset = 0;
    size_t input_pos = 0;
    size_t input_len = 0;
    size_t total_out = 0;
    size_t bytes_since_yield = 0;
    uint32_t output_crc = 0;
    bool any_sent = false;
    tinfl_status status = TINFL_STATUS_FAILED;

    tinfl_init(&s_decoder.decomp);
    for (;;) {
        if (input_pos == input_len) {
            if (input_offset >= compressed_len) {
                return finish_decode_error(req, any_sent,
                                           "Article decode error -- try again.");
            }
            size_t n = compressed_len - input_offset;
            if (n > sizeof s_decoder.in) n = sizeof s_decoder.in;
            if (!article_read_range(meta, input_offset, s_decoder.in, n)) {
                return finish_decode_error(req, any_sent, "Article read error -- try again.");
            }
            input_offset += n;
            input_pos = 0;
            input_len = n;
        }

        size_t available = input_len - input_pos;
        size_t consumed = available;
        size_t window_offset = total_out & (PW_INFLATE_RING_SIZE - 1);
        size_t room = PW_INFLATE_RING_SIZE - window_offset;
        uint32_t flags = input_offset < compressed_len ? TINFL_FLAG_HAS_MORE_INPUT : 0;
        size_t produced = room;
        status = tinfl_decompress(&s_decoder.decomp, s_decoder.in + input_pos, &consumed,
                                  s_decoder.window, s_decoder.window + window_offset,
                                  &produced, flags);
        input_pos += consumed;

        if (produced > 0) {
            if (produced > (size_t)meta->uncomp_len - total_out) {
                ESP_LOGE(TAG, "article %lu: deflate output exceeds declared length",
                         (unsigned long)id);
                return finish_decode_error(req, any_sent, "Article verification failed -- try again.");
            }
            total_out += produced;
            output_crc = esp_rom_crc32_le(output_crc, s_decoder.window + window_offset, produced);
            if (send_body(req, (const char *)s_decoder.window + window_offset,
                          produced) != ESP_OK) {
                ESP_LOGW(TAG, "article %lu: client send aborted", (unsigned long)id);
                return ESP_FAIL;
            }
            any_sent = true;
            bytes_since_yield += produced;
            if (bytes_since_yield >= PW_ARTICLE_YIELD_BYTES) {
                bytes_since_yield = 0;
                taskYIELD();
            }
            if (esp_task_wdt_status(NULL) == ESP_OK) esp_task_wdt_reset();
        }

        if (status == TINFL_STATUS_DONE) {
            if (input_pos != input_len || input_offset != compressed_len) {
                ESP_LOGE(TAG, "article %lu: deflate stream has trailing input", (unsigned long)id);
                return finish_decode_error(req, any_sent, "Article verification failed -- try again.");
            }
            break;
        }
        if (status != TINFL_STATUS_NEEDS_MORE_INPUT &&
                status != TINFL_STATUS_HAS_MORE_OUTPUT) {
            ESP_LOGE(TAG, "article %lu: deflate inflate failed (status=%d)",
                     (unsigned long)id, (int)status);
            return finish_decode_error(req, any_sent, "Article decode error -- try again.");
        }
        if (status == TINFL_STATUS_NEEDS_MORE_INPUT && consumed == 0 &&
                input_offset >= compressed_len) {
            return finish_decode_error(req, any_sent, "Article decode error -- try again.");
        }
    }

    /* The frame carries no checksum of its own: the index CRC and length are
     * the integrity contract, and both are checked here. */
    if (total_out != meta->uncomp_len || output_crc != meta->crc32) {
        ESP_LOGE(TAG, "article %lu: deflate length/CRC mismatch (%lu != %lu)",
                 (unsigned long)id, (unsigned long)total_out,
                 (unsigned long)meta->uncomp_len);
        return finish_decode_error(req, any_sent, "Article verification failed -- try again.");
    }
    if (!any_sent) {
        /* Empty article: no chunk was streamed, so a bare terminator would
         * arrive before the response headers. */
        return send_plain(req, NULL, 0);
    }
    ESP_LOGD(TAG, "article %lu: %lu bytes decoded, etag %s",
             (unsigned long)id, (unsigned long)total_out, etag);
    return finish_response(req);
}

/* Switch the active library when the request carries ?pack=<name>, so one
 * device can serve several installed packs from the same routes. */
static esp_err_t select_requested_library(httpd_req_t *req)
{
    char raw[128];
    char pack[PW_PACK_NAME_MAX + 1];
    size_t len = httpd_req_get_url_query_len(req);
    if (len == 0) return ESP_OK;
    if (len >= sizeof raw || httpd_req_get_url_query_str(req, raw, sizeof raw) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    if (httpd_query_key_value(raw, "pack", pack, sizeof pack) != ESP_OK) return ESP_OK;
    return pack_store_open_for_read(pack);
}

static esp_err_t handle_article_id(httpd_req_t *req)
{
    /* uri = "/a/<digits>" */
    const char *uri = req->uri;
    const char *query = strchr(uri, '?');
    size_t ulen = query != NULL ? (size_t)(query - uri) : strlen(uri);
    if (ulen < 4 || ulen > 20 || uri[3] == '\0') {
        return send_error(req, 400, "400 Bad Request", "Bad article id",
                          "Expected /a/&lt;number&gt;.");
    }
    for (size_t i = 3; i < ulen; i++) {
        if (uri[i] < '0' || uri[i] > '9') {
            return send_error(req, 400, "400 Bad Request", "Bad article id",
                              "Article ids are plain numbers.");
        }
    }
    uint32_t id;
    if (!parse_u32_strict(uri + 3, ulen - 3, &id)) {
        return send_error(req, 400, "400 Bad Request", "Bad article id",
                          "Article id out of range.");
    }
    if (select_requested_library(req) != ESP_OK) {
        return send_error(req, 404, "404 Not Found", "Library not found",
                          "That library is no longer installed. Return to the library shelf and choose another.");
    }
    return serve_article(req, id, false);
}

static esp_err_t handle_download_id(httpd_req_t *req)
{
    /* uri = "/download/<digits>" */
    const char *uri = req->uri;
    const char *query = strchr(uri, '?');
    size_t ulen = query != NULL ? (size_t)(query - uri) : strlen(uri);
    if (ulen < 11 || ulen > 27 || uri[10] == '\0') {
        return send_error(req, 400, "400 Bad Request", "Bad article id",
                          "Expected /download/&lt;number&gt;.");
    }
    for (size_t i = 10; i < ulen; i++) {
        if (uri[i] < '0' || uri[i] > '9') {
            return send_error(req, 400, "400 Bad Request", "Bad article id",
                              "Article ids are plain numbers.");
        }
    }
    uint32_t id;
    if (!parse_u32_strict(uri + 10, ulen - 10, &id)) {
        return send_error(req, 400, "400 Bad Request", "Bad article id",
                          "Article id out of range.");
    }
    if (select_requested_library(req) != ESP_OK) {
        return send_error(req, 404, "404 Not Found", "Library not found",
                          "That library is no longer installed. Return to the library shelf and choose another.");
    }
    return serve_article(req, id, true);
}

static esp_err_t handle_article_title(httpd_req_t *req)
{
    /* uri = "/title/<url-encoded title>" */
    const char *uri = req->uri;
    const char *query = strchr(uri, '?');
    size_t ulen = query != NULL ? (size_t)(query - uri) : strlen(uri);
    if (ulen < 8) {   /* "/title/" is 7 chars */
        return send_error(req, 400, "400 Bad Request", "Bad title",
                          "Expected /title/&lt;encoded title&gt;.");
    }
    char decoded[257];
    int dlen = url_decode_path(uri + 7, ulen - 7, decoded, sizeof decoded);
    if (dlen < 0) {
        return send_error(req, 400, "400 Bad Request", "Bad title",
                          "The title could not be decoded.");
    }
    uint8_t norm[256];
    size_t norm_len = tl_normalize((const uint8_t *)decoded, (size_t)dlen, norm);
    if (norm_len == 0) {
        return send_error(req, 400, "400 Bad Request", "Bad title", "Empty title.");
    }

    if (select_requested_library(req) != ESP_OK) {
        return send_error(req, 404, "404 Not Found", "Library not found",
                          "That library is no longer installed.");
    }

    const content_archive_t *ca = ca_get();
    if (!ca->valid) {
        return send_error(req, 500, "500 Internal Server Error",
                          "Content archive not loaded",
                          "The content partition is missing or invalid.");
    }
    tl_index_t ix = { .entries_off = ca->entries_off, .strings_off = ca->strings_off,
                      .count = ca->count, .entry_size = TL_ENTRY_SIZE,
                      .read = ca_index_read, .ctx = (void *)ca };
    uint32_t id;
    if (tl_exact(&ix, norm, norm_len, &id) != 0) {
        return send_error(req, 404, "404 Not Found", "Article not found",
                          "No article has this title. Use the search page.");
    }
    return serve_article(req, id, false);
}

static esp_err_t handle_style(httpd_req_t *req)
{
    size_t len = pocketwiki_style_css_len;
    httpd_resp_set_status(req, "200 OK");
    httpd_resp_set_type(req, "text/css; charset=utf-8");
    char lenbuf[16];
    snprintf(lenbuf, sizeof lenbuf, "%lu", (unsigned long)len);
    httpd_resp_set_hdr(req, "Content-Length", lenbuf);
    /* The device keeps the same origin across firmware updates. Revalidate the
     * small stylesheet so a phone cannot pair new HTML with stale cached CSS. */
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    send_plain(req, (const char *)pocketwiki_style_css, len);
    return ESP_OK;
}

static esp_err_t handle_health(httpd_req_t *req)
{
    const content_archive_t *ca = ca_get();
    const char *body = ca->valid
        ? "ok\n"
        : "ok (content archive not loaded)\n";
    httpd_resp_set_status(req, "200 OK");
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    send_plain(req, body, strlen(body));
    return ESP_OK;
}

static esp_err_t handle_stats(httpd_req_t *req)
{
    const content_archive_t *ca = ca_get();
    char buf[700];
    wifi_station_status_t station;
    wifi_ap_get_station(&station);
    char station_ssid[70];
    json_escape(station.ssid, station_ssid, sizeof station_ssid);
    size_t pack_total = 0, pack_used = 0;
    pack_store_usage(&pack_total, &pack_used);
    int enabled_packs = pack_store_count() + 1;
    int n = snprintf(buf, sizeof buf,
                     "{\"articles\":%lu,\"format_version\":%u,\"archive_ok\":%s,"
                     "\"archive_source\":\"%s\",\"enabled_packs\":%d,"
                     "\"wifi_connected\":%s,\"wifi_ssid\":\"%s\",\"wifi_ip\":\"%s\","
                     "\"clock_valid\":%s,"
                     "\"pack_bytes_used\":%u,\"pack_bytes_total\":%u,"
                     "\"content_bytes\":%lu,\"index_bytes\":%lu,"
                     "\"clients\":%lu,\"ble_on\":%s,"
                     "\"heap_free\":%lu,\"heap_min\":%lu,\"heap_largest\":%lu,"
                     "\"decoder_workspace_bytes\":%lu,\"decoder_queue_depth\":%u,"
                     "\"uptime_s\":%llu}\n",
                     (unsigned long)ca->count, ca->format_version,
                     ca->valid ? "true" : "false",
                     ca->from_file ? "pack" : PW_BUILTIN_PACK_NAME, enabled_packs,
                     station.connected ? "true" : "false", station_ssid, station.ip,
                     wifi_ap_clock_valid() ? "true" : "false",
                     (unsigned)pack_used, (unsigned)pack_total,
                     (unsigned long)ca->payload_size,
                     (unsigned long)ca->index_size,
                     (unsigned long)wifi_ap_get_client_count(),
                     ble_provisioning_active() ? "true" : "false",
                     (unsigned long)esp_get_free_heap_size(),
                     (unsigned long)esp_get_minimum_free_heap_size(),
                     (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                     (unsigned long)sizeof s_decoder,
                     (unsigned)0,
                     (unsigned long long)(esp_timer_get_time() / 1000000ULL));
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    if (n < 0) return ESP_FAIL;
    size_t body_len = (size_t)n < sizeof buf ? (size_t)n : sizeof buf - 1;
    send_plain(req, buf, body_len);
    return ESP_OK;
}

static esp_err_t handle_404(httpd_req_t *req)
{
    return send_error(req, 404, "404 Not Found", "Not found",
                      "There is no such page on this device.");
}

/* ---- provisioning + internal packs ------------------------------------ */

static esp_err_t json_error(httpd_req_t *req, const char *status, const char *message)
{
    char body[160];
    char escaped[128];
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    json_escape(message, escaped, sizeof escaped);
    snprintf(body, sizeof body, "{\"ok\":false,\"error\":\"%s\"}\n", escaped);
    return send_plain(req, body, strlen(body));
}

static bool query_value(httpd_req_t *req, const char *key, char *out, size_t cap)
{
    char raw[192];
    size_t len = httpd_req_get_url_query_len(req);
    return len > 0 && len < sizeof raw &&
           httpd_req_get_url_query_str(req, raw, sizeof raw) == ESP_OK &&
           httpd_query_key_value(raw, key, out, cap) == ESP_OK;
}

/* httpd_query_key_value() copies query values verbatim; decode percent
 * escapes (and '+' back to space) in place. */
static void url_decode(char *s)
{
    size_t out = 0;
    for (size_t i = 0; s[i] != '\0'; i++) {
        char c = s[i];
        if (c == '+') {
            s[out++] = ' ';
        } else if (c == '%' &&
                   isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2])) {
            unsigned hi = (unsigned char)s[i + 1], lo = (unsigned char)s[i + 2];
            unsigned hv = hi <= '9' ? hi - '0' : (hi | 0x20) - 'a' + 10;
            unsigned lv = lo <= '9' ? lo - '0' : (lo | 0x20) - 'a' + 10;
            s[out++] = (char)(hv * 16 + lv);
            i += 2;
        } else {
            s[out++] = c;
        }
    }
    s[out] = '\0';
}

static esp_err_t handle_wifi_scan(httpd_req_t *req)
{
    wifi_scan_item_t items[16];
    int count = wifi_ap_scan(items, 16);
    if (count < 0) return json_error(req, "503 Service Unavailable",
                                     "Could not scan nearby networks. Try again.");
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    SEND_LITERAL(req, "{\"networks\":[");
    char body[128];
    for (int i = 0; i < count; i++) {
        char escaped[70];
        json_escape(items[i].ssid, escaped, sizeof escaped);
        int n = snprintf(body, sizeof body,
            "%s{\"ssid\":\"%s\",\"rssi\":%d,\"open\":%s}", i ? "," : "",
            escaped, items[i].rssi, items[i].open ? "true" : "false");
        send_body(req, body, (size_t)n);
    }
    SEND_LITERAL(req, "]}\n");
    return finish_response(req);
}

static esp_err_t handle_wifi_status(httpd_req_t *req)
{
    wifi_station_status_t station;
    wifi_ap_get_station(&station);
    char station_ssid[70], ap_ssid[70];
    json_escape(station.ssid, station_ssid, sizeof station_ssid);
    json_escape(CONFIG_POCKETWIKI_AP_SSID, ap_ssid, sizeof ap_ssid);
    char ap_ip[16];
    if (wifi_ap_get_ip(ap_ip, sizeof ap_ip) != ESP_OK) strlcpy(ap_ip, "-", sizeof ap_ip);
    char buf[320];
    int n = snprintf(buf, sizeof buf,
                     "{\"ap_ssid\":\"%s\",\"ap_ip\":\"%s\",\"clients\":%lu,"
                     "\"connected\":%s,\"ssid\":\"%s\",\"ip\":\"%s\","
                     "\"rssi\":%d,\"reason\":%u,\"uplink_suspended\":%s}\n",
                     ap_ssid, ap_ip, (unsigned long)wifi_ap_get_client_count(),
                     station.connected ? "true" : "false", station_ssid, station.ip,
                     station.rssi, (unsigned)station.disconnect_reason,
                     station.uplink_suspended ? "true" : "false");
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    if (n < 0) return ESP_FAIL;
    return send_plain(req, buf, (size_t)n);
}

static esp_err_t handle_wifi_config(httpd_req_t *req)
{
    if (req->content_len < 2 || req->content_len > 256) {
        return json_error(req, "400 Bad Request", "The Wi-Fi request was not valid.");
    }
    char body[257];
    int total = 0;
    while (total < req->content_len) {
        int got = httpd_req_recv(req, body + total, req->content_len - total);
        if (got <= 0) return json_error(req, "400 Bad Request",
                                        "The Wi-Fi request was incomplete.");
        total += got;
    }
    body[total] = 0;
    cJSON *json = cJSON_Parse(body);
    cJSON *ssid = json ? cJSON_GetObjectItemCaseSensitive(json, "ssid") : NULL;
    cJSON *password = json ? cJSON_GetObjectItemCaseSensitive(json, "password") : NULL;
    if (!cJSON_IsString(ssid) || !cJSON_IsString(password)) {
        cJSON_Delete(json);
        return json_error(req, "400 Bad Request",
                          "A Wi-Fi network name and password are required.");
    }
    esp_err_t err = wifi_ap_set_station(ssid->valuestring, password->valuestring);
    cJSON_Delete(json);
    if (err != ESP_OK) return json_error(req, "400 Bad Request",
                                         "Could not save the Wi-Fi details. Check the network name and password.");
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    return SEND_LITERAL(req, "{\"ok\":true,\"state\":\"connecting\"}\n");
}

static esp_err_t handle_packs(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    uint32_t built_in_articles = 0;
    bool built_in_updated = false;
    if (pack_store_open_for_read(PW_BUILTIN_PACK_NAME) == ESP_OK) {
        built_in_articles = ca_get()->count;
        built_in_updated = ca_get()->from_file;
    }
    char built_in_title[PW_TITLE_CAP];
    pack_display_name(PW_BUILTIN_PACK_NAME, built_in_title, sizeof built_in_title);
    json_escape(built_in_title, s_render_label_esc, sizeof s_render_label_esc);
    int hlen = snprintf(s_render_item, sizeof s_render_item,
        "{\"packs\":[{\"name\":\"%s\",\"title\":\"%s\",\"articles\":%lu,\"bytes\":0,"
        "\"builtin\":true,\"updated\":%s,\"hidden\":%s,\"enabled\":true}",
        PW_BUILTIN_PACK_NAME, s_render_label_esc,
        (unsigned long)built_in_articles, built_in_updated ? "true" : "false",
        pack_store_guide_hidden() ? "true" : "false");
    send_body(req, s_render_item, (size_t)hlen);
    pack_store_item_t *items = NULL;
    int count = pack_store_list_all(&items);
    char *labels = catalog_labels(items, count);
    for (int i = 0; i < count; i++) {
        char fallback[PW_TITLE_CAP];
        const char *title = pack_label(items[i].name, labels, i, fallback, sizeof fallback);
        json_escape(title, s_render_label_esc, sizeof s_render_label_esc);
        int len = snprintf(s_render_item, sizeof s_render_item,
            ",{\"name\":\"%s\",\"title\":\"%s\",\"articles\":%lu,\"bytes\":%lu,\"enabled\":true}",
            items[i].name, s_render_label_esc, (unsigned long)items[i].articles,
            (unsigned long)items[i].bytes);
        send_body(req, s_render_item, (size_t)len);
    }
    free(labels);
    free(items);
    size_t total = 0, used = 0;
    pack_store_usage(&total, &used);
    size_t installable = pack_store_installable_bytes();
    char tail[160];
    int tlen = snprintf(tail, sizeof tail,
                        "],\"bytes_used\":%u,\"bytes_total\":%u,\"bytes_free\":%u,\"bytes_installable\":%u}\n",
                        (unsigned)used, (unsigned)total,
                        (unsigned)(total > used ? total - used : 0), (unsigned)installable);
    send_body(req, tail, (size_t)tlen);
    esp_err_t result = finish_response(req);
    catalog_cache_invalidate();
    return result;
}

/* Show or hide the built-in library in the reader's list. A plain form posts
 * here, so the toggle works without any client script; the redirect puts the
 * manage page back with the new state. */
static esp_err_t handle_guide_toggle(httpd_req_t *req)
{
    char scratch[64];
    int remaining = req->content_len;
    while (remaining > 0) {
        int want = remaining > (int)sizeof scratch ? (int)sizeof scratch : remaining;
        int got = httpd_req_recv(req, scratch, want);
        if (got <= 0) break;
        remaining -= got;
    }
    if (pack_store_set_guide_hidden(!pack_store_guide_hidden()) != ESP_OK) {
        return send_error(req, 500, "500 Internal Server Error", "Could not update",
                          "The library list could not be changed.");
    }
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/manage");
    return send_plain(req, NULL, 0);
}

static esp_err_t handle_pack_action(httpd_req_t *req)
{
    char action[16], name[PW_PACK_NAME_MAX + 1];
    if (!query_value(req, "action", action, sizeof action) ||
        !query_value(req, "name", name, sizeof name)) {
        return json_error(req, "400 Bad Request", "A pack action and pack name are required.");
    }
    esp_err_t err = strcmp(action, "delete") == 0 ? pack_store_remove(name) : ESP_ERR_INVALID_ARG;
    if (err != ESP_OK) return json_error(req, "400 Bad Request",
                                         "That pack could not be removed.");
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    return send_plain(req, "{\"ok\":true}\n", sizeof("{\"ok\":true}\n") - 1);
}

static esp_err_t handle_pack_upload(httpd_req_t *req)
{
    char name[PW_PACK_NAME_MAX + 1];
    if (!query_value(req, "name", name, sizeof name) || !pack_store_safe_name(name)) {
        return json_error(req, "400 Bad Request", "Please choose a pack with a simple file name.");
    }
    if (req->content_len <= 0 || req->content_len > CONFIG_POCKETWIKI_PACK_MAX_UPLOAD_KB * 1024) {
        return json_error(req, "413 Payload Too Large", "That pack is too large for this device.");
    }
    size_t installable = pack_store_installable_bytes();
    if ((size_t)req->content_len > installable) {
        char message[120];
        snprintf(message, sizeof message,
                 "Not enough storage: this pack needs %u bytes, but only %u bytes are available.",
                 (unsigned)req->content_len, (unsigned)installable);
        return json_error(req, "507 Insufficient Storage", message);
    }
    FILE *fh = NULL;
    char temp[40];
    esp_err_t err = pack_store_begin_upload(name, &fh, temp, sizeof temp);
    if (err != ESP_OK) return json_error(req, "507 Insufficient Storage",
                                          "PocketWiki could not prepare its pack storage.");
    size_t flash_total = 0, flash_used = 0;
    pack_store_usage(&flash_total, &flash_used);
    oled_show_transfer(name, 0, (size_t)req->content_len, flash_used, flash_total);
    uint8_t buffer[2048];
    int remaining = req->content_len;
    size_t received = 0;
    size_t next_display = 8192;
    while (remaining > 0) {
        int want = remaining > (int)sizeof buffer ? (int)sizeof buffer : remaining;
        int got = httpd_req_recv(req, (char *)buffer, want);
        if (got <= 0) {
            pack_store_abort_upload(fh, temp);
            oled_clear_transfer();
            return json_error(req, "400 Bad Request", "The pack upload was interrupted.");
        }
        if (fwrite(buffer, 1, got, fh) != (size_t)got) {
            pack_store_abort_upload(fh, temp);
            oled_clear_transfer();
            return json_error(req, "507 Insufficient Storage",
                              "Storage filled while writing the pack.");
        }
        remaining -= got;
        received += (size_t)got;
        if (received >= next_display || remaining == 0) {
            oled_show_transfer(name, received, (size_t)req->content_len,
                               flash_used + received, flash_total);
            next_display = received + 8192;
        }
    }
    uint32_t articles = 0;
    err = pack_store_finish_upload(fh, temp, name, &articles);
    if (err != ESP_OK) {
        /* A pack can be refused for reasons only the archive layer knows
         * (codec, dictionary size), so it supplies the wording. */
        oled_clear_transfer();
        return json_error(req, "422 Unprocessable Content", ca_last_reject_reason());
    }
    oled_clear_transfer();
    char body[96];
    snprintf(body, sizeof body, "{\"ok\":true,\"articles\":%lu}\n", (unsigned long)articles);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    return send_plain(req, body, strlen(body));
}

/* The device keeps a copy of the live pack catalogue (fetched by
 * handle_packs_catalog_sync) so the dashboard shows fresh packs even when the
 * browser has no internet of its own, e.g. joined to the PocketWiki access
 * point. The build-time embedded catalogue remains the fallback. */
#define CATALOG_FILE "/packs/.catalog.json"
#define CATALOG_FILE_MAX 262144
#define CATALOG_LIVE_URL "https://packs.educated.space/index.json"

/* Defined below; used by the catalogue-sync and pack-install fetchers. */
static void wdt_relieve(bool relieve);

typedef enum {
    CATALOG_SOURCE_EMBEDDED = 0,
    CATALOG_SOURCE_FILE = 1
} catalog_source_t;

/* The active catalogue is the synced file or the embedded asset, whichever is
 * newer. Nothing is held in RAM: a lookup streams the document once and keeps a
 * single pack entry, so the catalogue can grow with the library instead of
 * being capped by static storage. The previous implementation kept a fixed
 * array of entries, which the C3 cannot size for a catalogue of this size. */
static bool s_catalog_cache_ready;
static bool s_catalog_cache_ok;
static catalog_source_t s_catalog_source;

static int catalog_file_read(void *ctx, char *buf, size_t cap)
{
    FILE *fh = ctx;
    size_t n = fread(buf, 1, cap, fh);
    if (n == 0 && ferror(fh)) return -1;
    return (int)n;
}

static int catalog_asset_read(void *ctx, char *buf, size_t cap)
{
    size_t *offset = ctx;
    if (*offset >= pocketwiki_catalog_json_len) return 0;
    size_t n = pocketwiki_catalog_json_len - *offset;
    if (n > cap) n = cap;
    memcpy(buf, pocketwiki_catalog_json + *offset, n);
    *offset += n;
    return (int)n;
}

static bool catalog_header_from_file(pw_catalog_header_t *header)
{
    FILE *fh = fopen(CATALOG_FILE, "rb");
    if (fh == NULL) return false;
    bool ok = pw_catalog_read_header(catalog_file_read, fh, header);
    fclose(fh);
    return ok;
}

static bool catalog_header_from_asset(pw_catalog_header_t *header)
{
    size_t offset = 0;
    return pw_catalog_read_header(catalog_asset_read, &offset, header);
}

/* Select the newer catalogue once. A synced file wins on catalog_version, then
 * on generated_at; an equal timestamp keeps the deterministic firmware asset. */
static bool catalog_cache_ensure(void)
{
    if (s_catalog_cache_ready) return s_catalog_cache_ok;

    pw_catalog_header_t file_header = { 0 };
    pw_catalog_header_t asset_header = { 0 };
    bool have_file = catalog_header_from_file(&file_header);
    bool have_asset = catalog_header_from_asset(&asset_header);

    bool use_file = have_file &&
        (!have_asset || file_header.version > asset_header.version ||
         (file_header.version == asset_header.version &&
          strcmp(file_header.generated_at, asset_header.generated_at) > 0));

    /* The header is read here, so a synced file that is not a schema 1
     * catalogue falls back to the firmware asset instead of leaving the
     * dashboard with nothing. Entries are validated when they are used. */
    bool ok = use_file ? have_file : have_asset;
    s_catalog_cache_ready = true;
    s_catalog_cache_ok = ok;
    s_catalog_source = (ok && use_file) ? CATALOG_SOURCE_FILE : CATALOG_SOURCE_EMBEDDED;
    if (!ok) ESP_LOGW(TAG, "no usable catalogue in the synced file or the firmware asset");
    return ok;
}

static void catalog_cache_invalidate(void)
{
    s_catalog_cache_ready = false;
    s_catalog_cache_ok = false;
}

/* One pack from the active catalogue: by id, by url, or either. A synced file
 * that disappears between selection and use falls back to the firmware asset,
 * so a lookup never fails on a catalogue the device already accepted. */
static bool catalog_find(const char *id, const char *url, pw_catalog_pack_t *out)
{
    if (!catalog_cache_ensure()) return false;
    if (s_catalog_source == CATALOG_SOURCE_FILE) {
        FILE *fh = fopen(CATALOG_FILE, "rb");
        if (fh != NULL) {
            bool ok = pw_catalog_find(catalog_file_read, fh, id, url, out);
            fclose(fh);
            if (ok) return true;
        } else {
            ESP_LOGW(TAG, "catalogue: synced file unreadable at lookup time; using the asset");
        }
    }
    size_t offset = 0;
    return pw_catalog_find(catalog_asset_read, &offset, id, url, out);
}

/* Display names for several packs, in one pass over the active catalogue. */
static int catalog_names(pw_catalog_name_slot_t *slots, int count)
{
    if (!catalog_cache_ensure()) return 0;
    if (s_catalog_source == CATALOG_SOURCE_FILE) {
        FILE *fh = fopen(CATALOG_FILE, "rb");
        if (fh != NULL) {
            int filled = pw_catalog_names(catalog_file_read, fh, slots, count);
            fclose(fh);
            if (filled > 0) return filled;
        }
    }
    size_t offset = 0;
    return pw_catalog_names(catalog_asset_read, &offset, slots, count);
}

static bool catalog_pack_name(const char *id, char *out, size_t cap)
{
    if (id == NULL || out == NULL || cap == 0) return false;
    pw_catalog_pack_t pack;
    if (!catalog_find(id, NULL, &pack) || pack.name[0] == '\0') return false;
    strlcpy(out, pack.name, cap);
    return out[0] != '\0';
}

/* Human-readable name for an installed pack: the catalogue's name when the
 * catalogue has one, then the built-in defaults, then a title-cased id. */
static void pack_display_name(const char *id, char *out, size_t cap)
{
    if (catalog_pack_name(id, out, cap)) return;
    if (strcasecmp(id, PW_BUILTIN_PACK_NAME) == 0 || strcasecmp(id, "starter") == 0) {
        strlcpy(out, "PocketWiki Guide", cap);
        return;
    }
    strlcpy(out, id, cap);
    bool capitalize = true;
    for (size_t i = 0; out[i]; i++) {
        if (out[i] == '-' || out[i] == '_') {
            out[i] = ' ';
            capitalize = true;
        } else if (capitalize && out[i] >= 'a' && out[i] <= 'z') {
            out[i] = (char)(out[i] - ('a' - 'A'));
            capitalize = false;
        } else if (out[i] != ' ') {
            capitalize = false;
        }
    }
}

/* Longest display name a pack label needs: the catalogue caps a name at 48. */
#define PW_PACK_LABEL_CAP 64

/* Catalogue names for the installed packs, in one pass over the document. Entry
 * i holds the catalogue name of items[i], and is empty for a pack the catalogue
 * does not carry. NULL means the catalogue is unavailable, so the caller falls
 * back to pack_display_name per pack. */
static char *catalog_labels(const pack_store_item_t *items, int count)
{
    if (items == NULL || count <= 0) return NULL;
    char *labels = calloc((size_t)count, PW_PACK_LABEL_CAP);
    pw_catalog_name_slot_t *slots = calloc((size_t)count, sizeof *slots);
    if (labels == NULL || slots == NULL) {
        free(labels);
        free(slots);
        return NULL;
    }
    for (int i = 0; i < count; i++) {
        slots[i].id = items[i].name;
        slots[i].out = labels + (size_t)i * PW_PACK_LABEL_CAP;
        slots[i].cap = PW_PACK_LABEL_CAP;
    }
    catalog_names(slots, count);
    free(slots);
    return labels;
}

static const char *pack_label(const char *id, const char *labels, int index,
                              char *fallback, size_t cap)
{
    const char *name = labels == NULL ? NULL : labels + (size_t)index * PW_PACK_LABEL_CAP;
    if (name != NULL && name[0] != '\0') return name;
    pack_display_name(id, fallback, cap);
    return fallback;
}

/* Expected size and digest for one pack URL, used to verify an install. */
static bool catalog_lookup(const char *url, int64_t *bytes, char sha_hex[65])
{
    if (url == NULL) return false;
    pw_catalog_pack_t pack;
    if (!catalog_find(NULL, url, &pack)) return false;
    if (bytes != NULL) *bytes = pack.bytes;
    if (sha_hex != NULL) strlcpy(sha_hex, pack.sha256, PW_CATALOG_SHA_CAP);
    return true;
}

/* Stream the active catalogue back to a client, in bounded pieces and without
 * ever holding it in RAM. A synced file that disappears between selection and
 * serving falls back to the firmware asset: returning without a response would
 * leave the client waiting on a connection this server never answers. */
static esp_err_t catalog_stream_to_client(httpd_req_t *req)
{
    esp_err_t result = ESP_OK;
    size_t sent = 0;
    if (s_catalog_source == CATALOG_SOURCE_FILE) {
        FILE *fh = fopen(CATALOG_FILE, "rb");
        if (fh == NULL) {
            ESP_LOGW(TAG, "catalogue: synced file unreadable at serve time; using the asset");
        } else {
            char buf[PW_CHUNK_SIZE];
            for (;;) {
                size_t n = fread(buf, 1, sizeof buf, fh);
                if (n == 0) break;
                result = send_body(req, buf, n);
                if (result != ESP_OK) break;
                sent += n;
            }
            if (ferror(fh) && result == ESP_OK) result = ESP_FAIL;
            fclose(fh);
            ESP_LOGI(TAG, "catalogue: served %u bytes from the synced file (result=%d)",
                     (unsigned)sent, (int)result);
            return result;
        }
    }
    size_t offset = 0;
    char buf[PW_CHUNK_SIZE];
    for (;;) {
        int n = catalog_asset_read(&offset, buf, sizeof buf);
        if (n <= 0) break;
        result = send_body(req, buf, (size_t)n);
        if (result != ESP_OK) break;
        sent += (size_t)n;
    }
    ESP_LOGI(TAG, "catalogue: served %u bytes from the firmware asset (asset=%u, result=%d)",
             (unsigned)sent, (unsigned)pocketwiki_catalog_json_len, (int)result);
    return result;
}

/* Serve the pack catalogue: the synced live copy when present, else the
 * catalogue embedded at build time. */
static esp_err_t handle_packs_catalog(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    if (!catalog_cache_ensure()) {
        return json_error(req, "500 Internal Server Error", "catalogue unavailable");
    }
    /* A large body goes out as bounded chunks so a client that stops reading
     * is retired after one write instead of after the whole catalogue. The
     * index stays loaded: it holds no heap and is shared by display names and
     * install verification. Chunked encoding has no length, so the response
     * must be closed explicitly — without this the client waits forever. */
    esp_err_t err = catalog_stream_to_client(req);
    if (err != ESP_OK) return err;
    return finish_response(req);
}

/* Device-side catalogue sync: POST /api/packs/catalog/sync
 *
 * Fetches the live catalogue over the device's own station-mode internet
 * connection, validates it, and stores it in the pack flash filesystem
 * (temp file + rename so a power loss never corrupts the previous copy).
 * Responds with the fetched catalogue. The synced copy is what the dashboard
 * and install verification use until the next sync. */
static esp_err_t handle_packs_catalog_sync(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Connection", "close");

    esp_http_client_config_t cfg = {
        .url = CATALOG_LIVE_URL,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 20000,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
        .user_agent = "PocketWiki/1.0",
        .crt_bundle_attach = esp_crt_bundle_attach,
        .max_redirection_count = 4,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    int64_t got = 0;
    bool ble_suspended = false;
    const char *fail = NULL;
    {
        wifi_station_status_t st;
        wifi_ap_get_station(&st);
        if (st.uplink_suspended) {
            fail = "The saved Wi-Fi network was dropped because its signal is too weak to share the radio with PocketWiki's own network. Move the device closer to that network, or set up a different 2.4 GHz network under Manage.";
            goto sync_done;
        }
        if (!st.connected) {
            fail = "PocketWiki is not connected to Wi-Fi. Set up a 2.4 GHz network under Manage and try again.";
            goto sync_done;
        }
    }
    ESP_LOGI(TAG, "catalogue sync: waiting for the clock, heap=%lu",
             (unsigned long)esp_get_free_heap_size());
    if (wifi_ap_wait_for_clock(15000) != ESP_OK) {
        fail = "PocketWiki cannot verify the connection time. Check internet access and try again.";
        goto sync_done;
    }
    {
        esp_err_t ble_err = ble_provisioning_suspend();
        if (ble_err != ESP_OK) {
            if (ble_err == ESP_ERR_INVALID_STATE) {
                fail = "Disconnect Bluetooth before refreshing the catalogue.";
            } else {
                fail = "Bluetooth could not be paused for the catalogue refresh. Try again shortly.";
            }
            goto sync_done;
        }
        ble_suspended = true;
    }
    wdt_relieve(true);
    if (client == NULL) {
        fail = "PocketWiki could not start the catalogue download.";
        goto sync_done;
    }
    ESP_LOGI(TAG, "catalogue sync: fetching %s, heap=%lu", CATALOG_LIVE_URL,
             (unsigned long)esp_get_free_heap_size());
    {
        esp_err_t oerr = esp_http_client_open(client, 0);
        int64_t hlen = oerr == ESP_OK ? esp_http_client_fetch_headers(client) : -1;
        int status = oerr == ESP_OK ? esp_http_client_get_status_code(client) : 0;
        ESP_LOGI(TAG, "catalogue sync: open=%s status=%d length=%lld",
                 esp_err_to_name(oerr), status, (long long)hlen);
        if (oerr != ESP_OK || hlen < 0 || status != 200) {
            ESP_LOGE(TAG, "catalog sync: open=%s headers=%lld status=%d",
                     esp_err_to_name(oerr), (long long)hlen, status);
            fail = "The catalogue server could not be reached. Check internet access and try again.";
            goto sync_done;
        }
    }
    int64_t total = esp_http_client_get_content_length(client);
    if (total < 1 || total > CATALOG_FILE_MAX) {
        fail = "The catalogue is too large or did not report its size.";
        goto sync_done;
    }
    /* Stream straight to flash: the document plus a parsed tree does not fit
     * the C3 heap, and the index is built from the file afterwards. */
    FILE *out = fopen("/packs/.catalog.tmp", "wb");
    if (out == NULL) {
        fail = "PocketWiki could not save the refreshed catalogue.";
        goto sync_done;
    }
    {
        char chunk[1024];
        while (got < total) {
            int want = (int)(total - got);
            if (want > (int)sizeof chunk) want = (int)sizeof chunk;
            int n = esp_http_client_read(client, chunk, want);
            if (n <= 0) break;
            if (fwrite(chunk, 1, (size_t)n, out) != (size_t)n) {
                fclose(out);
                unlink("/packs/.catalog.tmp");
                fail = "PocketWiki could not save the refreshed catalogue.";
                goto sync_done;
            }
            got += n;
            if (esp_task_wdt_status(NULL) == ESP_OK) esp_task_wdt_reset();
        }
    }
    fclose(out);
    if (got != total) {
        unlink("/packs/.catalog.tmp");
        fail = "The catalogue download was incomplete. Try again.";
        goto sync_done;
    }
    {
        FILE *check = fopen("/packs/.catalog.tmp", "rb");
        bool valid = check != NULL && pw_catalog_validate(catalog_file_read, check);
        if (check != NULL) fclose(check);
        if (!valid) {
            unlink("/packs/.catalog.tmp");
            ESP_LOGW(TAG, "catalogue sync: downloaded document rejected (%lld bytes)", (long long)got);
            fail = "The downloaded catalogue failed validation.";
            goto sync_done;
        }
    }
    {
        /* FATFS rename does not replace an existing file on all targets.
         * Remove only the old catalogue after the complete temporary file is
         * safely written; the embedded catalogue remains the fallback if the
         * subsequent rename is interrupted. */
        if (unlink(CATALOG_FILE) != 0 && errno != ENOENT) {
            unlink("/packs/.catalog.tmp");
            fail = "PocketWiki could not replace the refreshed catalogue.";
            goto sync_done;
        }
        if (rename("/packs/.catalog.tmp", CATALOG_FILE) != 0) {
            unlink("/packs/.catalog.tmp");
            fail = "PocketWiki could not save the refreshed catalogue.";
            goto sync_done;
        }
        /* The index was filled from the validated temporary file above. */
        s_catalog_cache_ready = true;
        s_catalog_cache_ok = true;
        s_catalog_source = CATALOG_SOURCE_FILE;
    }
    ESP_LOGI(TAG, "catalogue synced (%lld bytes), heap=%lu", (long long)got,
             (unsigned long)esp_get_free_heap_size());

sync_done:
    if (client != NULL) esp_http_client_cleanup(client);
    if (ble_suspended) {
        esp_err_t berr = ble_provisioning_resume();
        if (berr != ESP_OK) ESP_LOGE(TAG, "catalogue sync: BLE resume failed: %s", esp_err_to_name(berr));
    }
    wdt_relieve(false);
    if (fail != NULL) {
        ESP_LOGW(TAG, "catalogue sync failed: %s", fail);
        httpd_resp_set_status(req, "502 Bad Gateway");
        return send_plain(req, fail, strlen(fail));
    }
    /* Same chunked response as GET /api/packs/catalog: it needs its closing
     * terminator or the client hangs on a body it never sees the end of. */
    esp_err_t stream_err = catalog_stream_to_client(req);
    if (stream_err != ESP_OK) return stream_err;
    return finish_response(req);
}

/* The httpd task subscribes to the Task WDT (5 s) and feeds it between
 * streaming chunks. A blocking TLS download can stall longer than that
 * (per-read socket timeout is 15-20 s), which reboots the device mid-install.
 * Relieve the calling task around blocking fetches and re-subscribe after. */
static bool s_wdt_relieved;

static void wdt_relieve(bool relieve)
{
    if (relieve && !s_wdt_relieved) {
        if (esp_task_wdt_status(NULL) == ESP_OK) {
            esp_task_wdt_delete(NULL);
            s_wdt_relieved = true;
        }
    } else if (!relieve && s_wdt_relieved) {
        esp_task_wdt_add(NULL);
        s_wdt_relieved = false;
    }
}


/* After httpd has sent the response headers, queue one complete HTTP chunk.
 * Never block the upstream download on the browser. A short socket write must
 * retain its unsent suffix; dropping it corrupts HTTP chunk framing. */
typedef struct {
    char data[96];
    size_t length;
    size_t offset;
    bool failed;
} install_progress_t;

static void install_progress_flush(httpd_req_t *req, install_progress_t *p, int flags)
{
    while (!p->failed && p->offset < p->length) {
        int n = send(httpd_req_to_sockfd(req), p->data + p->offset,
                     p->length - p->offset, flags);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && flags) return;
        if (n <= 0) { p->failed = true; return; }
        p->offset += (size_t)n;
    }
}

static void install_progress_send(httpd_req_t *req, install_progress_t *p,
                                  int64_t received, int64_t total)
{
    install_progress_flush(req, p, MSG_DONTWAIT);
    if (p->failed || p->offset < p->length) return;
    char line[64];
    int len = snprintf(line, sizeof line, "P:%lld:%lld\n",
                       (long long)received, (long long)total);
    p->length = (size_t)snprintf(p->data, sizeof p->data, "%x\r\n%s\r\n", len, line);
    p->offset = 0;
    install_progress_flush(req, p, MSG_DONTWAIT);
}

/* Device-side catalogue install: POST /api/packs/install?url=…&name=…
 *
 * The device downloads the pack over its own station-mode internet
 * connection, verifies size and SHA-256 against the embedded catalogue when
 * the URL is curated, and atomically installs it. This keeps the dashboard
 * usable when the browser is joined to the PocketWiki access point and has no
 * internet of its own. The response is chunked plain text: progress lines
 * "P:<received bytes>:<total bytes>\n" while downloading, then
 * "OK:<articles>\n" or "ERR:<message>\n". Received and total are the exact
 * values handed to oled_show_transfer(), keeping both displays in lockstep.
 */
static esp_err_t handle_pack_install(httpd_req_t *req)
{
    char url[512];
    char name[PW_PACK_NAME_MAX + 1];
    if (!query_value(req, "url", url, sizeof url)) {
        return json_error(req, "400 Bad Request", "Enter a secure HTTPS URL for the pack.");
    }
    url_decode(url);
    if (strncmp(url, "https://", 8) != 0) {
        return json_error(req, "400 Bad Request", "Enter a secure HTTPS URL for the pack.");
    }
    if (!query_value(req, "name", name, sizeof name) || !pack_store_safe_name(name)) {
        /* Derive a safe name from the URL filename. */
        const char *base = strrchr(url, '/');
        base = (base != NULL) ? base + 1 : url;
        strlcpy(name, base, sizeof name);
        size_t n = strlen(name);
        if (n > 4 && strcmp(name + n - 4, ".pwp") == 0) name[n - 4] = '\0';
        for (char *p = name; *p; p++) {
            char c = *p;
            *p = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '-' || c == '_') ? c : '-';
        }
        if (!pack_store_safe_name(name)) {
            return json_error(req, "400 Bad Request", "PocketWiki could not derive a valid name for that pack.");
        }
    } else {
        url_decode(name);
    }

    int64_t expected_bytes = -1;
    char sha_hex[65] = "";
    catalog_lookup(url, &expected_bytes, sha_hex);
    /* The catalogue tree pins tens of KB of heap. Expected size/hash are
     * already copied out, so drop it for the TLS fetch: mbedtls needs large
     * contiguous blocks and the C3 heap is too fragmented otherwise. The
     * cache re-parses on next use. */
    catalog_cache_invalidate();
    /* Close any pack archive currently held open for streaming: the finish
     * path unlinks the existing pack file, and FAT refuses to unlink a file
     * that is open. Article reads serve the built-in archive meanwhile. */
    pack_store_restore_builtin();

    size_t installable = pack_store_installable_bytes();
    if (expected_bytes > 0 && (uint64_t)expected_bytes > installable) {
        char message[120];
        snprintf(message, sizeof message,
                 "Not enough storage: this pack needs %llu bytes, but only %u bytes are available.",
                 (unsigned long long)expected_bytes, (unsigned)installable);
        return json_error(req, "507 Insufficient Storage", message);
    }
    /* Wait for SNTP before creating the upload temp file or switching the
     * response to the chunked progress protocol. */
    /* The dashboard is usually browsed over the AP, which works with no
     * uplink. The pack fetch needs the station: without it the TCP connect
     * fails and the GUI reports a cryptic download error. */
    {
        wifi_station_status_t st;
        wifi_ap_get_station(&st);
        if (!st.connected) {
            return json_error(req, "503 Service Unavailable",
                              "PocketWiki is not connected to Wi-Fi. Set up a 2.4 GHz network under Manage and try again.");
        }
    }
    if (wifi_ap_wait_for_clock(15000) != ESP_OK) {
        return json_error(req, "503 Service Unavailable",
                          "PocketWiki cannot verify the connection time. Check internet access and try again.");
    }

    FILE *fh = NULL;
    char temp[40];
    bool ble_suspended = false;
    if (pack_store_begin_upload(name, &fh, temp, sizeof temp) != ESP_OK) {
        return json_error(req, "507 Insufficient Storage",
                          "PocketWiki could not prepare its pack storage.");
    }

    /* On the single-core C3 the always-on BLE controller consumes enough
     * internal heap to prevent IDF from reserving a standard 16 KiB TLS
     * record. A web download and a BLE session are mutually exclusive; stop
     * NimBLE for this transfer and bring it back before the response ends. */
    esp_err_t ble_err = ble_provisioning_suspend();
    if (ble_err != ESP_OK) {
        pack_store_abort_upload(fh, temp);
        if (ble_err == ESP_ERR_INVALID_STATE) {
            return json_error(req, "409 Conflict",
                              "Disconnect Bluetooth before starting a web download.");
        }
        return json_error(req, "503 Service Unavailable",
                          "Bluetooth could not be paused for the web download. Try again shortly.");
    }
    ble_suspended = true;

    /* From here the response streams progress; always end with OK:/ERR:. */
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    /* Anchor the bar at 0% before the (potentially slow) TLS fetch begins, so
     * the browser's first progress event is never a mid-download jump. */
    install_progress_t progress = {0};
    progress.failed = SEND_LITERAL(req, "P:0:0\n") != ESP_OK;

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 30000,
        /* Keep the HTTP staging buffers small while the C3 retains a
         * contiguous heap block for a full standard TLS record. */
        .buffer_size = 1024,
        .buffer_size_tx = 512,
        .user_agent = "PocketWiki/1.0",
        .crt_bundle_attach = esp_crt_bundle_attach,
        .max_redirection_count = 4,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    psa_hash_operation_t hash = PSA_HASH_OPERATION_INIT;
    bool verify_hash = sha_hex[0] != '\0';
    bool hashing = false;
    char fail[128] = "";
    int64_t received = 0;
    int64_t total = -1;
    uint8_t *buffer = NULL;
    int64_t last_progress_us = 0;
    unsigned read_timeouts = 0;
    unsigned reconnects = 0;
    wdt_relieve(true);

    if (client == NULL) {
        ESP_LOGE(TAG, "install: http client init failed");
        strlcpy(fail, "PocketWiki could not start the download.", sizeof fail);
        goto install_done;
    }
    esp_http_client_set_header(client, "Accept-Encoding", "identity");
    ESP_LOGI(TAG, "install: heap=%lu largest=%lu url=%.80s",
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT), url);
    {
        esp_err_t oerr = esp_http_client_open(client, 0);
        int64_t hlen = oerr == ESP_OK ? esp_http_client_fetch_headers(client) : -1;
        int status = oerr == ESP_OK ? esp_http_client_get_status_code(client) : 0;
        if (oerr != ESP_OK || hlen < 0 || status != 200) {
            int tls_code = 0, tls_flags = 0;
            esp_err_t tls_err = esp_http_client_get_and_clear_last_tls_error(
                client, &tls_code, &tls_flags);
            ESP_LOGE(TAG, "install: open=%s headers=%lld status=%d url=%.80s",
                     esp_err_to_name(oerr), (long long)hlen, status, url);
            ESP_LOGE(TAG, "install: errno=%d tls=%s tls_code=0x%x tls_flags=0x%x",
                     esp_http_client_get_errno(client), esp_err_to_name(tls_err),
                     tls_code, tls_flags);
            strlcpy(fail, "The pack server could not be reached. Check internet access and try again.", sizeof fail);
            goto install_done;
        }
    }
    total = expected_bytes > 0 ? expected_bytes : esp_http_client_get_content_length(client);
    if (total > 0) {
        uint64_t cap = (uint64_t)CONFIG_POCKETWIKI_PACK_MAX_UPLOAD_KB * 1024;
        if ((uint64_t)total > cap || (uint64_t)total > installable) {
            strlcpy(fail, "That pack is too large for this device.", sizeof fail);
            goto install_done;
        }
    }
    size_t flash_total = 0, flash_used = 0;
    pack_store_usage(&flash_total, &flash_used);
    oled_show_transfer(name, 0, (size_t)(total > 0 ? total : 0), flash_used, flash_total);
    install_progress_send(req, &progress, 0, total);

    /* A 1 KiB application buffer is sufficient for the streamed response and
     * leaves the contiguous heap needed by a full 16 KiB TLS record. */
    buffer = malloc(1024);
    if (buffer == NULL) {
        strlcpy(fail, "PocketWiki does not have enough memory to download that pack.", sizeof fail);
        goto install_done;
    }
    for (;;) {
        int got = esp_http_client_read(client, (char *)buffer, 1024);
        /* IDF reports a read timeout as -ESP_ERR_HTTP_EAGAIN, not EOF.
         * Allow temporary radio stalls, but bound the idle wait to 90 s. */
        if (got == -ESP_ERR_HTTP_EAGAIN && ++read_timeouts < 3) continue;
        if (got <= 0 && !esp_http_client_is_complete_data_received(client)) {
            /* Prefer resuming curated bodies with a trusted hash, but allow
             * every download to restart from byte 0 safely. The catalogue
             * lookup can miss for a custom URL or an older device catalogue;
             * that must not turn one transient TCP read error into a hard
             * failure. Hosts that ignore Range return 200, so truncate the
             * temporary file and restart the hash before consuming the full
             * response. */
            if (received > 0 && total > received && reconnects++ < 3) {
                char range[64];
                esp_http_client_close(client);
                if (sha_hex[0]) {
                    snprintf(range, sizeof range, "bytes=%lld-", (long long)received);
                    esp_http_client_set_header(client, "Range", range);
                } else {
                    esp_http_client_delete_header(client, "Range");
                }
                esp_err_t err = esp_http_client_open(client, 0);
                int64_t length = err == ESP_OK ? esp_http_client_fetch_headers(client) : -1;
                int status = err == ESP_OK ? esp_http_client_get_status_code(client) : 0;
                if (err == ESP_OK && status == 206 && length == total - received) {
                    read_timeouts = 0;
                    continue;
                }
                if (err == ESP_OK && status == 200 && (length < 0 || length == total)) {
                    if (fflush(fh) != 0 || ftruncate(fileno(fh), 0) != 0 ||
                            fseek(fh, 0, SEEK_SET) != 0) {
                        strlcpy(fail, "PocketWiki could not restart the download.", sizeof fail);
                        goto install_done;
                    }
                    clearerr(fh);
                    received = 0;
                    read_timeouts = 0;
                    last_progress_us = 0;
                    if (hashing) {
                        psa_hash_abort(&hash);
                        hash = (psa_hash_operation_t)PSA_HASH_OPERATION_INIT;
                        if (psa_hash_setup(&hash, PSA_ALG_SHA_256) != PSA_SUCCESS) {
                            strlcpy(fail, "PocketWiki could not verify the pack checksum.", sizeof fail);
                            goto install_done;
                        }
                    }
                    ESP_LOGW(TAG, "install: restarting full download after read interruption");
                    install_progress_send(req, &progress, 0, total);
                    oled_show_transfer(name, 0, (size_t)(total > 0 ? total : 0),
                                       flash_used, flash_total);
                    continue;
                }
            }
            ESP_LOGE(TAG, "install: read failed after %lld bytes (read=%d errno=%d)",
                     (long long)received, got, errno);
            snprintf(fail, sizeof fail, "The pack download was interrupted at %lld bytes.", (long long)received);
            goto install_done;
        }
        if (got < 0) {
            strlcpy(fail, "PocketWiki could not read the pack download.", sizeof fail);
            goto install_done;
        }
        if (got == 0) break;
        read_timeouts = 0;
        if (fwrite(buffer, 1, (size_t)got, fh) != (size_t)got) {
            strlcpy(fail, "Storage filled while downloading the pack.", sizeof fail);
            goto install_done;
        }
        if (hashing && psa_hash_update(&hash, buffer, (size_t)got) != PSA_SUCCESS) {
            strlcpy(fail, "PocketWiki could not verify the pack checksum.", sizeof fail);
            goto install_done;
        }
        received += got;
        if (total > 0 && received > total) {
            strlcpy(fail, "The downloaded pack was larger than expected.", sizeof fail);
            goto install_done;
        }
        if ((uint64_t)received > (uint64_t)CONFIG_POCKETWIKI_PACK_MAX_UPLOAD_KB * 1024 ||
                (uint64_t)received > installable) {
            strlcpy(fail, "That pack is too large for this device.", sizeof fail);
            goto install_done;
        }
        int64_t now = esp_timer_get_time();
        if (now - last_progress_us >= 250000 || (total > 0 && received == total)) {
            install_progress_send(req, &progress, received, total);
            oled_show_transfer(name, (size_t)received, (size_t)(total > 0 ? total : 0),
                               flash_used + (size_t)received, flash_total);
            last_progress_us = now;
        }
    }
    if (total > 0 && received != total) {
        strlcpy(fail, "The downloaded pack size did not match its expected length.", sizeof fail);
        goto install_done;
    }
    if (expected_bytes > 0 && received != expected_bytes) {
        strlcpy(fail, "The downloaded pack size did not match the catalogue.", sizeof fail);
        goto install_done;
    }
    /* Release the TLS client before checksum verification. The C3 cannot
     * reliably allocate a full TLS record while the PSA hash context and
     * filesystem stream are also live. Reopen the completed temp file after
     * the network connection is gone, so integrity checking remains intact. */
    if (client != NULL) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        client = NULL;
    }
    hashing = verify_hash;
    if (hashing) {
        unsigned char digest[32];
        size_t olen = 0;
        if (fflush(fh) != 0 || fclose(fh) != 0) {
            fh = NULL;
            strlcpy(fail, "PocketWiki could not read the downloaded pack.", sizeof fail);
            goto install_done;
        }
        fh = NULL;
        FILE *rf = fopen(temp, "rb");
        if (rf == NULL || psa_crypto_init() != PSA_SUCCESS ||
            psa_hash_setup(&hash, PSA_ALG_SHA_256) != PSA_SUCCESS) {
            if (rf != NULL) fclose(rf);
            strlcpy(fail, "PocketWiki could not verify the pack checksum.", sizeof fail);
            goto install_done;
        }
        while (!feof(rf)) {
            size_t got = fread(buffer, 1, 1024, rf);
            if (got > 0 && psa_hash_update(&hash, buffer, got) != PSA_SUCCESS) {
                fclose(rf);
                strlcpy(fail, "PocketWiki could not verify the pack checksum.", sizeof fail);
                goto install_done;
            }
            if (ferror(rf)) {
                fclose(rf);
                strlcpy(fail, "PocketWiki could not read the downloaded pack.", sizeof fail);
                goto install_done;
            }
        }
        fclose(rf);
        if (psa_hash_finish(&hash, digest, sizeof digest, &olen) != PSA_SUCCESS ||
            olen != sizeof digest) {
            strlcpy(fail, "PocketWiki could not verify the pack checksum.", sizeof fail);
            goto install_done;
        }
        char actual[65];
        for (size_t i = 0; i < olen; i++) snprintf(actual + i * 2, 3, "%02x", digest[i]);
        if (strcasecmp(actual, sha_hex) != 0) {
            strlcpy(fail, "The pack checksum did not match the catalogue.", sizeof fail);
            goto install_done;
        }
        fh = fopen(temp, "ab");
        if (fh == NULL) {
            strlcpy(fail, "PocketWiki could not reopen the downloaded pack.", sizeof fail);
            goto install_done;
        }
    }

install_done:
    free(buffer);
    if (client != NULL) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }
    if (hashing) psa_hash_abort(&hash);
    if (ble_suspended) {
        esp_err_t berr = ble_provisioning_resume();
        if (berr != ESP_OK) ESP_LOGE(TAG, "install: BLE resume failed: %s", esp_err_to_name(berr));
    }
    oled_clear_transfer();
    /* Finish any partial progress frame before httpd writes the result. Only
     * this final drain may wait: the upstream TLS connection is closed now. */
    install_progress_flush(req, &progress, 0);
    if (fail[0] != 0) {
        pack_store_abort_upload(fh, temp);
        char line[192];
        int len = snprintf(line, sizeof line, "ERR:%s\n", fail);
        esp_err_t cres = progress.failed ? ESP_FAIL : send_body(req, line, (size_t)len);
        if (cres != ESP_OK) {
            ESP_LOGE(TAG, "install: ERR chunk send failed: %s (browser stuck on stale progress)", esp_err_to_name(cres));
        }
    } else {
        uint32_t articles = 0;
        esp_err_t err = pack_store_finish_upload(fh, temp, name, &articles);
        if (err != ESP_OK) {
            char line[160];
            snprintf(line, sizeof line, "ERR:%s\n", ca_last_reject_reason());
            esp_err_t cres = progress.failed ? ESP_FAIL : send_body(req, line, strlen(line));
            if (cres != ESP_OK) {
                ESP_LOGE(TAG, "install: ERR chunk send failed: %s", esp_err_to_name(cres));
            }
        } else {
            char line[64];
            int len = snprintf(line, sizeof line, "OK:%lu\n", (unsigned long)articles);
            esp_err_t cres = progress.failed ? ESP_FAIL : send_body(req, line, (size_t)len);
            if (cres != ESP_OK) {
                ESP_LOGE(TAG, "install: OK chunk send failed: %s", esp_err_to_name(cres));
            }
        }
    }
    esp_err_t result = progress.failed ? ESP_FAIL : finish_response(req);
    wdt_relieve(false);
    return result;
}

/* ---- /browse: paginated full-library listing with infinite scroll ---- */

static esp_err_t handle_browse(httpd_req_t *req)
{
    char raw[192];
    char pack[PW_PACK_NAME_MAX + 1];
    strlcpy(pack, PW_BUILTIN_PACK_NAME, sizeof pack);
    uint32_t offset = 0;
    bool partial = false;

    size_t qlen = httpd_req_get_url_query_len(req);
    if (qlen > 0 && qlen < sizeof raw &&
            httpd_req_get_url_query_str(req, raw, sizeof raw) == ESP_OK) {
        char val[PW_PACK_NAME_MAX + 1];
        if (httpd_query_key_value(raw, "pack", val, sizeof val) == ESP_OK) {
            strlcpy(pack, val, sizeof pack);
        }
        char num[12];
        if (httpd_query_key_value(raw, "offset", num, sizeof num) == ESP_OK) {
            uint32_t parsed = 0;
            if (parse_u32_strict(num, strlen(num), &parsed)) offset = parsed;
        }
        char part[4];
        if (httpd_query_key_value(raw, "partial", part, sizeof part) == ESP_OK &&
                strcmp(part, "1") == 0) {
            partial = true;
        }
    }

    if (pack_store_open_for_read(pack) != ESP_OK) {
        if (!partial) {
            return send_error(req, 404, "404 Not Found", "Library not found",
                              "That library is not installed.");
        }
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        return send_plain(req, "", 0);
    }

    const content_archive_t *ca = ca_get();
    if (!ca->valid) {
        if (!partial) {
            return send_error(req, 500, "500 Internal Server Error",
                              "Content archive not loaded", "");
        }
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        return send_plain(req, "", 0);
    }

    uint32_t total = ca->count;
    uint32_t end = offset + PW_BROWSE_PAGE_SIZE;
    if (end > total) end = total;

    tl_index_t ix = { .entries_off = ca->entries_off, .strings_off = ca->strings_off,
                      .count = ca->count, .entry_size = TL_ENTRY_SIZE,
                      .read = ca_index_read, .ctx = (void *)ca };

    if (!partial) {
        page_open(req, " &middot; browse");
        char hdr[320];
        snprintf(hdr, sizeof hdr,
                 "<h1>Browse library</h1>"
                 "<p class=\"muted\">%lu article%s total &middot; "
                 "<a href=\"/\">&#x2190; Back to libraries</a></p>"
                 "<div class=\"list\" id=\"browse-list\">",
                 (unsigned long)total, total == 1 ? "" : "s");
        send_body(req, hdr, strlen(hdr));
    } else {
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    }

    uint32_t shown_items = 0;
    for (uint32_t id = offset; id < end; id++) {
        uint32_t tlen = 0;
        if (tl_read_display_title(&ix, id, s_render_title, sizeof s_render_title, &tlen) != 0) continue;
        if (html_escape((const char *)s_render_title, tlen, s_render_title_esc,
                        sizeof s_render_title_esc) == 0) continue;
        if (send_library_item(req, id, pack, s_render_title_esc) == ESP_OK) shown_items++;
    }

    if (!partial) {
        SEND_LITERAL(req, "</div>");

        /* Sentinel + inline JS for IntersectionObserver infinite scroll.
         * Each intersection fires one fetch of the next PW_BROWSE_PAGE_SIZE
         * titles as raw HTML fragment (partial=1), appended to #browse-list. */
        if (end < total) {
            /* Build the quoted JS pack name — pack names are [a-z0-9_-] so
             * single-quoting is always safe without further escaping. */
            char js_pack[PW_PACK_NAME_MAX + 4];
            snprintf(js_pack, sizeof js_pack, "'%s'", pack);
            char sentinel[700];
            snprintf(sentinel, sizeof sentinel,
                "<div id=\"browse-sentinel\" style=\"height:2px\"></div>"
                "<script>"
                "(function(){"
                "var off=%lu,pack=%s,total=%lu,bsz=%d;"
                "var list=document.getElementById('browse-list');"
                "var obs=new IntersectionObserver(function(es){"
                "if(!es[0].isIntersecting)return;"
                "obs.unobserve(es[0].target);"
                "fetch('/browse?pack='+encodeURIComponent(pack)+'&offset='+off+'&partial=1')"
                ".then(function(r){return r.text()})"
                ".then(function(h){"
                "list.insertAdjacentHTML('beforeend',h);"
                "off+=bsz;"
                "var s=document.getElementById('browse-sentinel');"
                "if(s&&off<total)obs.observe(s);"
                "});"
                "},{rootMargin:'600px'});"
                "var s=document.getElementById('browse-sentinel');"
                "if(s)obs.observe(s);"
                "})();"
                "</script>",
                (unsigned long)end, js_pack, (unsigned long)total, PW_BROWSE_PAGE_SIZE);
            send_body(req, sentinel, strlen(sentinel));
        }

        return page_close(req);
    }
    if (shown_items == 0) {
        /* An empty scroll page: finish_response() would write a chunk
         * terminator before any response headers. Let esp_http_server send the
         * complete empty response instead. */
        return send_plain(req, NULL, 0);
    }
    return finish_response(req);
}

static esp_err_t handle_packs_page(httpd_req_t *req)
{
    page_open(req, " &middot; manage");

    size_t pack_total = 0, pack_used = 0;
    pack_store_usage(&pack_total, &pack_used);
    size_t installable = pack_store_installable_bytes();
    unsigned storage_pct = pack_total ? (unsigned)((pack_used * 100u) / pack_total) : 0;
    char buf[448];
    snprintf(buf, sizeof buf,
             "<h1>Manage library</h1><p class=\"muted\">The included library is ready. Add more packs when you want them.</p>"
             "<p id=\"storage-status\" class=\"storage-status\" data-installable=\"%u\">"
             "<strong>%u KB available</strong> for new packs &middot; %u KB used of %u KB</p>"
             "<div class=\"storage-meter\" role=\"progressbar\" aria-label=\"Flash storage used\" "
             "aria-valuemin=\"0\" aria-valuemax=\"100\" aria-valuenow=\"%u\"><span style=\"width:%u%%\"></span></div>",
             (unsigned)installable, (unsigned)(installable / 1024),
             (unsigned)(pack_used / 1024), (unsigned)(pack_total / 1024),
             storage_pct, storage_pct);
    send_body(req, buf, strlen(buf));

    httpd_resp_send_chunk(req,
        "<div class=\"wifi-status-card\" id=\"wifi-status-card\" aria-live=\"polite\">"
        "<p class=\"network-state\">Checking the connection…</p></div>",
        HTTPD_RESP_USE_STRLEN);

    SEND_LITERAL(req, "<div class=\"list pack-list\">");
    pack_store_item_t *items = NULL;
    int n = pack_store_list_all(&items);
    /* The built-in library is never removable. When a pack of the same name is
     * installed it is served in its place, and that installed copy can be
     * removed to go back to the archive in flash. */
    bool built_in_updated = false;
    uint32_t built_in_articles = 0;
    if (pack_store_open_for_read(PW_BUILTIN_PACK_NAME) == ESP_OK) {
        built_in_updated = ca_get()->from_file;
        built_in_articles = ca_get()->count;
    }
    pack_display_name(PW_BUILTIN_PACK_NAME, (char *)s_render_title, sizeof s_render_title);
    if (html_escape((const char *)s_render_title, strlen((const char *)s_render_title),
                    s_render_title_esc, sizeof s_render_title_esc) == 0) {
        strlcpy(s_render_title_esc, "PocketWiki Guide", sizeof s_render_title_esc);
    }
    snprintf(buf, sizeof buf, "<p data-builtin=\"1\"%s><strong>",
             built_in_updated ? " data-pack-name=\"" PW_BUILTIN_PACK_NAME "\"" : "");
    send_body(req, buf, strlen(buf));
    send_body(req, s_render_title_esc, strlen(s_render_title_esc));
    snprintf(buf, sizeof buf, "</strong> &middot; %lu articles &middot; built in%s",
             (unsigned long)built_in_articles,
             built_in_updated ? ", updated" : " and always available");
    send_body(req, buf, strlen(buf));
    if (pack_store_guide_hidden()) {
        SEND_LITERAL(req, " &middot; hidden from the library list");
    }
    /* A plain form: the manage page's script only wires Remove buttons, and the
     * toggle must not need one. */
    snprintf(buf, sizeof buf,
             " <form class=\"inline-form\" method=\"post\" action=\"/packs/guide-toggle\">"
             "<button class=\"inline-action\" type=\"submit\">%s</button></form>",
             pack_store_guide_hidden() ? "Show in libraries" : "Hide from libraries");
    send_body(req, buf, strlen(buf));
    if (built_in_updated) {
        SEND_LITERAL(req, " <button class=\"inline-action danger\" data-action=\"delete\" "
                          "data-name=\"" PW_BUILTIN_PACK_NAME "\">Remove update</button>");
    }
    SEND_LITERAL(req, "</p>");
    if (n > 0) {
        for (int i = 0; i < n; i++) {
            pack_display_name(items[i].name, (char *)s_render_title, sizeof s_render_title);
            if (html_escape((const char *)s_render_title,
                            strlen((const char *)s_render_title), s_render_title_esc,
                            sizeof s_render_title_esc) == 0) continue;
            snprintf(buf, sizeof buf, "<p data-pack-name=\"%.40s\"><strong>", items[i].name);
            send_body(req, buf, strlen(buf));
            send_body(req, s_render_title_esc, strlen(s_render_title_esc));
            snprintf(buf, sizeof buf,
                     "</strong> &middot; %lu articles &middot; installed "
                     "<button class=\"inline-action danger\" data-action=\"delete\" data-name=\"%.40s\">Remove</button></p>",
                     (unsigned long)items[i].articles, items[i].name);
            send_body(req, buf, strlen(buf));
        }
    }
    free(items);
    SEND_LITERAL(req, "</div>");

    SEND_BODY_LITERAL(req,
        "<section class=\"manager-section\"><div class=\"catalog-head\"><div>"
"<h2>Add a library pack</h2>"
"<p class=\"muted\">Choose a pack from the catalogue, or install a <code>.pwp</code> file you already have. Catalogue downloads need internet access.</p></div>"
"<div class=\"catalog-sync\"><button class=\"scan-action\" id=\"catalog-sync\" type=\"button\">Refresh catalogue</button>"
"<span id=\"catalog-synced\" class=\"synced-at\" hidden></span></div></div>"
"<div id=\"pack-catalog\" class=\"catalog-list\"><p class=\"network-state\">Loading the catalogue…</p></div>"
"<div id=\"catalog-actions\" class=\"catalog-actions\" hidden>"
"<div class=\"catalog-actions-copy\"><strong id=\"catalog-count\"></strong>"
"<span id=\"catalog-room\" class=\"muted\"></span></div>"
"<div class=\"catalog-actions-buttons\">"
"<button class=\"inline-action\" id=\"catalog-clear\" type=\"button\">Clear</button>"
"<button class=\"catalog-action\" id=\"catalog-install\" type=\"button\" disabled>Install</button>"
"</div></div>"
"<p id=\"catalog-status\" class=\"muted\" aria-live=\"polite\"></p>"
"<progress id=\"catalog-progress\" class=\"install-progress\" max=\"100\" value=\"0\" hidden></progress>"
"<h3 class=\"install-other\">Install from a file or URL</h3>"
"<form class=\"upload\" id=\"pack-upload\"><label>Pack file on this device"
"<input id=\"pack-file\" type=\"file\" accept=\".pwp\" required></label>"
"<button type=\"submit\">Install pack file</button></form>"
"<form class=\"upload\" id=\"url-upload\"><label>Secure pack URL"
"<input id=\"pack-url\" type=\"url\" inputmode=\"url\" placeholder=\"https://example.com/library.pwp\" required></label>"
"<button type=\"submit\">Download and install</button></form>"
"<progress id=\"upload-progress\" class=\"install-progress\" max=\"100\" value=\"0\" hidden></progress>"
"<p id=\"upload-status\" class=\"muted\" aria-live=\"polite\"></p></section>"
"<section class=\"manager-section\" id=\"wifi-manager\"><h2>Connect PocketWiki to Wi-Fi</h2>"
"<p class=\"muted\">Optional: connect to a 2.4 GHz network to refresh the catalogue and download packs. The PocketWiki access point stays available for reading.</p>"
"<button class=\"scan-action\" id=\"wifi-toggle\" type=\"button\">Set up Wi-Fi</button>"
"<div id=\"wifi-panel\" hidden>"
"<div class=\"wifi-scan-head\"><strong>Nearby 2.4 GHz networks</strong><button class=\"scan-action\" id=\"wifi-scan\" type=\"button\">Scan nearby networks</button></div>"
"<div id=\"wifi-networks\" class=\"wifi-networks\" role=\"listbox\" aria-label=\"Nearby Wi-Fi networks\">"
"<p class=\"network-state\">No scan yet. Scan nearby networks or enter the name below.</p></div>"
"<form class=\"upload\" id=\"wifi-form\"><label>Wi-Fi network name<input id=\"wifi-ssid\" autocomplete=\"username\" required></label>"
"<label>Wi-Fi password<input id=\"wifi-password\" type=\"password\" autocomplete=\"current-password\"></label>"
"<button type=\"submit\">Save and connect</button></form>"
"<p id=\"wifi-status\" class=\"muted\" aria-live=\"polite\"></p></div></section>");
    SEND_LITERAL(req, "<script>");
    send_body(req, (const char *)pocketwiki_manager_js,
              pocketwiki_manager_js_len);
    SEND_LITERAL(req, "</script>");
    snprintf(buf, sizeof buf, "<p><a href=\"/\">&larr; Back to your libraries</a></p>");
    send_body(req, buf, strlen(buf));

    return page_close(req);
}

static esp_err_t handle_legacy_packs_page(httpd_req_t *req)
{
    httpd_resp_set_status(req, "301 Moved Permanently");
    httpd_resp_set_hdr(req, "Location", "/manage");
    return send_plain(req, "Moved permanently to /manage\n", sizeof("Moved permanently to /manage\n") - 1);
}

/* ---- init ---- */

esp_err_t web_server_init(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    /* One transfer budget for both targets. Handlers are serialized, so the
     * send timeout is how long one client that stopped reading can hold the
     * whole server: keep it short enough that a dead session is retired
     * quickly, and long enough for a healthy client to drain a 1 KB chunk.
     * (A measured C3 wedge with the pool stuck at 10 s produced one failed
     * send every 10 s, forever.) */
    cfg.stack_size = PW_HTTPD_STACK_BYTES;    /* inflate + 1 KB chunk + headroom */
    cfg.send_wait_timeout = 5;
    cfg.recv_wait_timeout = 3;                /* faster socket recycling (was 5) */
#if CONFIG_IDF_TARGET_ESP32S3
    cfg.core_id = 1;                          /* dual core: keep httpd off Core 0 (WiFi/LWIP) */
#endif
    cfg.max_open_sockets = CONFIG_POCKETWIKI_HTTPD_MAX_SOCKETS;
    cfg.max_uri_handlers = 28;
    cfg.lru_purge_enable = true;
    cfg.uri_match_fn = httpd_uri_match_wildcard;

    httpd_handle_t server = NULL;
    ESP_RETURN_ON_ERROR(httpd_start(&server, &cfg), TAG, "httpd start");

    static const httpd_uri_t uris[] = {
        { .uri = "/",           .method = HTTP_GET,  .handler = handle_root },
        { .uri = "/search",     .method = HTTP_GET,  .handler = handle_search },
        { .uri = "/download/*", .method = HTTP_GET,  .handler = handle_download_id },
        { .uri = "/a/*",        .method = HTTP_GET,  .handler = handle_article_id,
          .user_ctx = NULL },
        { .uri = "/title/*",    .method = HTTP_GET,  .handler = handle_article_title },
        { .uri = "/browse",     .method = HTTP_GET,  .handler = handle_browse },
        { .uri = "/style.css",  .method = HTTP_GET,  .handler = handle_style },
        { .uri = "/manage",     .method = HTTP_GET,  .handler = handle_packs_page },
        { .uri = "/packs",      .method = HTTP_GET,  .handler = handle_legacy_packs_page },
        { .uri = "/api/packs",  .method = HTTP_GET,  .handler = handle_packs },
        { .uri = "/api/packs/catalog", .method = HTTP_GET, .handler = handle_packs_catalog },
        { .uri = "/api/packs/catalog/sync", .method = HTTP_POST, .handler = handle_packs_catalog_sync },
        { .uri = "/api/packs/install", .method = HTTP_POST, .handler = handle_pack_install },
        { .uri = "/api/packs/action", .method = HTTP_POST, .handler = handle_pack_action },
        { .uri = "/packs/guide-toggle", .method = HTTP_POST, .handler = handle_guide_toggle },
        { .uri = "/api/packs/upload", .method = HTTP_POST, .handler = handle_pack_upload },
        { .uri = "/api/wifi/scan", .method = HTTP_GET, .handler = handle_wifi_scan },
        { .uri = "/api/wifi/status", .method = HTTP_GET, .handler = handle_wifi_status },
        { .uri = "/api/wifi/config", .method = HTTP_POST, .handler = handle_wifi_config },
        { .uri = "/health",     .method = HTTP_GET,  .handler = handle_health },
        { .uri = "/api/stats",  .method = HTTP_GET,  .handler = handle_stats },
    };
    for (size_t i = 0; i < sizeof uris / sizeof uris[0]; i++) {
        esp_err_t err = httpd_register_uri_handler(server, &uris[i]);
        if (err != ESP_OK) {
            httpd_stop(server);
            return err;
        }
    }
    /* catch-all */
    httpd_uri_t all = { .uri = "/*", .method = HTTP_GET, .handler = handle_404 };
    esp_err_t err = httpd_register_uri_handler(server, &all);
    if (err != ESP_OK) {
        httpd_stop(server);
        return err;
    }

    ESP_LOGI(TAG, "HTTP server ready (%d sockets, heap=%lu)", cfg.max_open_sockets,
             (unsigned long)esp_get_free_heap_size());
    return ESP_OK;
}

