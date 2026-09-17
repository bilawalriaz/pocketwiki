#include "wifi_ap.h"

#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "nvs.h"

#include "oled_display.h"

static const char *TAG = "wifi";
static esp_netif_t *s_ap_netif;
static esp_netif_t *s_sta_netif;
static bool s_connected;
static bool s_reconfiguring; /* suppress auto-reconnect while applying a manual config */
static uint8_t s_reason;
static char s_ssid[33];
static bool s_sntp_started;
static esp_timer_handle_t s_reconnect_timer;
static esp_timer_handle_t s_uplink_timer;
static uint32_t s_reconnect_attempts;
static bool s_sta_suspended;
static uint8_t s_weak_samples;

/* Station retry pacing. The access point and the station share one radio, so
 * every esp_wifi_connect() attempt scans channels while the AP is off the air
 * (see docs/HARDWARE.md). Retrying instantly from the disconnect event turns a
 * stale or unreachable saved network into a continuous scan loop that makes
 * the device's own access point unusable; each consecutive failure therefore
 * waits longer, up to a cap that still rejoins a returning network quickly. */
#define PW_RECONNECT_FIRST_MS 1000u
#define PW_RECONNECT_MAX_MS   30000u

static uint32_t schedule_station_reconnect(void);

/* How long the uplink may stay below CONFIG_POCKETWIKI_STA_MIN_RSSI before it
 * is dropped. The host samples once a second; a few seconds of grace keeps a
 * transient dip from tearing the uplink down, while still restoring the access
 * point within seconds of a hopeless one appearing. */
#define PW_STA_WEAK_SAMPLES 5

#define PW_MIN_VALID_EPOCH ((time_t)1735689600) /* 2025-01-01T00:00:00Z */

/* Device-side HTTPS (pack downloads, catalogue sync) validates server
 * certificates against the wall clock. The C3 has no battery-backed RTC,
 * so sync once the station has internet; without this every TLS fetch
 * fails and the dashboard can only install packs over BLE/file upload. */
static void sntp_synced(struct timeval *tv)
{
    ESP_LOGI(TAG, "time synced (%lld)", (long long)tv->tv_sec);
}

static void sntp_start(void)
{
    if (s_sntp_started) {
        /* The first network may have had no DNS/internet. Restart the client
         * after every new station IP so switching networks can recover. */
        esp_err_t err = esp_netif_sntp_start();
        if (err != ESP_OK) ESP_LOGW(TAG, "sntp restart failed: %s", esp_err_to_name(err));
        return;
    }
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    cfg.sync_cb = sntp_synced;
    if (esp_netif_sntp_init(&cfg) == ESP_OK) {
        s_sntp_started = true;
    } else {
        ESP_LOGW(TAG, "sntp init failed; device-side downloads will fail");
    }
}

bool wifi_ap_clock_valid(void)
{
    return time(NULL) >= PW_MIN_VALID_EPOCH;
}

esp_err_t wifi_ap_wait_for_clock(uint32_t timeout_ms)
{
    if (wifi_ap_clock_valid()) return ESP_OK;
    if (!s_connected) return ESP_ERR_INVALID_STATE;
    sntp_start();
    if (wifi_ap_clock_valid()) return ESP_OK;
    TickType_t ticks = pdMS_TO_TICKS(timeout_ms);
    if (ticks == 0) ticks = 1;
    esp_err_t err = esp_netif_sntp_sync_wait(ticks);
    return err == ESP_OK && wifi_ap_clock_valid() ? ESP_OK : ESP_ERR_TIMEOUT;
}


static void reconnect_timer_cb(void *arg)
{
    (void)arg;
    if (!s_ssid[0] || s_connected || s_reconfiguring) return;
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "station reconnect could not start (%s)", esp_err_to_name(err));
        schedule_station_reconnect();
    }
}

