/* PocketWiki HTTP server.
 *
 * Design notes:
 *  - Articles are stored as independent compressed frames. Current v2 zstd
 *    frames use the bounded streaming decoder; older v1 gzip frames remain
 *    readable through the compatibility path.
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
#include "pwpk_service.h"
#include "pwpk_http.h"

#define ZSTD_STATIC_LINKING_ONLY
#include "zstd.h"

static const char *TAG = "httpd";

/* One decoder is shared by the serialized HTTP article handlers.  Declared
 * here so the diagnostic endpoint can report its allocation footprint. */
static ZSTD_DStream *s_dstream = NULL;

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
/* Legacy gzip is decoded into a wrapping 32 KiB window. This keeps the
 * compatibility path bounded without requiring a comp_len + uncomp_len heap
 * allocation. */
#define PW_GZIP_WINDOW_SIZE 32768
/* Uncompressed cap for the legacy v1 (gzip) decode path. This is a protocol
 * sanity limit, not a decoder allocation size. */
#define PW_LEGACY_GZIP_MAX_UNCOMP (128u * 1024u)

#define SEND_LITERAL(req, literal) \
    httpd_resp_send_chunk((req), (literal), sizeof(literal) - 1)

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

static esp_err_t send_decode_error(httpd_req_t *req, const char *message)
{
    char body[192];
    int n = snprintf(body, sizeof body, "<html><body><p>%s</p></body></html>", message);
    if (n < 0) return ESP_FAIL;
    esp_err_t err = httpd_resp_send_chunk(req, body,
                                          (size_t)n < sizeof body ? (size_t)n : sizeof body - 1);
    if (err != ESP_OK) return err;
    return httpd_resp_send_chunk(req, NULL, 0);
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
static bool catalog_file_json(char **out, size_t *len);
static bool catalog_pack_name(const char *id, char *out, size_t cap);
static void pack_display_name(const char *id, char *out, size_t cap);
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
    if (len < 0 || (size_t)len >= sizeof header) return ESP_FAIL;
    return httpd_resp_send_chunk(req, header, (size_t)len);
}

static esp_err_t page_close(httpd_req_t *req)
{
    char foot[160];
    int libraries = pack_store_count() + 1;
    snprintf(foot, sizeof foot,
             "</main><footer>PocketWiki offline &middot; %d librar%s ready &middot; %lu articles</footer></body></html>",
             libraries, libraries == 1 ? "y" : "ies",
             (unsigned long)pack_store_total_articles());
    httpd_resp_send_chunk(req, foot, strlen(foot));
    esp_err_t result = httpd_resp_send_chunk(req, NULL, 0);
    /* The catalogue is needed while rendering titles, but retaining its cJSON
     * tree after the response costs most of the C3's remaining heap. Keep the
     * cache request-scoped; the next request can rebuild it if needed. */
    catalog_cache_invalidate();
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
    return httpd_resp_send_chunk(req, s_render_item, (size_t)len);
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
    httpd_resp_send_chunk(req, s_render_label_esc, strlen(s_render_label_esc));
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
    return httpd_resp_send_chunk(req, s_render_item, (size_t)len);
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
    httpd_resp_send_chunk(req, head, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, s_render_label_esc, strlen(s_render_label_esc));
    snprintf(head, sizeof head,
             "</strong><small>%lu article%s</small></span><span class=\"active-label\">Ready</span>"
             "</summary><div class=\"library-articles\">",
             (unsigned long)ca->count, ca->count == 1 ? "" : "s");
    httpd_resp_send_chunk(req, head, HTTPD_RESP_USE_STRLEN);

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
        httpd_resp_send_chunk(req, head, HTTPD_RESP_USE_STRLEN);
    }
    return httpd_resp_send_chunk(req, "</div></details>", HTTPD_RESP_USE_STRLEN);
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
    httpd_resp_send_chunk(req, s_render_item, (size_t)len);
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
    httpd_resp_send_chunk(req, hdr, HTTPD_RESP_USE_STRLEN);

    if (pack_store_open_for_read(PW_BUILTIN_PACK_NAME) == ESP_OK) {
        char label[PW_TITLE_CAP];
        pack_display_name(PW_BUILTIN_PACK_NAME, label, sizeof label);
        send_library_group(req, PW_BUILTIN_PACK_NAME, label);
    }
    for (int i = 0; i < count; i++) {
        if (pack_store_open_for_read(items[i].name) == ESP_OK) {
            char label[PW_TITLE_CAP];
            pack_display_name(items[i].name, label, sizeof label);
            send_library_group(req, items[i].name, label);
        }
    }
    free(items);
    httpd_resp_send_chunk(req, "</div>", HTTPD_RESP_USE_STRLEN);
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
    httpd_resp_send_chunk(req, "<h1>Search</h1>", HTTPD_RESP_USE_STRLEN);

    html_escape(q, (size_t)qlen, s_render_query_esc, sizeof s_render_query_esc);
    SEND_LITERAL(req, "<p class=\"muted\">Results for &ldquo;");
    httpd_resp_send_chunk(req, s_render_query_esc, strlen(s_render_query_esc));
    SEND_LITERAL(req, "&rdquo;</p>");

    if (norm_len == 0) {
        httpd_resp_send_chunk(req, "<p>Empty search term. Try a word from a title.</p>",
                              HTTPD_RESP_USE_STRLEN);
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
    for (int i = 0; i < pack_count && shown < PW_SEARCH_LIMIT && !failed; i++) {
        if (pack_store_open_for_read(items[i].name) != ESP_OK) continue;
        char label[PW_TITLE_CAP];
        pack_display_name(items[i].name, label, sizeof label);
        int n = send_library_search_results(req, items[i].name, label, norm, norm_len,
                                            PW_SEARCH_LIMIT - shown);
        if (n < 0) failed = true; else shown += n;
    }
    free(items);
    if (pack_store_restore_builtin() != ESP_OK) {
        ESP_LOGE(TAG, "could not restore built-in library after search");
    }
    if (failed) {
        httpd_resp_send_chunk(req, "<p>One library could not be searched.</p>",
                              HTTPD_RESP_USE_STRLEN);
    } else if (shown == 0) {
        httpd_resp_send_chunk(req, "<p>No articles match.</p>", HTTPD_RESP_USE_STRLEN);
    }
    if (shown >= PW_SEARCH_LIMIT) {
        httpd_resp_send_chunk(req, "<p class=\"muted\">First 20 matches shown across all libraries.</p>",
                              HTTPD_RESP_USE_STRLEN);
    } else if (shown > 0) {
        char summary[96];
        snprintf(summary, sizeof summary,
                 "<p class=\"muted\">%d result%s across all libraries.</p>",
                 shown, shown == 1 ? "" : "s");
        httpd_resp_send_chunk(req, summary, HTTPD_RESP_USE_STRLEN);
    }
    return page_close(req);
}

