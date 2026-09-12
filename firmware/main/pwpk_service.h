#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "esp_http_server.h"

typedef esp_err_t (*pwpk_service_job_fn_t)(httpd_req_t *request, void *ctx);

esp_err_t pwpk_service_init(void);
esp_err_t pwpk_service_submit(httpd_req_t *request, pwpk_service_job_fn_t fn, void *ctx);
unsigned pwpk_service_queue_depth(void);

unsigned pwpk_service_queue_peak(void);
bool pwpk_service_busy(void);