/* Queue the next station attempt and return the delay in milliseconds. */
static uint32_t schedule_station_reconnect(void)
{
    if (s_reconnect_timer == NULL || s_sta_suspended) return 0;
    uint32_t delay = PW_RECONNECT_FIRST_MS;
    for (uint32_t i = 0; i < s_reconnect_attempts && delay < PW_RECONNECT_MAX_MS; i++) delay *= 2;
    if (delay > PW_RECONNECT_MAX_MS) delay = PW_RECONNECT_MAX_MS;
    s_reconnect_attempts++;
    if (esp_timer_is_active(s_reconnect_timer)) esp_timer_stop(s_reconnect_timer);
    if (esp_timer_start_once(s_reconnect_timer, (uint64_t)delay * 1000) != ESP_OK) return 0;
    return delay;
}

static void cancel_station_reconnect(void)
{
    s_reconnect_attempts = 0;
    if (s_reconnect_timer != NULL && esp_timer_is_active(s_reconnect_timer)) {
        esp_timer_stop(s_reconnect_timer);
    }
}

/* Drop an uplink that cannot share the radio with the access point.
 *
 * The access point is the product: it is how the reader is reached, while the
 * station is only used on demand for pack downloads. A station that is
 * connected far below usable signal spends the shared radio on retries and
 * produces exactly the reported failure - the page loads once or twice and
 * then nothing answers. Credentials are kept so a better network only needs to
 * be re-selected at /manage. */
static void uplink_timer_cb(void *arg)
{
    (void)arg;
    if (!s_connected || s_sta_suspended || oled_transfer_active()) {
        s_weak_samples = 0;   /* an in-flight pack transfer owns the uplink */
        return;
    }
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return;
    if (ap.rssi > CONFIG_POCKETWIKI_STA_MIN_RSSI) {
        s_weak_samples = 0;
        return;
    }
    if (++s_weak_samples < PW_STA_WEAK_SAMPLES) return;
    s_weak_samples = 0;
    s_sta_suspended = true;
    ESP_LOGW(TAG, "station %s at %d dBm (limit %d dBm) cannot share the radio with the "
                  "access point: uplink dropped, Wi-Fi details kept - reconnect from /manage",
             s_ssid, ap.rssi, CONFIG_POCKETWIKI_STA_MIN_RSSI);
    esp_wifi_disconnect();
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *event = data;
        s_connected = false;
        s_reason = event->reason;
        /* On a flapping link the driver can be mid-connect when the user
         * reconfigures; the manual path owns the radio during that window. */
        if (!s_ssid[0]) return;
        if (s_sta_suspended) {
            ESP_LOGI(TAG, "station %s dropped: access point keeps the radio", s_ssid);
            return;
        }
        if (s_reconfiguring) {
            ESP_LOGI(TAG, "station %s disconnected (reason %u), applying new configuration",
                     s_ssid, (unsigned)event->reason);
            return;
        }
        uint32_t delay = schedule_station_reconnect();
        ESP_LOGW(TAG, "station %s disconnected (reason %u), retry %u in %lu ms",
                 s_ssid, (unsigned)event->reason, (unsigned)s_reconnect_attempts,
                 (unsigned long)delay);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_connected = true;
        s_reason = 0;
        cancel_station_reconnect();
        ESP_LOGI(TAG, "station connected to %s", s_ssid);
        sntp_start();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        const wifi_event_ap_staconnected_t *event = data;
        ESP_LOGI(TAG, "AP client joined " MACSTR " aid=%u",
                 MAC2STR(event->mac), (unsigned)event->aid);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        const wifi_event_ap_stadisconnected_t *event = data;
        ESP_LOGI(TAG, "AP client left " MACSTR " aid=%u reason=%u",
                 MAC2STR(event->mac), (unsigned)event->aid,
                 (unsigned)event->reason);
    } else if (base == IP_EVENT && id == IP_EVENT_ASSIGNED_IP_TO_CLIENT) {
        const ip_event_assigned_ip_to_client_t *event = data;
        ESP_LOGI(TAG, "AP DHCP assigned " IPSTR " to " MACSTR " hostname=%s",
                 IP2STR(&event->ip), MAC2STR(event->mac), event->hostname);
    }
}

static void load_credentials(char *password, size_t pass_cap)
{
    nvs_handle_t nvs;
    size_t ssid_cap = sizeof s_ssid;
    if (nvs_open("pocketwiki", NVS_READONLY, &nvs) != ESP_OK) return;
    if (nvs_get_str(nvs, "wifi_ssid", s_ssid, &ssid_cap) != ESP_OK) s_ssid[0] = 0;
    if (nvs_get_str(nvs, "wifi_pass", password, &pass_cap) != ESP_OK) password[0] = 0;
    nvs_close(nvs);
}