/* ---- article streaming ---- */

static esp_err_t stream_article_zstd(httpd_req_t *req, uint32_t id,
                                     const pw_article_meta_t *meta,
                                     const char *etag);
static esp_err_t stream_article_gzip(httpd_req_t *req, uint32_t id,
                                     const pw_article_meta_t *meta,
                                     const char *etag);

/* ESP-IDF's chunk helper can enter its partial-write retry loop on the final
 * zero-length chunk on ESP32-C3/lwIP. The terminator is only five bytes, so a
 * single bounded socket write is sufficient and cannot consume the handler
 * indefinitely. */
static esp_err_t finish_article_stream(httpd_req_t *req)
{
    static const char terminator[] = "0\r\n\r\n";
    int fd = httpd_req_to_sockfd(req);
    int sent = fd >= 0 ? httpd_socket_send(req->handle, fd, terminator,
                                           sizeof(terminator) - 1, 0) : -1;
    return sent == (int)(sizeof(terminator) - 1) ? ESP_OK : ESP_FAIL;
}

static esp_err_t finish_decode_error(httpd_req_t *req, bool any_sent,
                                     const char *message)
{
    if (!any_sent) {
        char body[192];
        int len = snprintf(body, sizeof body,
                           "<html><body><p>%s</p></body></html>", message);
        if (len < 0 || (size_t)len >= sizeof body) return ESP_FAIL;
        httpd_resp_send_chunk(req, body, (size_t)len);
    } else {
        SEND_LITERAL(req, "</body></html>");
    }
    return finish_article_stream(req);
}

/* tinfl_decompressor is too large for the C3 HTTP task stack, and the C3
 * heap is already fragmented by Wi-Fi/BLE. Keep one decoder in static storage
 * because HTTP handlers are serialized by esp_http_server. */
static tinfl_decompressor s_gzip_decomp;
static uint8_t s_gzip_window[PW_GZIP_WINDOW_SIZE];
static uint8_t s_gzip_in[PW_CHUNK_SIZE];

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
            return httpd_resp_send(req, NULL, 0);
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
     * bounded chunks — clients never see the storage codec. v2 packs are zstd frames
     * sharing the pack's trained dictionary; v1 (legacy) packs are gzip
     * streams, still readable so packs installed before the zstd cutover keep
     * serving after a firmware upgrade. */
    if (ca_get()->format_version == PW_FORMAT_VERSION_LEGACY) {
        return stream_article_gzip(req, id, &meta, etag);
    }
    return stream_article_zstd(req, id, &meta, etag);
}

/* Stream a v2 article: an independent zstd frame compressed against the pack's
 * trained dictionary (compression-at-rest). httpd handlers run serially in one
 * task, so a single persistent DCtx (s_dstream) and a pre-built DDict (s_ddict)
 * avoid any heap allocation per request. ZSTD_DCtx_refDDict stores only a
 * pointer — zero bytes allocated on the fragmented runtime heap. */
static ZSTD_DDict  *s_ddict   = NULL;
static uint32_t s_ddict_generation = UINT32_MAX;
static uint8_t s_stream_in[PW_CHUNK_SIZE];
static uint8_t s_stream_out[PW_CHUNK_SIZE];

void web_server_archive_about_to_change(void)
{
    if (s_dstream != NULL) {
        ZSTD_freeDStream(s_dstream);
        s_dstream = NULL;
    }
    if (s_ddict != NULL) {
        ZSTD_freeDDict(s_ddict);
        s_ddict = NULL;
    }
    s_ddict_generation = UINT32_MAX;
}

/* (Re)build s_ddict from whatever dictionary ca_get_dict() currently returns.
 * The archive generation check makes this cheap on repeated reads and ensures
 * every library switch rebinds the matching dictionary. */
static esp_err_t rebuild_ddict(void)
{
    uint32_t generation = ca_generation();
    if (s_ddict_generation == generation) return ESP_OK;

    uint32_t dict_len = 0;
    const uint8_t *dict = ca_get_dict(&dict_len);
    /* A streaming context retains its last frame window. Release it while
     * switching libraries so that memory is available for the new DDict and
     * so no state can remain associated with the previous dictionary. */
    web_server_archive_about_to_change();
    if (dict_len > 0) {
        /* content_archive owns the blob for the archive lifetime, so the
         * by-reference form avoids a redundant 32 KiB dictionary copy. */
        s_ddict = ZSTD_createDDict_byReference(dict, dict_len);
        if (s_ddict == NULL) {
            ESP_LOGW(TAG, "zstd DDict pre-alloc failed (%lu B dict)", (unsigned long)dict_len);
            return ESP_ERR_NO_MEM;
        } else {
            ESP_LOGI(TAG, "zstd DDict pre-allocated (%lu B dict)", (unsigned long)dict_len);
        }
    }
    s_ddict_generation = generation;
    return ESP_OK;
}

