/* esp_http_server on POSIX sockets.
 *
 * The device's server runs every handler in one task, select()s over a small
 * session pool, purges the least recently used session when the pool is full,
 * and enforces a request-header and URI budget. This keeps all four, so a
 * route that is slow, leaks a session, or depends on a large header behaves
 * here as it does on a board.
 *
 * It also checks the firmware's side of the protocol, which the device cannot
 * report: a handler that returns without answering, or a chunked response that
 * is never terminated, is logged as a protocol error (and aborts the process
 * when PW_SIM_STRICT=1, so the test harness fails loudly).
 */
#include "esp_http_server.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>

#include "esp_log.h"

static const char *TAG = "sim-httpd";

#define MAX_HANDLERS 64
#define MAX_SESSIONS 16
#define MAX_REQ_HDR_LEN CONFIG_HTTPD_MAX_REQ_HDR_LEN
#define RESP_HDR_MAX 8
#define RX_BUF 4096

typedef struct {
    int fd;
    uint64_t last_used;
    char rx[RX_BUF];                /* bytes received beyond the current position */
    size_t rx_len;
} session_t;

typedef struct {
    httpd_config_t cfg;
    httpd_uri_t handlers[MAX_HANDLERS];
    int handler_count;
    int listen_fd;
    session_t sessions[MAX_SESSIONS];
    pthread_t thread;
    uint64_t clock;
} server_t;

typedef struct {
    session_t *sess;
    server_t *srv;
    char headers[MAX_REQ_HDR_LEN + 1];
    size_t headers_len;
    size_t body_remaining;
    const char *status;
    const char *type;
    const char *hdr_field[RESP_HDR_MAX];
    const char *hdr_value[RESP_HDR_MAX];
    int hdr_count;
    bool headers_sent;
    bool chunked;
    bool terminated;
    bool close_after;
    bool send_failed;
} req_aux_t;

static uint16_t s_port = 8080;
static uint16_t s_bound_port;
static bool s_strict;

void sim_httpd_set_port(uint16_t port) { s_port = port; }
uint16_t sim_httpd_bound_port(void) { return s_bound_port; }

static void protocol_error(const char *what, const char *uri)
{
    ESP_LOGE(TAG, "PROTOCOL ERROR: %s (%s)", what, uri);
    fprintf(stderr, "PWSIM-PROTOCOL-ERROR %s %s\n", what, uri);
    if (s_strict) abort();
}

/* ---- matching ------------------------------------------------------------ */

bool httpd_uri_match_wildcard(const char *tmpl, const char *uri, size_t len)
{
    size_t tlen = strlen(tmpl);
    if (tlen > 0 && tmpl[tlen - 1] == '*') {
        size_t prefix = tlen - 1;
        if (len >= prefix && strncmp(tmpl, uri, prefix) == 0) return true;
        /* "/a/<anything>" templates also match "/a" */
        if (prefix > 0 && tmpl[prefix - 1] == '/' && len == prefix - 1 &&
                strncmp(tmpl, uri, len) == 0) return true;
        return false;
    }
    return tlen == len && strncmp(tmpl, uri, len) == 0;
}

/* ---- socket helpers ------------------------------------------------------ */

static int send_all(int fd, const char *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        ssize_t n = send(fd, buf + off, len - off, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        off += (size_t)n;
    }
    return 0;
}

static int raw_socket_send(int sockfd, const char *buf, size_t len, int flags)
{
    ssize_t n = send(sockfd, buf, len, flags);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return HTTPD_SOCK_ERR_TIMEOUT;
        return HTTPD_SOCK_ERR_FAIL;
    }
    return (int)n;
}

/* The firmware ends chunked responses itself through httpd_socket_send(); note
 * when that happens so an unterminated response can be reported. */
static __thread req_aux_t *t_current;

int httpd_socket_send(httpd_handle_t hd, int fd, const char *buf, size_t len, int flags)
{
    (void)hd;
    int n = raw_socket_send(fd, buf, len, flags);
    if (t_current != NULL && n == (int)len && len == 5 && memcmp(buf, "0\r\n\r\n", 5) == 0) {
        t_current->terminated = true;
    }
    return n;
}