static esp_err_t apply_station(const char *ssid, const char *password)
{
    wifi_config_t cfg = { 0 };
    s_sta_suspended = false;
    s_weak_samples = 0;
    cancel_station_reconnect();
    strlcpy((char *)cfg.sta.ssid, ssid, sizeof cfg.sta.ssid);
    strlcpy((char *)cfg.sta.password, password, sizeof cfg.sta.password);
    cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    cfg.sta.pmf_cfg.capable = true;
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &cfg), TAG, "station config");
    strlcpy(s_ssid, ssid, sizeof s_ssid);
    s_connected = false;
    return ssid[0] ? esp_wifi_connect() : ESP_OK;
}

esp_err_t wifi_ap_init(void)
{
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "esp_netif_init");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");
    s_ap_netif = esp_netif_create_default_wifi_ap();
    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (s_ap_netif == NULL || s_sta_netif == NULL) return ESP_FAIL;
    esp_netif_set_hostname(s_sta_netif, CONFIG_POCKETWIKI_HOSTNAME);
    const esp_timer_create_args_t reconnect_timer_args = {
        .callback = reconnect_timer_cb,
        .name = "pw_reconnect",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&reconnect_timer_args, &s_reconnect_timer),
                        TAG, "reconnect timer");
    const esp_timer_create_args_t uplink_timer_args = {
        .callback = uplink_timer_cb,
        .name = "pw_uplink",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&uplink_timer_args, &s_uplink_timer),
                        TAG, "uplink timer");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(s_uplink_timer, 1000000),
                        TAG, "uplink timer run");
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "esp_wifi_init");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                                   wifi_event, NULL), TAG, "wifi handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                   wifi_event, NULL), TAG, "ip handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_ASSIGNED_IP_TO_CLIENT,
                                                   wifi_event, NULL), TAG, "ap ip handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED,
                                                   wifi_event, NULL), TAG, "ap connect handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STADISCONNECTED,
                                                   wifi_event, NULL), TAG, "ap disconnect handler");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "wifi APSTA mode");

    wifi_config_t ap = { 0 };
    strlcpy((char *)ap.ap.ssid, CONFIG_POCKETWIKI_AP_SSID, sizeof ap.ap.ssid);
    ap.ap.ssid_len = strlen(CONFIG_POCKETWIKI_AP_SSID);
    if (CONFIG_POCKETWIKI_AP_PASSWORD[0]) {
        strlcpy((char *)ap.ap.password, CONFIG_POCKETWIKI_AP_PASSWORD, sizeof ap.ap.password);
        ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else ap.ap.authmode = WIFI_AUTH_OPEN;
    ap.ap.pmf_cfg.required = false;
    ap.ap.channel = CONFIG_POCKETWIKI_AP_CHANNEL;
    ap.ap.max_connection = CONFIG_POCKETWIKI_AP_MAX_CONNECTIONS;
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap), TAG, "AP config");

    esp_netif_ip_info_t ip = { 0 };
    if (esp_netif_str_to_ip4(CONFIG_POCKETWIKI_AP_IP, &ip.ip) != ESP_OK ||
        esp_netif_str_to_ip4(CONFIG_POCKETWIKI_AP_NETMASK, &ip.netmask) != ESP_OK ||
        esp_netif_str_to_ip4(CONFIG_POCKETWIKI_AP_IP, &ip.gw) != ESP_OK) return ESP_ERR_INVALID_ARG;
    esp_netif_dhcps_stop(s_ap_netif);
    ESP_RETURN_ON_ERROR(esp_netif_set_ip_info(s_ap_netif, &ip), TAG, "AP IP");
    ESP_RETURN_ON_ERROR(esp_netif_dhcps_start(s_ap_netif), TAG, "DHCP");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");

