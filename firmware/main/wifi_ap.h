/* SoftAP setup: static IP + DHCP server via the standard esp_netif stack. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t wifi_ap_init(void);
esp_err_t wifi_ap_set_station(const char *ssid, const char *password);

/* HTTPS certificate validation needs wall-clock time. These helpers report
 * and, when the station has an IP address, wait briefly for SNTP to finish. */
bool wifi_ap_clock_valid(void);
esp_err_t wifi_ap_wait_for_clock(uint32_t timeout_ms);

/* Copy the AP's IPv4 address (dotted quad) into buf. */
esp_err_t wifi_ap_get_ip(char *buf, size_t cap);

/* Number of stations currently associated with the SoftAP. */
uint32_t wifi_ap_get_client_count(void);

typedef struct {
    bool connected;
    char ssid[33];
    char ip[16];
    int8_t rssi;
    uint8_t disconnect_reason;
} wifi_station_status_t;

typedef struct {
    char ssid[33];
    int8_t rssi;
    bool open;
} wifi_scan_item_t;

void wifi_ap_get_station(wifi_station_status_t *out);
int wifi_ap_scan(wifi_scan_item_t *out, size_t cap);

#ifdef __cplusplus
}
#endif
