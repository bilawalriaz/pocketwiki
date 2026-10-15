#pragma once
/* A POSIX-socket stand-in for ESP-IDF's esp_http_server, with the same
 * single-task, one-handler-at-a-time model as the device. Query and header
 * helpers copy IDF's truncation semantics, so a buffer that is too small on
 * the device is too small here. */
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "esp_err.h"

typedef void *httpd_handle_t;

typedef enum { HTTP_DELETE = 0, HTTP_GET = 1, HTTP_HEAD = 2, HTTP_POST = 3, HTTP_PUT = 4 } httpd_method_t;

#define HTTPD_RESP_USE_STRLEN -1
#define HTTPD_SOCK_ERR_FAIL -1
#define HTTPD_SOCK_ERR_INVALID -2
#define HTTPD_SOCK_ERR_TIMEOUT -3

typedef struct httpd_req {
    httpd_handle_t handle;
    int method;
    char uri[CONFIG_HTTPD_MAX_URI_LEN + 1];
    size_t content_len;
    void *aux;
    void *user_ctx;
} httpd_req_t;

typedef bool (*httpd_uri_match_func_t)(const char *tmpl, const char *uri, size_t len);

typedef struct {
    unsigned task_priority;
    size_t stack_size;
    int core_id;
    uint16_t server_port;
    uint16_t ctrl_port;
    uint16_t max_open_sockets;
    uint16_t max_uri_handlers;
    uint16_t max_resp_headers;
    uint16_t backlog_conn;
    bool lru_purge_enable;
    uint16_t recv_wait_timeout;
    uint16_t send_wait_timeout;
    httpd_uri_match_func_t uri_match_fn;
} httpd_config_t;

#define HTTPD_DEFAULT_CONFIG() { .task_priority = 5, .stack_size = 4096, .core_id = 0x7fffffff, \
    .server_port = 80, .ctrl_port = 32768, .max_open_sockets = 7, .max_uri_handlers = 8, \
    .max_resp_headers = 8, .backlog_conn = 5, .lru_purge_enable = false, \
    .recv_wait_timeout = 5, .send_wait_timeout = 5, .uri_match_fn = NULL }

typedef struct {
    const char *uri;
    httpd_method_t method;
    esp_err_t (*handler)(httpd_req_t *r);
    void *user_ctx;
} httpd_uri_t;

esp_err_t httpd_start(httpd_handle_t *handle, const httpd_config_t *config);
esp_err_t httpd_stop(httpd_handle_t handle);
esp_err_t httpd_register_uri_handler(httpd_handle_t handle, const httpd_uri_t *uri);
bool httpd_uri_match_wildcard(const char *tmpl, const char *uri, size_t len);

esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *status);
esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *type);
esp_err_t httpd_resp_set_hdr(httpd_req_t *r, const char *field, const char *value);
esp_err_t httpd_resp_send(httpd_req_t *r, const char *buf, ssize_t len);
esp_err_t httpd_resp_send_chunk(httpd_req_t *r, const char *buf, ssize_t len);
int httpd_req_recv(httpd_req_t *r, char *buf, size_t len);
int httpd_req_to_sockfd(httpd_req_t *r);
int httpd_socket_send(httpd_handle_t hd, int sockfd, const char *buf, size_t len, int flags);
esp_err_t httpd_sess_trigger_close(httpd_handle_t handle, int sockfd);

size_t httpd_req_get_url_query_len(httpd_req_t *r);
esp_err_t httpd_req_get_url_query_str(httpd_req_t *r, char *buf, size_t len);
esp_err_t httpd_query_key_value(const char *qry, const char *key, char *val, size_t val_size);
size_t httpd_req_get_hdr_value_len(httpd_req_t *r, const char *field);
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *r, const char *field, char *val, size_t size);

/* Simulator only: the port httpd_start() binds (0 picks a free one). */
void sim_httpd_set_port(uint16_t port);
uint16_t sim_httpd_bound_port(void);