static esp_err_t stream_article_zstd(httpd_req_t *req, uint32_t id,
                                     const pw_article_meta_t *meta,
                                     const char *etag)
{
    if (rebuild_ddict() != ESP_OK) {
        ESP_LOGE(TAG, "article %lu: dictionary unavailable", (unsigned long)id);
        return send_decode_error(req, "Out of memory \xe2\x80\x94 try again.");
    }
    if (s_dstream == NULL) {
        s_dstream = ZSTD_createDStream();
    }
    if (s_dstream == NULL) {
        ESP_LOGE(TAG, "article %lu: decoder stream unavailable", (unsigned long)id);
        return send_decode_error(req, "Out of memory \xe2\x80\x94 try again.");
    }

    uint8_t *in = s_stream_in;
    uint8_t *out = s_stream_out;
    ZSTD_DStream *ds = s_dstream;

    /* Reset the session (clears history/pending state) then bind the
     * pre-built DDict via a zero-copy pointer reference — no heap
     * allocation takes place here. */
    size_t rc = ZSTD_DCtx_reset(ds, ZSTD_reset_session_and_parameters);
    if (!ZSTD_isError(rc)) {
        rc = ZSTD_DCtx_setParameter(ds, ZSTD_d_maxBlockSize, 1024);
    }
    if (!ZSTD_isError(rc) && s_ddict != NULL) {
        rc = ZSTD_DCtx_refDDict(ds, s_ddict);
    }
    if (ZSTD_isError(rc)) {
        ESP_LOGE(TAG, "article %lu: zstd init failed: %s", (unsigned long)id,
                 ZSTD_getErrorName(rc));
        return send_decode_error(req, "Decoder init error \xe2\x80\x94 try again.");
    }

    ZSTD_inBuffer ib = { in, 0, 0 };
    uint32_t sent_in = 0;
    uint32_t total_out = 0;
    uint32_t output_crc = 0;
    bool any_sent = false;              /* tracks whether any chunk was flushed */
    size_t bytes_since_yield = 0;
    bool frame_done = false;
    esp_err_t result = ESP_OK;

    for (;;) {
        if (ib.pos == ib.size) {
            if (sent_in >= meta->comp_len) {
                break;                  /* input exhausted before frame end */
            }
            size_t n = meta->comp_len - sent_in;
            if (n > PW_CHUNK_SIZE) n = PW_CHUNK_SIZE;
            if (ca_read_payload(meta->content_offset + sent_in, in, n) != ESP_OK) {
                ESP_LOGE(TAG, "article %lu: flash read failed mid-stream", (unsigned long)id);
                result = ESP_FAIL;
                break;
            }
            ib.src = in;
            ib.size = n;
            ib.pos = 0;
            sent_in += (uint32_t)n;
        }
        ZSTD_outBuffer ob = { out, PW_CHUNK_SIZE, 0 };
        rc = ZSTD_decompressStream(ds, &ob, &ib);
        if (ZSTD_isError(rc)) {
            ESP_LOGE(TAG, "article %lu: zstd decode error: %s", (unsigned long)id,
                     ZSTD_getErrorName(rc));
            result = ESP_FAIL;
            break;
        }
        if (ob.pos > 0) {
            total_out += (uint32_t)ob.pos;
            if (total_out > meta->uncomp_len) {
                ESP_LOGE(TAG, "article %lu: decode exceeds declared uncomp_len %lu",
                         (unsigned long)id, (unsigned long)meta->uncomp_len);
                result = ESP_FAIL;
                break;
            }
            output_crc = esp_rom_crc32_le(output_crc, out, ob.pos);
            esp_err_t send_err = httpd_resp_send_chunk(req, (const char *)out, ob.pos);
            if (send_err != ESP_OK) {
                ESP_LOGW(TAG, "article %lu: client send aborted (%s)",
                         (unsigned long)id, esp_err_to_name(send_err));
                result = ESP_FAIL;
                break;
            }
            any_sent = true;
            bytes_since_yield += ob.pos;
            if (bytes_since_yield >= PW_ARTICLE_YIELD_BYTES) {
                bytes_since_yield = 0;
                taskYIELD();
            }
            if (esp_task_wdt_status(NULL) == ESP_OK) {
                esp_task_wdt_reset();
            }
        }
        if (rc == 0) {
            frame_done = true;
            break;                      /* frame fully decoded */
        }
    }
    if (result == ESP_OK && (!frame_done || sent_in != meta->comp_len ||
                             ib.pos != ib.size)) {
        ESP_LOGE(TAG, "article %lu: zstd frame has truncated or trailing input",
                 (unsigned long)id);
        result = ESP_FAIL;
    }

    if (result != ESP_OK) {
        return finish_decode_error(req, any_sent,
                                   "Article decode error \xe2\x80\x94 try again.");
    }
    if (total_out != meta->uncomp_len) {
        ESP_LOGE(TAG, "article %lu: truncated decode (%lu != %lu)", (unsigned long)id,
                 (unsigned long)total_out, (unsigned long)meta->uncomp_len);
        return finish_decode_error(req, any_sent,
                                   "Article truncated \xe2\x80\x94 try again.");
    }
    if (output_crc != meta->crc32) {
        ESP_LOGE(TAG, "article %lu: zstd CRC mismatch", (unsigned long)id);
        return finish_decode_error(req, any_sent,
                                   "Article verification failed \xe2\x80\x94 try again.");
    }
    ESP_LOGD(TAG, "article %lu: %lu bytes decoded (zstd), etag %s", (unsigned long)id,
             (unsigned long)total_out, etag);
    return finish_article_stream(req);
}

/* Stream a v1 (legacy) article: a gzip stream inflated with miniz. The v1
 * packer wrote minimal gzip headers (Python gzip.compress, flags = 0), but the
 * header is parsed generically (FEXTRA/FNAME/FCOMMENT/FHCRC) and the
 * uncompressed CRC from the index validates the result. */
static bool gzip_read_range(const pw_article_meta_t *meta, size_t offset,
                             uint8_t *buf, size_t len)
{
    if (offset > meta->comp_len || len > (size_t)meta->comp_len - offset) return false;
    return ca_read_payload(meta->content_offset + (uint32_t)offset, buf, len) == ESP_OK;
}

static bool gzip_read_byte(const pw_article_meta_t *meta, size_t *offset, size_t end,
                           uint8_t *value)
{
    if (*offset >= end || !gzip_read_range(meta, *offset, value, 1)) return false;
    (*offset)++;
    return true;
}

/* Stream a v1 (legacy) article: a gzip stream inflated with miniz. The gzip
 * header and trailer are read directly from the archive; decompressed bytes
 * are emitted from a static 32 KiB wrapping window. This preserves v1
 * compatibility while keeping peak heap use independent of article size. */
