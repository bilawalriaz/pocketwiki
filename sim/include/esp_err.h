#pragma once
#include "sdkconfig.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef int esp_err_t;

#define ESP_OK                    0
#define ESP_FAIL                  -1
#define ESP_ERR_NO_MEM            0x101
#define ESP_ERR_INVALID_ARG       0x102
#define ESP_ERR_INVALID_STATE     0x103
#define ESP_ERR_INVALID_SIZE      0x104
#define ESP_ERR_NOT_FOUND         0x105
#define ESP_ERR_NOT_SUPPORTED     0x106
#define ESP_ERR_TIMEOUT           0x107
#define ESP_ERR_INVALID_RESPONSE  0x108
#define ESP_ERR_INVALID_CRC       0x109
#define ESP_ERR_INVALID_VERSION   0x10A
#define ESP_ERR_HTTPD_BASE        0xb000
#define ESP_ERR_HTTPD_RESULT_TRUNC (ESP_ERR_HTTPD_BASE + 3)
#define ESP_ERR_HTTPD_INVALID_REQ  (ESP_ERR_HTTPD_BASE + 6)
#define ESP_ERR_HTTP_BASE         0x7000
#define ESP_ERR_HTTP_EAGAIN       (ESP_ERR_HTTP_BASE + 7)
#define ESP_ERR_NVS_NOT_FOUND     0x1102

const char *esp_err_to_name(esp_err_t code);
