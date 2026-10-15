/* The radio, Bluetooth and display modules, reduced to the state the web
 * server reads: an access point that is always up, an optional station uplink
 * (on by default so catalogue installs work against a local server), a
 * Bluetooth link that can always be paused, and an OLED that logs transfers. */
#include <stdio.h>
#include <string.h>

#include "ble_provisioning.h"
#include "esp_log.h"
#include "oled_display.h"
#include "sim.h"
#include "wifi_ap.h"

static const char *TAG = "sim-dev";
static char s_sta_ssid[33] = "SimUplink";

esp_err_t wifi_ap_init(void) { return ESP_OK; }

esp_err_t wifi_ap_set_station(const char *ssid, const char *password)
{
    (void)password;
    if (ssid == NULL || ssid[0] == '\0' || strlen(ssid) > 32) return ESP_ERR_INVALID_ARG;
    snprintf(s_sta_ssid, sizeof s_sta_ssid, "%s", ssid);
    g_sim.sta_connected = true;
    ESP_LOGI(TAG, "station set to \"%s\"", ssid);
    return ESP_OK;
}

bool wifi_ap_clock_valid(void) { return g_sim.sta_connected; }

esp_err_t wifi_ap_wait_for_clock(uint32_t timeout_ms)
{
    (void)timeout_ms;
    return g_sim.sta_connected ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t wifi_ap_get_ip(char *buf, size_t cap)
{
    snprintf(buf, cap, "%s", CONFIG_POCKETWIKI_AP_IP);
    return ESP_OK;
}

uint32_t wifi_ap_get_client_count(void) { return 1; }

void wifi_ap_get_station(wifi_station_status_t *out)
{
    memset(out, 0, sizeof *out);
    out->connected = g_sim.sta_connected;
    if (g_sim.sta_connected) {
        snprintf(out->ssid, sizeof out->ssid, "%s", s_sta_ssid);
        snprintf(out->ip, sizeof out->ip, "10.0.0.42");
        out->rssi = -55;
    } else {
        snprintf(out->ip, sizeof out->ip, "-");
    }
}

int wifi_ap_scan(wifi_scan_item_t *out, size_t cap)
{
    static const wifi_scan_item_t nets[] = {
        { "SchoolNet", -48, false },
        { "Library \"Guest\"", -67, true },
        { "Staff-2.4G", -80, false },
    };
    size_t n = sizeof nets / sizeof nets[0];
    if (n > cap) n = cap;
    memcpy(out, nets, n * sizeof *out);
    return (int)n;
}

esp_err_t ble_provisioning_init(void) { return ESP_OK; }
esp_err_t ble_provisioning_suspend(void) { return ESP_OK; }
esp_err_t ble_provisioning_resume(void) { return ESP_OK; }
bool ble_provisioning_active(void) { return true; }

static bool s_transfer;

esp_err_t oled_init(void) { return ESP_OK; }
void oled_show_status(const char *ssid, const char *ip, uint32_t article_count,
                      uint32_t pack_count, size_t flash_used, size_t flash_total,
                      bool online, unsigned cpu_pct, uint32_t queued_packs,
                      uint8_t disconnect_reason)
{
    (void)ssid; (void)ip; (void)article_count; (void)pack_count; (void)flash_used;
    (void)flash_total; (void)online; (void)cpu_pct; (void)queued_packs; (void)disconnect_reason;
}

void oled_show_transfer(const char *pack, size_t received, size_t total,
                        size_t flash_used, size_t flash_total)
{
    s_transfer = true;
    ESP_LOGD("oled", "transfer %s %zu/%zu (flash %zu/%zu)", pack, received, total,
             flash_used, flash_total);
}

bool oled_transfer_active(void) { return s_transfer; }
void oled_clear_transfer(void) { s_transfer = false; }
bool oled_show_qr(const char *payload) { (void)payload; return true; }
bool oled_qr_active(void) { return false; }
void oled_clear_qr(void) {}
void oled_show_boot(const char *line) { ESP_LOGI("oled", "boot: %s", line); }