static esp_err_t stream_article_gzip(httpd_req_t *req, uint32_t id,
                                     const pw_article_meta_t *meta,
                                     const char *etag)
{
    if (meta->uncomp_len > PW_LEGACY_GZIP_MAX_UNCOMP || meta->comp_len < 18) {
        ESP_LOGE(TAG, "article %lu: invalid legacy gzip size", (unsigned long)id);
        return finish_decode_error(req, false, "Article is too large or invalid.");
    }

    size_t trailer_offset = (size_t)meta->comp_len - 8;
    uint8_t header[10];
    if (!gzip_read_range(meta, 0, header, sizeof header) ||
            header[0] != 0x1f || header[1] != 0x8b || header[2] != 8) {
        ESP_LOGE(TAG, "article %lu: not a gzip stream", (unsigned long)id);
        return finish_decode_error(req, false, "Article decode error -- try again.");
    }

    size_t pos = sizeof header;
    uint8_t flg = header[3];
    if (flg & 0x04) {
        uint8_t extra_len[2];
        if (!gzip_read_range(meta, pos, extra_len, sizeof extra_len)) {
            return finish_decode_error(req, false, "Article decode error -- try again.");
        }
        size_t xlen = (size_t)extra_len[0] | ((size_t)extra_len[1] << 8);
        pos += sizeof extra_len;
        if (pos > trailer_offset || xlen > trailer_offset - pos) {
            return finish_decode_error(req, false, "Article decode error -- try again.");
        }
        pos += xlen;
    }
    if (flg & 0x08 || flg & 0x10) {
        for (unsigned field = 0; field < 2; field++) {
            if ((field == 0 && !(flg & 0x08)) || (field == 1 && !(flg & 0x10))) continue;
            uint8_t byte;
            do {
                if (!gzip_read_byte(meta, &pos, trailer_offset, &byte)) {
                    return finish_decode_error(req, false, "Article decode error -- try again.");
                }
            } while (byte != 0);
        }
    }
    if (flg & 0x02) {
        if (pos > trailer_offset - 2) {
            return finish_decode_error(req, false, "Article decode error -- try again.");
        }
        pos += 2;
    }
    if (pos >= trailer_offset) {
        return finish_decode_error(req, false, "Article decode error -- try again.");
    }

    tinfl_init(&s_gzip_decomp);
    size_t input_offset = pos;
    size_t input_pos = 0;
    size_t input_len = 0;
    size_t total_out = 0;
    size_t bytes_since_yield = 0;
    uint32_t output_crc = 0;
    bool any_sent = false;
    tinfl_status status = TINFL_STATUS_FAILED;

    for (;;) {
        if (input_pos == input_len) {
            if (input_offset >= trailer_offset) {
                return finish_decode_error(req, any_sent,
                                           "Article decode error -- try again.");
            }
            size_t n = trailer_offset - input_offset;
            if (n > sizeof s_gzip_in) n = sizeof s_gzip_in;
            if (!gzip_read_range(meta, input_offset, s_gzip_in, n)) {
                return finish_decode_error(req, any_sent, "Article read error -- try again.");
            }
            input_offset += n;
            input_pos = 0;
            input_len = n;
        }

        size_t available = input_len - input_pos;
        size_t consumed = available;
        size_t window_offset = total_out & (PW_GZIP_WINDOW_SIZE - 1);
        size_t room = PW_GZIP_WINDOW_SIZE - window_offset;
        uint32_t flags = input_offset < trailer_offset ? TINFL_FLAG_HAS_MORE_INPUT : 0;
        size_t produced = room;
        status = tinfl_decompress(&s_gzip_decomp, s_gzip_in + input_pos, &consumed,
                                  s_gzip_window, s_gzip_window + window_offset,
                                  &produced, flags);
        input_pos += consumed;

        if (produced > 0) {
            if (produced > (size_t)meta->uncomp_len - total_out) {
                ESP_LOGE(TAG, "article %lu: gzip output exceeds declared length",
                         (unsigned long)id);
                return finish_decode_error(req, any_sent, "Article verification failed -- try again.");
            }
            total_out += produced;
            output_crc = esp_rom_crc32_le(output_crc, s_gzip_window + window_offset, produced);
            if (httpd_resp_send_chunk(req, (const char *)s_gzip_window + window_offset,
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
            if (input_pos != input_len || input_offset != trailer_offset) {
                ESP_LOGE(TAG, "article %lu: gzip stream has trailing input", (unsigned long)id);
                return finish_decode_error(req, any_sent, "Article verification failed -- try again.");
            }
            break;
        }
        if (status != TINFL_STATUS_NEEDS_MORE_INPUT &&
                status != TINFL_STATUS_HAS_MORE_OUTPUT) {
            ESP_LOGE(TAG, "article %lu: gzip inflate failed (status=%d)",
                     (unsigned long)id, (int)status);
            return finish_decode_error(req, any_sent, "Article decode error -- try again.");
        }
        if (status == TINFL_STATUS_NEEDS_MORE_INPUT && consumed == 0 &&
                input_offset >= trailer_offset) {
            return finish_decode_error(req, any_sent, "Article decode error -- try again.");
        }
    }

    uint8_t trailer[8];
    if (!gzip_read_range(meta, trailer_offset, trailer, sizeof trailer)) {
        return finish_decode_error(req, any_sent, "Article read error -- try again.");
    }
    uint32_t trailer_crc = (uint32_t)trailer[0] | ((uint32_t)trailer[1] << 8) |
        ((uint32_t)trailer[2] << 16) | ((uint32_t)trailer[3] << 24);
    uint32_t trailer_len = (uint32_t)trailer[4] | ((uint32_t)trailer[5] << 8) |
        ((uint32_t)trailer[6] << 16) | ((uint32_t)trailer[7] << 24);
    if (total_out != meta->uncomp_len || trailer_len != (uint32_t)total_out ||
            trailer_crc != output_crc || output_crc != meta->crc32) {
        ESP_LOGE(TAG, "article %lu: gzip length/CRC mismatch", (unsigned long)id);
        return finish_decode_error(req, any_sent, "Article verification failed -- try again.");
    }

    ESP_LOGD(TAG, "article %lu: %lu bytes decoded (gzip v1), etag %s",
             (unsigned long)id, (unsigned long)total_out, etag);
    return finish_article_stream(req);
}
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

/* Experimental browser-decompression endpoint.  It exposes exactly one
 * independently compressed article frame and leaves decompression to the
 * client.  The response is chunked so the ESP never allocates the frame (or
 * an article-sized buffer); clients that understand Content-Encoding: zstd
 * can consume it directly, while a WASM client can treat the body as an
 * opaque frame.  Legacy gzip packs remain served through /a/<id>. */
static esp_err_t handle_raw_article(httpd_req_t *req)
{
    const char *uri = req->uri;
    const char *query = strchr(uri, '?');
    size_t ulen = query != NULL ? (size_t)(query - uri) : strlen(uri);
    if (ulen < 8 || ulen > 24 || uri[7] == '\0') {
        return send_error(req, 400, "400 Bad Request", "Bad article id",
                          "Expected /raw/a/&lt;number&gt;.");
    }
    for (size_t i = 7; i < ulen; i++) {
        if (uri[i] < '0' || uri[i] > '9') {
            return send_error(req, 400, "400 Bad Request", "Bad article id",
                              "Article ids are plain numbers.");
        }
    }
    uint32_t id;
    if (!parse_u32_strict(uri + 7, ulen - 7, &id)) {
        return send_error(req, 400, "400 Bad Request", "Bad article id",
                          "Article id out of range.");
    }
    if (select_requested_library(req) != ESP_OK) {
        return send_error(req, 404, "404 Not Found", "Library not found",
                          "That library is no longer installed.");
    }
    const content_archive_t *ca = ca_get();
    uint32_t dict_len = 0;
    (void)ca_get_dict(&dict_len);
    if (!ca->valid || ca->format_version != PW_FORMAT_VERSION || dict_len != 0) {
        return send_error(req, 406, "406 Not Acceptable", "Compressed endpoint unavailable",
                          dict_len ? "This library requires a pack dictionary; use the browser decoder." :
                                     "This library uses the legacy gzip format.");
    }
    pw_article_meta_t meta;
    esp_err_t err = ca_get_article(id, &meta);
    if (err == ESP_ERR_NOT_FOUND) {
        return send_error(req, 404, "404 Not Found", "Article not found", "No article has this identifier.");
    }
    if (err != ESP_OK) {
        return send_error(req, 500, "500 Internal Server Error", "Corrupt index", "The article index entry could not be read safely.");
    }
    char etag[16];
    etag_from_crc(meta.crc32, etag);
    httpd_resp_set_type(req, "application/zstd");
    httpd_resp_set_hdr(req, "Content-Encoding", "zstd");
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=86400");
    httpd_resp_set_hdr(req, "ETag", etag);
    uint8_t buf[PW_CHUNK_SIZE];
    uint32_t offset = 0;
    while (offset < meta.comp_len) {
        size_t n = meta.comp_len - offset;
        if (n > sizeof buf) n = sizeof buf;
        if (ca_read_payload(meta.content_offset + offset, buf, n) != ESP_OK) {
            return ESP_FAIL;
        }
        err = httpd_resp_send_chunk(req, (const char *)buf, n);
        if (err != ESP_OK) return err;
        offset += (uint32_t)n;
        if (esp_task_wdt_status(NULL) == ESP_OK) esp_task_wdt_reset();
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

/* Downloads use the same bounded article decoder as /a/<id>, with attachment
 * headers and without conditional caching. */

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
    httpd_resp_send(req, (const char *)pocketwiki_style_css, len);
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
    httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
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
                     "\"clients\":%lu,\"heap_free\":%lu,\"heap_min\":%lu,\"heap_largest\":%lu,"
                     "\"zstd_decoder_bytes\":%lu,\"decoder_queue_depth\":%u,"
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
                     (unsigned long)esp_get_free_heap_size(),
                     (unsigned long)esp_get_minimum_free_heap_size(),
                     (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                     (unsigned long)(s_dstream != NULL ? ZSTD_sizeof_DStream(s_dstream) : 0),
                     pwpk_service_queue_depth(),
                     (unsigned long long)(esp_timer_get_time() / 1000000ULL));
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    if (n < 0) return ESP_FAIL;
    size_t body_len = (size_t)n < sizeof buf ? (size_t)n : sizeof buf - 1;
    httpd_resp_send(req, buf, body_len);
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
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
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
    httpd_resp_send_chunk(req, "{\"networks\":[", HTTPD_RESP_USE_STRLEN);
    char body[128];
    for (int i = 0; i < count; i++) {
        char escaped[70];
        json_escape(items[i].ssid, escaped, sizeof escaped);
        int n = snprintf(body, sizeof body,
            "%s{\"ssid\":\"%s\",\"rssi\":%d,\"open\":%s}", i ? "," : "",
            escaped, items[i].rssi, items[i].open ? "true" : "false");
        httpd_resp_send_chunk(req, body, (size_t)n);
    }
    httpd_resp_send_chunk(req, "]}\n", HTTPD_RESP_USE_STRLEN);
    return httpd_resp_send_chunk(req, NULL, 0);
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
                     "\"rssi\":%d,\"reason\":%u}\n",
                     ap_ssid, ap_ip, (unsigned long)wifi_ap_get_client_count(),
                     station.connected ? "true" : "false", station_ssid, station.ip,
                     station.rssi, (unsigned)station.disconnect_reason);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    if (n < 0) return ESP_FAIL;
    return httpd_resp_send(req, buf, (size_t)n);
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
    return httpd_resp_send(req, "{\"ok\":true,\"state\":\"connecting\"}\n", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handle_packs(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    uint32_t built_in_articles = 0;
    if (pack_store_open_for_read(PW_BUILTIN_PACK_NAME) == ESP_OK) built_in_articles = ca_get()->count;
    char built_in_title[PW_TITLE_CAP];
    pack_display_name(PW_BUILTIN_PACK_NAME, built_in_title, sizeof built_in_title);
    json_escape(built_in_title, s_render_label_esc, sizeof s_render_label_esc);
    int hlen = snprintf(s_render_item, sizeof s_render_item,
        "{\"packs\":[{\"name\":\"%s\",\"title\":\"%s\",\"articles\":%lu,\"bytes\":0,\"enabled\":true}",
        PW_BUILTIN_PACK_NAME, s_render_label_esc,
        (unsigned long)built_in_articles);
    httpd_resp_send_chunk(req, s_render_item, (size_t)hlen);
    pack_store_item_t *items = NULL;
    int count = pack_store_list_all(&items);
    for (int i = 0; i < count; i++) {
        char title[PW_TITLE_CAP];
        pack_display_name(items[i].name, title, sizeof title);
        json_escape(title, s_render_label_esc, sizeof s_render_label_esc);
        int len = snprintf(s_render_item, sizeof s_render_item,
            ",{\"name\":\"%s\",\"title\":\"%s\",\"articles\":%lu,\"bytes\":%lu,\"enabled\":true}",
            items[i].name, s_render_label_esc, (unsigned long)items[i].articles,
            (unsigned long)items[i].bytes);
        httpd_resp_send_chunk(req, s_render_item, (size_t)len);
    }
    free(items);
    size_t total = 0, used = 0;
    pack_store_usage(&total, &used);
    size_t installable = pack_store_installable_bytes();
    char tail[160];
    int tlen = snprintf(tail, sizeof tail,
                        "],\"bytes_used\":%u,\"bytes_total\":%u,\"bytes_free\":%u,\"bytes_installable\":%u}\n",
                        (unsigned)used, (unsigned)total,
                        (unsigned)(total > used ? total - used : 0), (unsigned)installable);
    httpd_resp_send_chunk(req, tail, (size_t)tlen);
    esp_err_t result = httpd_resp_send_chunk(req, NULL, 0);
    catalog_cache_invalidate();
    return result;
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
    return httpd_resp_send(req, "{\"ok\":true}\n", HTTPD_RESP_USE_STRLEN);
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
        oled_clear_transfer();
        return json_error(req, "422 Unprocessable Content",
                          "That file is not a valid PocketWiki pack.");
    }
    oled_clear_transfer();
    char body[96];
    snprintf(body, sizeof body, "{\"ok\":true,\"articles\":%lu}\n", (unsigned long)articles);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
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

/* Read the synced catalogue file into a malloc'd buffer. Returns false when
 * no synced copy exists. Caller owns *out. */
static bool catalog_file_json(char **out, size_t *len)
{
    FILE *fh = fopen(CATALOG_FILE, "rb");
    if (fh == NULL) return false;
    if (fseek(fh, 0, SEEK_END) != 0) { fclose(fh); return false; }
    long size = ftell(fh);
    if (size <= 0 || (unsigned long)size > CATALOG_FILE_MAX) { fclose(fh); return false; }
    if (fseek(fh, 0, SEEK_SET) != 0) { fclose(fh); return false; }
    char *buf = malloc((size_t)size + 1);
    if (buf == NULL) { fclose(fh); return false; }
    if (fread(buf, 1, (size_t)size, fh) != (size_t)size) {
        free(buf); fclose(fh); return false;
    }
    buf[size] = '\0';
    fclose(fh);
    *out = buf;
    *len = (size_t)size;
    return true;
}

typedef enum {
    CATALOG_SOURCE_EMBEDDED = 0,
    CATALOG_SOURCE_FILE = 1
} catalog_source_t;

static cJSON *s_catalog_root;
static char *s_catalog_text;
static size_t s_catalog_text_len;
static catalog_source_t s_catalog_source;
static int s_catalog_version = -1;
static bool s_catalog_cache_ready;

static int catalog_root_version(const cJSON *root)
{
    const cJSON *version = root ? cJSON_GetObjectItem(root, "catalog_version") : NULL;
    return cJSON_IsNumber(version) ? version->valueint : 0;
}

static const char *catalog_root_generated_at(const cJSON *root)
{
    const cJSON *generated = root ? cJSON_GetObjectItem(root, "generated_at") : NULL;
    return cJSON_IsString(generated) ? generated->valuestring : NULL;
}

/* Catalogues can be republished under the same major catalogue_version. Use
 * the timestamp to reject an older cached file; equal timestamps prefer the
 * firmware asset because it is the deterministic fallback shipped with this
 * image. */
static bool catalog_cached_is_newer(const cJSON *cached, const cJSON *embedded)
{
    int cached_version = catalog_root_version(cached);
    int embedded_version = catalog_root_version(embedded);
    if (cached_version != embedded_version) return cached_version > embedded_version;
    const char *cached_at = catalog_root_generated_at(cached);
    const char *embedded_at = catalog_root_generated_at(embedded);
    return cached_at != NULL && embedded_at != NULL && strcmp(cached_at, embedded_at) > 0;
}

static void catalog_cache_invalidate(void)
{
    cJSON_Delete(s_catalog_root);
    s_catalog_root = NULL;
    free(s_catalog_text);
    s_catalog_text = NULL;
    s_catalog_text_len = 0;
    s_catalog_version = -1;
    s_catalog_cache_ready = false;
}

/* Resolve the active catalogue once. The embedded asset is parsed directly
 * from flash, while a synced file is read and retained only when it wins the
 * version comparison. Subsequent display-name and install lookups reuse this
 * parsed tree until catalogue sync invalidates it. */
static bool catalog_cache_ensure(void)
{
    if (s_catalog_cache_ready) return s_catalog_root != NULL;

    char *cached_text = NULL;
    size_t cached_len = 0;
    catalog_file_json(&cached_text, &cached_len);
    cJSON *cached_root = cached_text != NULL
        ? cJSON_ParseWithLength(cached_text, cached_len) : NULL;
    cJSON *embedded_root = cJSON_ParseWithLength((const char *)pocketwiki_catalog_json,
                                                  pocketwiki_catalog_json_len);
    if (cached_root != NULL &&
            (embedded_root == NULL || catalog_cached_is_newer(cached_root, embedded_root))) {
        s_catalog_source = CATALOG_SOURCE_FILE;
        s_catalog_text = cached_text;
        s_catalog_text_len = cached_len;
        s_catalog_root = cached_root;
        cJSON_Delete(embedded_root);
    } else if (embedded_root != NULL) {
        s_catalog_source = CATALOG_SOURCE_EMBEDDED;
        s_catalog_root = embedded_root;
        free(cached_text);
        cJSON_Delete(cached_root);
    } else {
        free(cached_text);
        cJSON_Delete(cached_root);
    }
    s_catalog_version = catalog_root_version(s_catalog_root);
    s_catalog_cache_ready = true;
    ESP_LOGD(TAG, "catalogue cache selected source=%s version=%d",
             s_catalog_source == CATALOG_SOURCE_FILE ? "synced" : "embedded",
             s_catalog_version);
    return s_catalog_root != NULL;
}

static bool catalog_pack_name(const char *id, char *out, size_t cap)
{
    if (id == NULL || out == NULL || cap == 0 || !catalog_cache_ensure()) return false;

    bool found = false;
    cJSON *packs = cJSON_GetObjectItem(s_catalog_root, "packs");
    if (cJSON_IsArray(packs)) {
        cJSON *pack;
        cJSON_ArrayForEach(pack, packs) {
            cJSON *pid = cJSON_GetObjectItem(pack, "id");
            cJSON *pname = cJSON_GetObjectItem(pack, "name");
            if (cJSON_IsString(pid) && cJSON_IsString(pname) &&
                    strcmp(pid->valuestring, id) == 0) {
                strlcpy(out, pname->valuestring, cap);
                found = out[0] != '\0';
                break;
            }
        }
    }
    /* Keep the request-scoped catalogue tree alive. Pages commonly resolve
     * several installed pack names; reparsing the flash file for every name
     * made the management and home pages unnecessarily sluggish. page_close()
     * releases it after the response is complete. */
    return found;
}
static void pack_display_name(const char *id, char *out, size_t cap)
{
    if (catalog_pack_name(id, out, cap)) return;
    if (strcasecmp(id, PW_BUILTIN_PACK_NAME) == 0 || strcasecmp(id, "starter") == 0) {
        strlcpy(out, "Biology, Health & Medicine", cap);
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

/* Serve the pack catalogue: the synced live copy when present, else the
 * catalogue embedded at build time. */
static esp_err_t handle_packs_catalog(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    if (!catalog_cache_ensure()) {
        return json_error(req, "500 Internal Server Error", "catalogue unavailable");
    }
    esp_err_t result;
    if (s_catalog_source == CATALOG_SOURCE_FILE) {
        result = httpd_resp_send(req, s_catalog_text, (int)s_catalog_text_len);
    } else {
        result = httpd_resp_send(req, (const char *)pocketwiki_catalog_json,
                                 (int)pocketwiki_catalog_json_len);
    }
    catalog_cache_invalidate();
    return result;
}

/* Validate a catalogue document before persisting it: schema 1, non-empty
 * packs array, every entry with id/url/sha256 strings and a numeric bytes. */
static bool catalog_valid(const char *text)
{
    cJSON *root = cJSON_Parse(text);
    if (root == NULL) return false;
    bool ok = false;
    cJSON *schema = cJSON_GetObjectItem(root, "schema");
    cJSON *packs = cJSON_GetObjectItem(root, "packs");
    if (cJSON_IsNumber(schema) && schema->valuedouble == 1 &&
        cJSON_IsArray(packs) && cJSON_GetArraySize(packs) >= 1) {
        ok = true;
        cJSON *pack;
        cJSON_ArrayForEach(pack, packs) {
            cJSON *pid = cJSON_GetObjectItem(pack, "id");
            cJSON *purl = cJSON_GetObjectItem(pack, "url");
            cJSON *psha = cJSON_GetObjectItem(pack, "sha256");
            cJSON *pbytes = cJSON_GetObjectItem(pack, "bytes");
            if (!cJSON_IsString(pid) || !cJSON_IsString(purl) ||
                !cJSON_IsString(psha) || !cJSON_IsNumber(pbytes)) {
                ok = false;
                break;
            }
        }
    }
    cJSON_Delete(root);
    return ok;
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
    char *body = NULL;
    int64_t got = 0;
    const char *fail = NULL;
    {
        wifi_station_status_t st;
        wifi_ap_get_station(&st);
        if (!st.connected) {
            fail = "PocketWiki is not connected to Wi-Fi. Set up a 2.4 GHz network under Manage and try again.";
            goto sync_done;
        }
    }
    if (wifi_ap_wait_for_clock(15000) != ESP_OK) {
        fail = "PocketWiki cannot verify the connection time. Check internet access and try again.";
        goto sync_done;
    }
    wdt_relieve(true);
    if (client == NULL) {
        fail = "PocketWiki could not start the catalogue download.";
        goto sync_done;
    }
    {
        esp_err_t oerr = esp_http_client_open(client, 0);
        int64_t hlen = oerr == ESP_OK ? esp_http_client_fetch_headers(client) : -1;
        int status = oerr == ESP_OK ? esp_http_client_get_status_code(client) : 0;
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
    body = malloc((size_t)total + 1);
    if (body == NULL) {
        fail = "PocketWiki does not have enough memory to refresh the catalogue.";
        goto sync_done;
    }
    while (got < total) {
        int n = esp_http_client_read(client, body + got, (int)(total - got));
        if (n <= 0) break;
        got += n;
    }
    if (got != total) {
        fail = "The catalogue download was incomplete. Try again.";
        goto sync_done;
    }
    body[got] = '\0';
    if (!catalog_valid(body)) {
        fail = "The downloaded catalogue failed validation.";
        goto sync_done;
    }
    {
        FILE *fh = fopen("/packs/.catalog.tmp", "wb");
        if (fh == NULL || fwrite(body, 1, (size_t)got, fh) != (size_t)got) {
            if (fh) fclose(fh);
            unlink("/packs/.catalog.tmp");
            fail = "PocketWiki could not save the refreshed catalogue.";
            goto sync_done;
        }
        fclose(fh);
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
        catalog_cache_invalidate();
    }
    ESP_LOGI(TAG, "catalogue synced (%lld bytes)", (long long)got);

sync_done:
    if (client != NULL) esp_http_client_cleanup(client);
    wdt_relieve(false);
    if (fail != NULL) {
        free(body);
        httpd_resp_set_status(req, "502 Bad Gateway");
        return httpd_resp_sendstr(req, fail);
    }
    esp_err_t r = httpd_resp_send(req, body, (int)got);
    free(body);
    return r;
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

/* Find a curated pack in the pack catalogue by download URL. Returns true
 * and fills *bytes (-1 when unknown) and the lowercase SHA-256 hex (empty
 * string when absent) for the matching entry. Prefers the synced catalogue. */
static bool catalog_lookup(const char *url, int64_t *bytes, char sha_hex[65])
{
    if (url == NULL || !catalog_cache_ensure()) return false;
    cJSON *packs = cJSON_GetObjectItem(s_catalog_root, "packs");
    bool found = false;
    if (cJSON_IsArray(packs)) {
        cJSON *pack;
        cJSON_ArrayForEach(pack, packs) {
            cJSON *entry_url = cJSON_GetObjectItem(pack, "url");
            if (!cJSON_IsString(entry_url) || strcmp(entry_url->valuestring, url) != 0) continue;
            if (bytes) {
                cJSON *entry_bytes = cJSON_GetObjectItem(pack, "bytes");
                *bytes = cJSON_IsNumber(entry_bytes) ? (int64_t)entry_bytes->valuedouble : -1;
            }
            if (sha_hex) {
                cJSON *entry_sha = cJSON_GetObjectItem(pack, "sha256");
                if (cJSON_IsString(entry_sha)) strlcpy(sha_hex, entry_sha->valuestring, 65);
            }
            found = true;
            break;
        }
    }
    return found;
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
    progress.failed = httpd_resp_send_chunk(req, "P:0:0\n", HTTPD_RESP_USE_STRLEN) != ESP_OK;

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
        esp_err_t cres = progress.failed ? ESP_FAIL : httpd_resp_send_chunk(req, line, (size_t)len);
        if (cres != ESP_OK) {
            ESP_LOGE(TAG, "install: ERR chunk send failed: %s (browser stuck on stale progress)", esp_err_to_name(cres));
        }
    } else {
        uint32_t articles = 0;
        esp_err_t err = pack_store_finish_upload(fh, temp, name, &articles);
        if (err != ESP_OK) {
            esp_err_t cres = progress.failed ? ESP_FAIL : httpd_resp_send_chunk(req, "ERR:invalid pack\n", HTTPD_RESP_USE_STRLEN);
            if (cres != ESP_OK) {
                ESP_LOGE(TAG, "install: ERR chunk send failed: %s", esp_err_to_name(cres));
            }
        } else {
            char line[64];
            int len = snprintf(line, sizeof line, "OK:%lu\n", (unsigned long)articles);
            esp_err_t cres = progress.failed ? ESP_FAIL : httpd_resp_send_chunk(req, line, (size_t)len);
            if (cres != ESP_OK) {
                ESP_LOGE(TAG, "install: OK chunk send failed: %s", esp_err_to_name(cres));
            }
        }
    }
    esp_err_t result = progress.failed ? ESP_FAIL : httpd_resp_send_chunk(req, NULL, 0);
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
        return httpd_resp_send(req, "", 0);
    }

    const content_archive_t *ca = ca_get();
    if (!ca->valid) {
        if (!partial) {
            return send_error(req, 500, "500 Internal Server Error",
                              "Content archive not loaded", "");
        }
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        return httpd_resp_send(req, "", 0);
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
        httpd_resp_send_chunk(req, hdr, HTTPD_RESP_USE_STRLEN);
    } else {
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    }

    for (uint32_t id = offset; id < end; id++) {
        uint32_t tlen = 0;
        if (tl_read_display_title(&ix, id, s_render_title, sizeof s_render_title, &tlen) != 0) continue;
        if (html_escape((const char *)s_render_title, tlen, s_render_title_esc,
                        sizeof s_render_title_esc) == 0) continue;
        send_library_item(req, id, pack, s_render_title_esc);
    }

    if (!partial) {
        httpd_resp_send_chunk(req, "</div>", HTTPD_RESP_USE_STRLEN);

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
            httpd_resp_send_chunk(req, sentinel, HTTPD_RESP_USE_STRLEN);
        }

        return page_close(req);
    }
    return httpd_resp_send_chunk(req, NULL, 0);
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
    httpd_resp_send_chunk(req, buf, HTTPD_RESP_USE_STRLEN);

    httpd_resp_send_chunk(req,
        "<div class=\"wifi-status-card\" id=\"wifi-status-card\" aria-live=\"polite\">"
        "<p class=\"network-state\">Checking the connection…</p></div>",
        HTTPD_RESP_USE_STRLEN);

    httpd_resp_send_chunk(req, "<div class=\"list pack-list\">", HTTPD_RESP_USE_STRLEN);
    pack_store_item_t *items = NULL;
    int n = pack_store_list_all(&items);
    pack_display_name(PW_BUILTIN_PACK_NAME, (char *)s_render_title, sizeof s_render_title);
    if (html_escape((const char *)s_render_title, strlen((const char *)s_render_title),
                    s_render_title_esc, sizeof s_render_title_esc) == 0) {
        strlcpy(s_render_title_esc, "Biology, Health &amp; Medicine",
                sizeof s_render_title_esc);
    }
    SEND_LITERAL(req, "<p><strong>");
    httpd_resp_send_chunk(req, s_render_title_esc, strlen(s_render_title_esc));
    SEND_LITERAL(req, "</strong> &middot; included and always available</p>");
    if (n > 0) {
        for (int i = 0; i < n; i++) {
            pack_display_name(items[i].name, (char *)s_render_title, sizeof s_render_title);
            if (html_escape((const char *)s_render_title,
                            strlen((const char *)s_render_title), s_render_title_esc,
                            sizeof s_render_title_esc) == 0) continue;
            snprintf(buf, sizeof buf, "<p data-pack-name=\"%.40s\"><strong>", items[i].name);
            httpd_resp_send_chunk(req, buf, strlen(buf));
            httpd_resp_send_chunk(req, s_render_title_esc, strlen(s_render_title_esc));
            snprintf(buf, sizeof buf,
                     "</strong> &middot; %lu articles &middot; installed "
                     "<button class=\"inline-action danger\" data-action=\"delete\" data-name=\"%.40s\">Remove</button></p>",
                     (unsigned long)items[i].articles, items[i].name);
            httpd_resp_send_chunk(req, buf, HTTPD_RESP_USE_STRLEN);
        }
    }
    free(items);
    httpd_resp_send_chunk(req, "</div>", HTTPD_RESP_USE_STRLEN);

    httpd_resp_send_chunk(req,
        "<section class=\"manager-section\"><div class=\"catalog-head\"><div>"
"<h2>Add a library pack</h2>"
"<p class=\"muted\">Choose a pack from the catalogue, or install a <code>.pwp</code> file you already have. Catalogue downloads need internet access.</p></div>"
"<div class=\"catalog-sync\"><button class=\"scan-action\" id=\"catalog-sync\" type=\"button\">Refresh catalogue</button>"
"<span id=\"catalog-synced\" class=\"synced-at\" hidden></span></div></div>"
"<div id=\"pack-catalog\" class=\"catalog-list\"><p class=\"network-state\">Loading the catalogue…</p></div>"
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
"<p id=\"wifi-status\" class=\"muted\" aria-live=\"polite\"></p></div></section>",
        HTTPD_RESP_USE_STRLEN);
    SEND_LITERAL(req, "<script>");
    httpd_resp_send_chunk(req, (const char *)pocketwiki_manager_js,
                          pocketwiki_manager_js_len);
    SEND_LITERAL(req, "</script>");
    snprintf(buf, sizeof buf, "<p><a href=\"/\">&larr; Back to your libraries</a></p>");
    httpd_resp_send_chunk(req, buf, HTTPD_RESP_USE_STRLEN);

    return page_close(req);
}

static esp_err_t handle_legacy_packs_page(httpd_req_t *req)
{
    httpd_resp_set_status(req, "301 Moved Permanently");
    httpd_resp_set_hdr(req, "Location", "/manage");
    return httpd_resp_sendstr(req, "Moved permanently to /manage\n");
}

/* ---- init ---- */

esp_err_t web_server_init(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
#if CONFIG_IDF_TARGET_ESP32S3
    cfg.stack_size = PW_HTTPD_STACK_BYTES;
    cfg.core_id = 1;                          /* S3: keep httpd off Core 0 (WiFi/LWIP) */
    cfg.send_wait_timeout = 10;               /* S3: larger articles need more time to flush */
#else
    cfg.stack_size = PW_HTTPD_STACK_BYTES;    /* gzip inflate + 2 KB chunk + headroom */
    cfg.send_wait_timeout = 3;
#endif
    cfg.max_open_sockets = CONFIG_POCKETWIKI_HTTPD_MAX_SOCKETS;
    cfg.max_uri_handlers = 28;
    cfg.lru_purge_enable = true;
    cfg.recv_wait_timeout = 3;                /* faster socket recycling (was 5) */
    cfg.uri_match_fn = httpd_uri_match_wildcard;

    httpd_handle_t server = NULL;
    ESP_RETURN_ON_ERROR(httpd_start(&server, &cfg), TAG, "httpd start");

#if CONFIG_POCKETWIKI_PWPK_SERVICE
    ESP_RETURN_ON_ERROR(pwpk_http_register(server), TAG, "start PWPK service");
#endif

    /* Prepare zstd compatibility state at boot. The normal built-in biology
     * archive is gzip and does not retain a dictionary. */
    if (rebuild_ddict() != ESP_OK) {
        ESP_LOGW(TAG, "zstd dictionary pre-allocation deferred");
    }

    static const httpd_uri_t uris[] = {
        { .uri = "/",           .method = HTTP_GET,  .handler = handle_root },
        { .uri = "/search",     .method = HTTP_GET,  .handler = handle_search },
        { .uri = "/download/*", .method = HTTP_GET,  .handler = handle_download_id },
        { .uri = "/a/*",        .method = HTTP_GET,  .handler = handle_article_id,
          .user_ctx = NULL },
        { .uri = "/raw/a/*",    .method = HTTP_GET,  .handler = handle_raw_article },
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
        { .uri = "/api/packs/upload", .method = HTTP_POST, .handler = handle_pack_upload },
        { .uri = "/api/wifi/scan", .method = HTTP_GET, .handler = handle_wifi_scan },
        { .uri = "/api/wifi/status", .method = HTTP_GET, .handler = handle_wifi_status },
        { .uri = "/api/wifi/config", .method = HTTP_POST, .handler = handle_wifi_config },
        { .uri = "/health",     .method = HTTP_GET,  .handler = handle_health },
        { .uri = "/api/stats",  .method = HTTP_GET,  .handler = handle_stats },
    };
    for (size_t i = 0; i < sizeof uris / sizeof uris[0]; i++) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &uris[i]), TAG, "register uri");
    }
    /* catch-all */
    httpd_uri_t all = { .uri = "/*", .method = HTTP_GET, .handler = handle_404 };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &all), TAG, "register catch-all");

    ESP_LOGI(TAG, "HTTP server ready (%d sockets)", cfg.max_open_sockets);
    return ESP_OK;
}
