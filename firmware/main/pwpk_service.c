#include "pwpk_service.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define PWPK_QUEUE_LENGTH 2
#define PWPK_WORKER_STACK 16384

typedef struct { httpd_req_t *request; pwpk_service_job_fn_t fn; void *ctx; } pwpk_job_t;
static QueueHandle_t s_queue;
static volatile bool s_active;
static unsigned s_peak;

static void pwpk_worker(void *arg)
{
    (void)arg;
    pwpk_job_t job;
    for (;;) {
        if (xQueueReceive(s_queue, &job, portMAX_DELAY) != pdTRUE) continue;
        s_active = true;
        (void)job.fn(job.request, job.ctx);
        s_active = false;
        httpd_req_async_handler_complete(job.request);
    }
}

esp_err_t pwpk_service_init(void)
{
    if (s_queue != NULL) return ESP_OK;
    s_queue = xQueueCreate(PWPK_QUEUE_LENGTH, sizeof(pwpk_job_t));
    if (s_queue == NULL) return ESP_ERR_NO_MEM;
    if (xTaskCreate(pwpk_worker, "pwpk_decode", PWPK_WORKER_STACK, NULL,
                    tskIDLE_PRIORITY + 2, NULL) != pdPASS) {
        vQueueDelete(s_queue); s_queue = NULL; return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t pwpk_service_submit(httpd_req_t *request, pwpk_service_job_fn_t fn, void *ctx)
{
    if (request == NULL || fn == NULL || s_queue == NULL) return ESP_ERR_INVALID_STATE;
    httpd_req_t *async_request = NULL;
    esp_err_t err = httpd_req_async_handler_begin(request, &async_request);
    if (err != ESP_OK) return err;
    pwpk_job_t job = { .request = async_request, .fn = fn, .ctx = ctx };
    if (xQueueSend(s_queue, &job, 0) != pdTRUE) {
        httpd_resp_set_status(async_request, "503 Service Unavailable");
        httpd_resp_set_hdr(async_request, "Retry-After", "1");
        httpd_resp_sendstr(async_request, "Decoder queue full");
        httpd_req_async_handler_complete(async_request); return ESP_ERR_TIMEOUT;
    }
    unsigned depth = (unsigned)uxQueueMessagesWaiting(s_queue);
    if (depth > s_peak) s_peak = depth;
    return ESP_OK;
}

unsigned pwpk_service_queue_depth(void)
{
    return s_queue == NULL ? 0 : (unsigned)uxQueueMessagesWaiting(s_queue);
}

unsigned pwpk_service_queue_peak(void) { return s_peak; }
bool pwpk_service_busy(void) { return s_active || pwpk_service_queue_depth() != 0; }