/* ---- response ------------------------------------------------------------ */

esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *status)
{
    ((req_aux_t *)r->aux)->status = status;
    return ESP_OK;
}

esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *type)
{
    ((req_aux_t *)r->aux)->type = type;
    return ESP_OK;
}

esp_err_t httpd_resp_set_hdr(httpd_req_t *r, const char *field, const char *value)
{
    req_aux_t *ra = r->aux;
    for (int i = 0; i < ra->hdr_count; i++) {
        if (strcasecmp(ra->hdr_field[i], field) == 0) {
            ra->hdr_value[i] = value;
            return ESP_OK;
        }
    }
    if (ra->hdr_count >= RESP_HDR_MAX) {
        ESP_LOGE(TAG, "too many response headers (max %d) on %s", RESP_HDR_MAX, r->uri);
        return ESP_ERR_INVALID_SIZE;
    }
    ra->hdr_field[ra->hdr_count] = field;
    ra->hdr_value[ra->hdr_count] = value;
    ra->hdr_count++;
    return ESP_OK;
}

static esp_err_t send_headers(httpd_req_t *r, bool chunked, size_t content_len)
{
    req_aux_t *ra = r->aux;
    char head[2048];
    int n = snprintf(head, sizeof head, "HTTP/1.1 %s\r\nContent-Type: %s\r\n",
                     ra->status ? ra->status : "200 OK",
                     ra->type ? ra->type : "text/html");
    if (chunked) {
        n += snprintf(head + n, sizeof head - (size_t)n, "Transfer-Encoding: chunked\r\n");
    } else {
        n += snprintf(head + n, sizeof head - (size_t)n, "Content-Length: %zu\r\n", content_len);
    }
    for (int i = 0; i < ra->hdr_count; i++) {
        if (strcasecmp(ra->hdr_field[i], "Content-Length") == 0) continue;
        if (strcasecmp(ra->hdr_field[i], "Connection") == 0 &&
                strcasecmp(ra->hdr_value[i], "close") == 0) ra->close_after = true;
        n += snprintf(head + n, sizeof head - (size_t)n, "%s: %s\r\n",
                      ra->hdr_field[i], ra->hdr_value[i]);
    }
    n += snprintf(head + n, sizeof head - (size_t)n, "\r\n");
    ra->headers_sent = true;
    ra->chunked = chunked;
    if (send_all(ra->sess->fd, head, (size_t)n) != 0) {
        ra->send_failed = true;
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t httpd_resp_send(httpd_req_t *r, const char *buf, ssize_t len)
{
    req_aux_t *ra = r->aux;
    if (ra->headers_sent) {
        protocol_error("httpd_resp_send after headers were already sent", r->uri);
        return ESP_ERR_INVALID_STATE;
    }
    if (buf == NULL) len = 0;
    else if (len == HTTPD_RESP_USE_STRLEN) len = (ssize_t)strlen(buf);
    if (send_headers(r, false, (size_t)len) != ESP_OK) return ESP_FAIL;
    if (len > 0 && send_all(ra->sess->fd, buf, (size_t)len) != 0) {
        ra->send_failed = true;
        return ESP_FAIL;
    }
    ra->terminated = true;
    return ESP_OK;
}

esp_err_t httpd_resp_send_chunk(httpd_req_t *r, const char *buf, ssize_t len)
{
    req_aux_t *ra = r->aux;
    if (buf == NULL) len = 0;
    else if (len == HTTPD_RESP_USE_STRLEN) len = (ssize_t)strlen(buf);
    if (!ra->headers_sent) {
        if (send_headers(r, true, 0) != ESP_OK) return ESP_FAIL;
    } else if (!ra->chunked) {
        protocol_error("httpd_resp_send_chunk after a non-chunked response", r->uri);
        return ESP_ERR_INVALID_STATE;
    }
    char frame[16];
    int n = snprintf(frame, sizeof frame, "%zx\r\n", (size_t)len);
    if (send_all(ra->sess->fd, frame, (size_t)n) != 0 ||
            (len > 0 && send_all(ra->sess->fd, buf, (size_t)len) != 0) ||
            send_all(ra->sess->fd, "\r\n", 2) != 0) {
        ra->send_failed = true;
        return ESP_FAIL;
    }
    if (len == 0) ra->terminated = true;
    return ESP_OK;
}

int httpd_req_to_sockfd(httpd_req_t *r)
{
    req_aux_t *ra = r->aux;
    return ra->sess ? ra->sess->fd : -1;
}

esp_err_t httpd_sess_trigger_close(httpd_handle_t handle, int sockfd)
{
    (void)handle;
    if (t_current != NULL && t_current->sess->fd == sockfd) t_current->close_after = true;
    return ESP_OK;
}

/* ---- request ------------------------------------------------------------- */

int httpd_req_recv(httpd_req_t *r, char *buf, size_t len)
{
    req_aux_t *ra = r->aux;
    session_t *s = ra->sess;
    if (len > ra->body_remaining) len = ra->body_remaining;
    if (len == 0) return 0;
    if (s->rx_len > 0) {
        size_t n = s->rx_len < len ? s->rx_len : len;
        memcpy(buf, s->rx, n);
        memmove(s->rx, s->rx + n, s->rx_len - n);
        s->rx_len -= n;
        ra->body_remaining -= n;
        return (int)n;
    }
    for (;;) {
        ssize_t n = recv(s->fd, buf, len, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return HTTPD_SOCK_ERR_TIMEOUT;
        if (n <= 0) return HTTPD_SOCK_ERR_FAIL;
        ra->body_remaining -= (size_t)n;
        return (int)n;
    }
}

size_t httpd_req_get_url_query_len(httpd_req_t *r)
{
    const char *q = strchr(r->uri, '?');
    return q == NULL ? 0 : strlen(q + 1);
}

esp_err_t httpd_req_get_url_query_str(httpd_req_t *r, char *buf, size_t len)
{
    const char *q = strchr(r->uri, '?');
    if (q == NULL) return ESP_ERR_NOT_FOUND;
    size_t data_len = strlen(q + 1);
    size_t copy = data_len < len - 1 ? data_len : len - 1;
    if (copy < data_len) return ESP_ERR_HTTPD_RESULT_TRUNC;
    memcpy(buf, q + 1, copy);
    buf[copy] = '\0';
    return ESP_OK;
}

/* Verbatim port of IDF's httpd_query_key_value(). */
esp_err_t httpd_query_key_value(const char *qry_str, const char *key, char *val, size_t val_size)
{
    if (qry_str == NULL || key == NULL || val == NULL) return ESP_ERR_INVALID_ARG;
    const char *qry_ptr = qry_str;
    while (strlen(qry_ptr)) {
        const char *val_ptr = strchr(qry_ptr, '=');
        if (!val_ptr) break;
        size_t offset = (size_t)(val_ptr - qry_ptr);
        if (offset != strlen(key) || strncasecmp(qry_ptr, key, offset)) {
            qry_ptr = strchr(val_ptr, '&');
            if (!qry_ptr) break;
            qry_ptr++;
            continue;
        }
        qry_ptr = strchr(++val_ptr, '&');
        if (!qry_ptr) qry_ptr = val_ptr + strlen(val_ptr);
        size_t val_len = (size_t)(qry_ptr - val_ptr);
        size_t copy_len = val_len < val_size - 1 ? val_len : val_size - 1;
        if (copy_len < val_len) return ESP_ERR_HTTPD_RESULT_TRUNC;
        memcpy(val, val_ptr, copy_len);
        val[copy_len] = '\0';
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

static const char *find_header(httpd_req_t *r, const char *field)
{
    req_aux_t *ra = r->aux;
    size_t flen = strlen(field);
    const char *p = ra->headers;
    while (*p) {
        const char *eol = strstr(p, "\r\n");
        if (eol == NULL) eol = p + strlen(p);
        if ((size_t)(eol - p) > flen && p[flen] == ':' && strncasecmp(p, field, flen) == 0) {
            const char *v = p + flen + 1;
            while (*v == ' ') v++;
            return v;
        }
        if (*eol == '\0') break;
        p = eol + 2;
    }
    return NULL;
}

static size_t header_len(const char *v)
{
    const char *eol = strstr(v, "\r\n");
    return eol ? (size_t)(eol - v) : strlen(v);
}

size_t httpd_req_get_hdr_value_len(httpd_req_t *r, const char *field)
{
    const char *v = find_header(r, field);
    return v ? header_len(v) : 0;
}

esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *r, const char *field, char *val, size_t size)
{
    const char *v = find_header(r, field);
    if (v == NULL) return ESP_ERR_NOT_FOUND;
    size_t n = header_len(v);
    size_t copy = n < size - 1 ? n : size - 1;
    memcpy(val, v, copy);
    val[copy] = '\0';
    return copy < n ? ESP_ERR_HTTPD_RESULT_TRUNC : ESP_OK;
}

/* ---- server loop --------------------------------------------------------- */

static void session_close(session_t *s)
{
    if (s->fd >= 0) close(s->fd);
    s->fd = -1;
    s->rx_len = 0;
}

static void set_timeouts(int fd, const httpd_config_t *cfg)
{
    struct timeval rcv = { .tv_sec = cfg->recv_wait_timeout };
    struct timeval snd = { .tv_sec = cfg->send_wait_timeout };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcv, sizeof rcv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &snd, sizeof snd);
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
}

static void send_status_only(int fd, const char *status)
{
    char buf[160];
    int n = snprintf(buf, sizeof buf,
                     "HTTP/1.1 %s\r\nContent-Type: text/html\r\nContent-Length: 0\r\n"
                     "Connection: close\r\n\r\n", status);
    send_all(fd, buf, (size_t)n);
}

static int method_from(const char *m, size_t len)
{
    if (len == 3 && memcmp(m, "GET", 3) == 0) return HTTP_GET;
    if (len == 4 && memcmp(m, "POST", 4) == 0) return HTTP_POST;
    if (len == 4 && memcmp(m, "HEAD", 4) == 0) return HTTP_HEAD;
    if (len == 3 && memcmp(m, "PUT", 3) == 0) return HTTP_PUT;
    if (len == 6 && memcmp(m, "DELETE", 6) == 0) return HTTP_DELETE;
    return -1;
}

/* Serve one request from a readable session. Returns false when the session
 * must be closed. */
static bool serve_one(server_t *srv, session_t *s)
{
    /* Accumulate until the end of the header block. */
    char *end = NULL;
    for (;;) {
        if (s->rx_len > 0) {
            s->rx[s->rx_len < RX_BUF ? s->rx_len : RX_BUF - 1] = '\0';
            end = strstr(s->rx, "\r\n\r\n");
            if (end != NULL) break;
        }
        if (s->rx_len >= RX_BUF - 1) {
            send_status_only(s->fd, "431 Request Header Fields Too Large");
            return false;
        }
        ssize_t n = recv(s->fd, s->rx + s->rx_len, RX_BUF - 1 - s->rx_len, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;            /* closed or recv timeout */
        s->rx_len += (size_t)n;
    }

    size_t head_len = (size_t)(end - s->rx) + 4;
    char *line_end = strstr(s->rx, "\r\n");
    char *sp1 = memchr(s->rx, ' ', (size_t)(line_end - s->rx));
    char *sp2 = sp1 ? memchr(sp1 + 1, ' ', (size_t)(line_end - sp1 - 1)) : NULL;
    if (sp1 == NULL || sp2 == NULL) {
        send_status_only(s->fd, "400 Bad Request");
        return false;
    }
    int method = method_from(s->rx, (size_t)(sp1 - s->rx));
    size_t uri_len = (size_t)(sp2 - sp1 - 1);
    if (uri_len > CONFIG_HTTPD_MAX_URI_LEN) {
        send_status_only(s->fd, "414 URI Too Long");
        return false;
    }
    size_t hdrs_len = head_len - (size_t)(line_end + 2 - s->rx) - 2;
    if (hdrs_len > MAX_REQ_HDR_LEN) {
        send_status_only(s->fd, "431 Request Header Fields Too Large");
        return false;
    }

    httpd_req_t req = { .handle = srv, .method = method };
    req_aux_t ra = { .sess = s, .srv = srv };
    memcpy(req.uri, sp1 + 1, uri_len);
    req.uri[uri_len] = '\0';
    memcpy(ra.headers, line_end + 2, hdrs_len);
    ra.headers[hdrs_len] = '\0';
    req.aux = &ra;

    const char *cl = find_header(&req, "Content-Length");
    req.content_len = cl ? strtoul(cl, NULL, 10) : 0;
    ra.body_remaining = req.content_len;
    const char *conn = find_header(&req, "Connection");
    bool http10 = strncmp(sp2 + 1, "HTTP/1.0", 8) == 0;
    if ((conn && strncasecmp(conn, "close", 5) == 0) || (http10 && !(conn && strncasecmp(conn, "keep-alive", 10) == 0))) {
        ra.close_after = true;
    }

    /* Keep only the body bytes that followed the header block. */
    memmove(s->rx, s->rx + head_len, s->rx_len - head_len);
    s->rx_len -= head_len;

    const char *q = strchr(req.uri, '?');
    size_t path_len = q ? (size_t)(q - req.uri) : uri_len;
    const httpd_uri_t *match = NULL;
    bool uri_matched = false;
    for (int i = 0; i < srv->handler_count; i++) {
        const httpd_uri_t *h = &srv->handlers[i];
        bool m = srv->cfg.uri_match_fn
            ? srv->cfg.uri_match_fn(h->uri, req.uri, path_len)
            : (strlen(h->uri) == path_len && strncmp(h->uri, req.uri, path_len) == 0);
        if (!m) continue;
        uri_matched = true;
        if ((int)h->method == method) { match = h; break; }
    }

    int64_t t0 = 0;
    {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        t0 = (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
    }
    if (match == NULL) {
        send_status_only(s->fd, uri_matched ? "405 Method Not Allowed" : "404 Not Found");
        return false;
    }

    req.user_ctx = match->user_ctx;
    t_current = &ra;
    esp_err_t ret = match->handler(&req);
    t_current = NULL;

    struct timeval tv;
    gettimeofday(&tv, NULL);
    int64_t dt = (int64_t)tv.tv_sec * 1000000 + tv.tv_usec - t0;
    ESP_LOGI(TAG, "%s %s -> %s%s (%lld ms)", method == HTTP_POST ? "POST" : "GET", req.uri,
             ra.status ? ra.status : "200 OK", ret == ESP_OK ? "" : " [handler error]",
             (long long)(dt / 1000));

    if (!ra.send_failed && !ra.close_after) {
        if (!ra.headers_sent && ret == ESP_OK) {
            protocol_error("handler returned ESP_OK without sending a response", req.uri);
        } else if (ra.headers_sent && ra.chunked && !ra.terminated && ret == ESP_OK) {
            protocol_error("chunked response was never terminated", req.uri);
        }
    }

    /* Discard any body the handler did not read, as IDF does. */
    char sink[512];
    while (ra.body_remaining > 0 && !ra.send_failed) {
        int n = httpd_req_recv(&req, sink, sizeof sink);
        if (n <= 0) { ra.close_after = true; break; }
    }
    if (ret != ESP_OK || ra.send_failed || ra.close_after) return false;
    return true;
}

static void *server_main(void *arg)
{
    server_t *srv = arg;
    for (;;) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(srv->listen_fd, &rfds);
        int maxfd = srv->listen_fd;
        for (int i = 0; i < srv->cfg.max_open_sockets; i++) {
            int fd = srv->sessions[i].fd;
            if (fd >= 0) {
                FD_SET(fd, &rfds);
                if (fd > maxfd) maxfd = fd;
            }
        }
        if (select(maxfd + 1, &rfds, NULL, NULL, NULL) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (FD_ISSET(srv->listen_fd, &rfds)) {
            int fd = accept(srv->listen_fd, NULL, NULL);
            if (fd >= 0) {
                session_t *slot = NULL;
                session_t *lru = NULL;
                for (int i = 0; i < srv->cfg.max_open_sockets; i++) {
                    session_t *s = &srv->sessions[i];
                    if (s->fd < 0) { slot = s; break; }
                    if (lru == NULL || s->last_used < lru->last_used) lru = s;
                }
                if (slot == NULL && srv->cfg.lru_purge_enable && lru != NULL) {
                    ESP_LOGW(TAG, "session pool full (%d): purging LRU fd %d",
                             srv->cfg.max_open_sockets, lru->fd);
                    session_close(lru);
                    slot = lru;
                }
                if (slot == NULL) {
                    ESP_LOGW(TAG, "session pool full: refusing connection");
                    close(fd);
                } else {
                    set_timeouts(fd, &srv->cfg);
                    int one = 1;
                    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
                    slot->fd = fd;
                    slot->rx_len = 0;
                    slot->last_used = ++srv->clock;
                }
            }
        }
        for (int i = 0; i < srv->cfg.max_open_sockets; i++) {
            session_t *s = &srv->sessions[i];
            if (s->fd < 0 || !FD_ISSET(s->fd, &rfds)) continue;
            s->last_used = ++srv->clock;
            if (!serve_one(srv, s)) session_close(s);
        }
    }
    return NULL;
}

esp_err_t httpd_start(httpd_handle_t *handle, const httpd_config_t *config)
{
    const char *strict = getenv("PW_SIM_STRICT");
    s_strict = strict != NULL && strcmp(strict, "1") == 0;
    server_t *srv = calloc(1, sizeof *srv);
    if (srv == NULL) return ESP_ERR_NO_MEM;
    srv->cfg = *config;
    if (srv->cfg.max_open_sockets > MAX_SESSIONS) srv->cfg.max_open_sockets = MAX_SESSIONS;
    for (int i = 0; i < MAX_SESSIONS; i++) srv->sessions[i].fd = -1;

    srv->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(srv->listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(s_port) };
    const char *bind_addr = getenv("PW_SIM_BIND");
    addr.sin_addr.s_addr = bind_addr ? inet_addr(bind_addr) : htonl(INADDR_LOOPBACK);
    if (bind(srv->listen_fd, (struct sockaddr *)&addr, sizeof addr) != 0 ||
            listen(srv->listen_fd, config->backlog_conn) != 0) {
        ESP_LOGE(TAG, "cannot listen on port %u: %s", s_port, strerror(errno));
        close(srv->listen_fd);
        free(srv);
        return ESP_FAIL;
    }
    socklen_t alen = sizeof addr;
    getsockname(srv->listen_fd, (struct sockaddr *)&addr, &alen);
    s_bound_port = ntohs(addr.sin_port);
    if (pthread_create(&srv->thread, NULL, server_main, srv) != 0) {
        close(srv->listen_fd);
        free(srv);
        return ESP_FAIL;
    }
    *handle = srv;
    return ESP_OK;
}

esp_err_t httpd_stop(httpd_handle_t handle)
{
    (void)handle;
    return ESP_OK;
}

esp_err_t httpd_register_uri_handler(httpd_handle_t handle, const httpd_uri_t *uri)
{
    server_t *srv = handle;
    if (srv->handler_count >= srv->cfg.max_uri_handlers || srv->handler_count >= MAX_HANDLERS) {
        ESP_LOGE(TAG, "no slot for %s: max_uri_handlers=%u", uri->uri, srv->cfg.max_uri_handlers);
        return ESP_ERR_NO_MEM;
    }
    srv->handlers[srv->handler_count++] = *uri;
    return ESP_OK;
}