#if CONFIG_IDF_TARGET_ESP32C3
    /* Single-core: keep some headroom for the HTTP server, but 19.5 dBm
     * (up from 12 dBm) keeps the station uplink usable on marginal links —
     * pack downloads need reliable TCP ACKs, not just close-range AP range. */
    esp_wifi_set_max_tx_power(78);  /* units of 0.25 dBm → 19.5 dBm */
#endif
    /* No modem sleep: with the radio sleeping between beacons, long
     * device-side pack downloads stall and time out. The pack store is the
     * main workload; power save is not worth the reliability cost. */
    esp_wifi_set_ps(WIFI_PS_NONE);

    char password[65] = "";
    load_credentials(password, sizeof password);
    if (s_ssid[0]) apply_station(s_ssid, password);
    ESP_LOGI(TAG, "SoftAP %s at %s; station=%s", CONFIG_POCKETWIKI_AP_SSID,
             CONFIG_POCKETWIKI_AP_IP, s_ssid[0] ? s_ssid : "not configured");
    return ESP_OK;
}

esp_err_t wifi_ap_set_station(const char *ssid, const char *password)
{
    if (ssid == NULL || strlen(ssid) > 32 || password == NULL || strlen(password) > 64) return ESP_ERR_INVALID_ARG;
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open("pocketwiki", NVS_READWRITE, &nvs), TAG, "open nvs");
    esp_err_t err = nvs_set_str(nvs, "wifi_ssid", ssid);
    if (err == ESP_OK) err = nvs_set_str(nvs, "wifi_pass", password);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err != ESP_OK) return err;
    s_reconfiguring = true;
    esp_wifi_disconnect();
    /* esp_wifi_set_config() rejects ESP_ERR_WIFI_STATE while the STA is still
     * connecting (flapping link: the event handler's reconnect is in flight).
     * Wait out the transition and retry; the driver settles in < 100 ms. */
    err = apply_station(ssid, password);
    for (int attempt = 0; err == ESP_ERR_WIFI_STATE && attempt < 20; attempt++) {
        vTaskDelay(pdMS_TO_TICKS(100));
        err = apply_station(ssid, password);
    }
    s_reconfiguring = false;
    return err;
}

esp_err_t wifi_ap_get_ip(char *buf, size_t cap)
{
    if (s_ap_netif == NULL || cap < 16) return ESP_ERR_INVALID_STATE;
    esp_netif_ip_info_t info;
    ESP_RETURN_ON_ERROR(esp_netif_get_ip_info(s_ap_netif, &info), TAG, "AP IP");
    snprintf(buf, cap, IPSTR, IP2STR(&info.ip));
    return ESP_OK;
}

void wifi_ap_get_station(wifi_station_status_t *out)
{
    memset(out, 0, sizeof *out);
    out->connected = s_connected;
    out->uplink_suspended = s_sta_suspended;
    out->disconnect_reason = s_reason;
    strlcpy(out->ssid, s_ssid, sizeof out->ssid);
    if (s_connected) {
        esp_netif_ip_info_t info;
        if (esp_netif_get_ip_info(s_sta_netif, &info) == ESP_OK) snprintf(out->ip, sizeof out->ip, IPSTR, IP2STR(&info.ip));
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) out->rssi = ap.rssi;
    }
}

uint32_t wifi_ap_get_client_count(void)
{
    wifi_sta_list_t stations = { 0 };
    return esp_wifi_ap_get_sta_list(&stations) == ESP_OK ? stations.num : 0;
}

int wifi_ap_scan(wifi_scan_item_t *out, size_t cap)
{
    wifi_scan_config_t scan = { .show_hidden = false };
    if (esp_wifi_scan_start(&scan, true) != ESP_OK) return -1;
    uint16_t count = 0;
    if (esp_wifi_scan_get_ap_num(&count) != ESP_OK) return -1;
    if (count > cap) count = (uint16_t)cap;
    wifi_ap_record_t records[16];
    if (count > 16) count = 16;
    if (esp_wifi_scan_get_ap_records(&count, records) != ESP_OK) return -1;
    for (uint16_t i = 0; i < count; i++) {
        strlcpy(out[i].ssid, (const char *)records[i].ssid, sizeof out[i].ssid);
        out[i].rssi = records[i].rssi;
        out[i].open = records[i].authmode == WIFI_AUTH_OPEN;
    }
    return count;
}
