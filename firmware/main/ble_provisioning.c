#include "ble_provisioning.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_rom_crc.h"
#include "host/ble_hs.h"
#include "host/ble_hs_mbuf.h"
#include "host/ble_uuid.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "content_archive.h"
#include "oled_display.h"
#include "pack_store.h"
#include "wifi_ap.h"

static const char *TAG = "ble_setup";
static uint16_t s_status_handle;
static uint16_t s_response_handle;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint8_t s_own_addr_type;


static bool s_ble_up;             /* NimBLE is initialised right now */
static bool s_stopping;           /* the host is shutting down */

/* Existing provisioning UUIDs remain stable. Management adds control (0004),
 * pack data (0005), and response (0006) characteristics. */
static const ble_uuid128_t service_uuid = BLE_UUID128_INIT(
    0x01,0xc3,0xa0,0xf1,0x5e,0x8a,0x10,0x9d,0x8f,0x4c,0x45,0x7b,0x01,0x00,0x1e,0x7a);
static const ble_uuid128_t command_uuid = BLE_UUID128_INIT(
    0x01,0xc3,0xa0,0xf1,0x5e,0x8a,0x10,0x9d,0x8f,0x4c,0x45,0x7b,0x02,0x00,0x1e,0x7a);
static const ble_uuid128_t status_uuid = BLE_UUID128_INIT(
    0x01,0xc3,0xa0,0xf1,0x5e,0x8a,0x10,0x9d,0x8f,0x4c,0x45,0x7b,0x03,0x00,0x1e,0x7a);
static const ble_uuid128_t control_uuid = BLE_UUID128_INIT(
    0x01,0xc3,0xa0,0xf1,0x5e,0x8a,0x10,0x9d,0x8f,0x4c,0x45,0x7b,0x04,0x00,0x1e,0x7a);
static const ble_uuid128_t data_uuid = BLE_UUID128_INIT(
    0x01,0xc3,0xa0,0xf1,0x5e,0x8a,0x10,0x9d,0x8f,0x4c,0x45,0x7b,0x05,0x00,0x1e,0x7a);
static const ble_uuid128_t response_uuid = BLE_UUID128_INIT(
    0x01,0xc3,0xa0,0xf1,0x5e,0x8a,0x10,0x9d,0x8f,0x4c,0x45,0x7b,0x06,0x00,0x1e,0x7a);

enum {
    PW_BLE_INFO = 0x01,
    PW_BLE_PACK_AT = 0x02,
    PW_BLE_BEGIN = 0x10,
    PW_BLE_COMMIT = 0x11,
    PW_BLE_ABORT = 0x12,
    PW_BLE_DELETE = 0x20,
};

enum {
    PW_BLE_OK = 0,
    PW_BLE_BAD_COMMAND = 1,
    PW_BLE_BAD_ARGUMENT = 2,
    PW_BLE_NO_SPACE = 3,
    PW_BLE_IO_ERROR = 4,
    PW_BLE_INVALID_PACK = 5,
    PW_BLE_NOT_FOUND = 6,
    PW_BLE_BAD_OFFSET = 7,
};

typedef struct {
    FILE *fh;
    char temp_path[40];
    char name[PW_PACK_NAME_MAX + 1];
    uint32_t expected;
    uint32_t received;
    uint32_t expected_crc;
    uint32_t crc;
    size_t flash_used;
    size_t flash_total;
} ble_upload_t;

static ble_upload_t s_upload;
static uint8_t s_response[256];
static uint16_t s_response_len = 2;

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void wr32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static void set_response(uint8_t opcode, uint8_t status)
{
    s_response[0] = opcode;
    s_response[1] = status;
    s_response_len = 2;
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) ble_gatts_chr_updated(s_response_handle);
}

static void abort_upload(void)
{
    if (s_upload.fh != NULL) pack_store_abort_upload(s_upload.fh, s_upload.temp_path);
    memset(&s_upload, 0, sizeof s_upload);
    oled_clear_transfer();
}

static int append_wifi_status(struct os_mbuf *om)
{
    wifi_station_status_t status;
    wifi_ap_get_station(&status);
    /* AP credentials ride along so the phone can join the SoftAP for a fast
     * HTTP pack upload without asking the user for the network name. The AP
     * is open by default; password-protected APs fall back to BLE upload. */
    char ap_ip[16];
    if (wifi_ap_get_ip(ap_ip, sizeof ap_ip) != ESP_OK) strlcpy(ap_ip, "-", sizeof ap_ip);
    char body[240];
    int len = snprintf(body, sizeof body,
        "{\"connected\":%s,\"ssid\":\"%s\",\"ip\":\"%s\",\"reason\":%u,"
        "\"ap_ssid\":\"%s\",\"ap_ip\":\"%s\"}",
        status.connected ? "true" : "false", status.ssid, status.ip,
        (unsigned)status.disconnect_reason,
        CONFIG_POCKETWIKI_AP_SSID, ap_ip);
    return os_mbuf_append(om, body, (uint16_t)len) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int wifi_access(uint16_t conn_handle, uint16_t attr_handle,
                       struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    if (arg == (void *)1) {
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        if (len < 3 || len > 98) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        char body[99];
        if (ble_hs_mbuf_to_flat(ctxt->om, body, len, NULL) != 0) return BLE_ATT_ERR_UNLIKELY;
        body[len] = 0;
        char *newline = strchr(body, '\n');
        if (newline == NULL) return BLE_ATT_ERR_VALUE_NOT_ALLOWED;
        *newline = 0;
        esp_err_t err = wifi_ap_set_station(body, newline + 1);
        ESP_LOGI(TAG, "received credentials for %s: %s", body, esp_err_to_name(err));
        if (s_conn != BLE_HS_CONN_HANDLE_NONE) ble_gatts_notify(s_conn, s_status_handle);
        return err == ESP_OK ? 0 : BLE_ATT_ERR_UNLIKELY;
    }
    return append_wifi_status(ctxt->om);
}

static void respond_info(void)
{
    set_response(PW_BLE_INFO, PW_BLE_OK);
    size_t total = 0, used = 0;
    pack_store_usage(&total, &used);
    wr32(s_response + 2, (uint32_t)total);
    wr32(s_response + 6, (uint32_t)used);
    wr32(s_response + 10, (uint32_t)pack_store_installable_bytes());
    wr16(s_response + 14, (uint16_t)(pack_store_count() + 1));
    s_response_len = 16;
}

static void respond_pack_at(uint16_t index)
{
    set_response(PW_BLE_PACK_AT, PW_BLE_OK);
    const char *name = PW_BUILTIN_PACK_NAME;
    uint32_t articles = 0;
    uint32_t bytes = 0;
    pack_store_item_t *items = NULL;
    int count = pack_store_list_all(&items);
    if (index == 0) {
        if (pack_store_open_for_read(PW_BUILTIN_PACK_NAME) == ESP_OK) articles = ca_get()->count;
    } else if (count >= 0 && index <= (uint16_t)count) {
        name = items[index - 1].name;
        articles = items[index - 1].articles;
        bytes = items[index - 1].bytes;
    } else {
        free(items);
        set_response(PW_BLE_PACK_AT, PW_BLE_NOT_FOUND);
        return;
    }
    size_t name_len = strlen(name);
    wr16(s_response + 2, index);
    wr32(s_response + 4, articles);
    wr32(s_response + 8, bytes);
    s_response[12] = (uint8_t)name_len;
    memcpy(s_response + 13, name, name_len);
    s_response_len = (uint16_t)(13 + name_len);
    free(items);
}

static void begin_upload(const uint8_t *body, uint16_t len)
{
    set_response(PW_BLE_BEGIN, PW_BLE_BAD_ARGUMENT);
    if (len < 11 || s_upload.fh != NULL) return;
    uint32_t total = rd32(body + 1);
    uint32_t crc = rd32(body + 5);
    uint8_t name_len = body[9];
    if (name_len == 0 || name_len > PW_PACK_NAME_MAX || len != (uint16_t)(10 + name_len) ||
            total == 0 || total > CONFIG_POCKETWIKI_PACK_MAX_UPLOAD_KB * 1024u) return;
    char name[PW_PACK_NAME_MAX + 1];
    memcpy(name, body + 10, name_len);
    name[name_len] = 0;
    if (!pack_store_safe_name(name)) return;
    if (total > pack_store_installable_bytes()) {
        set_response(PW_BLE_BEGIN, PW_BLE_NO_SPACE);
        return;
    }
    abort_upload();
    if (pack_store_begin_upload(name, &s_upload.fh, s_upload.temp_path,
                                sizeof s_upload.temp_path) != ESP_OK) {
        set_response(PW_BLE_BEGIN, PW_BLE_IO_ERROR);
        return;
    }
    strlcpy(s_upload.name, name, sizeof s_upload.name);
    s_upload.expected = total;
    s_upload.expected_crc = crc;
    pack_store_usage(&s_upload.flash_total, &s_upload.flash_used);
    oled_show_transfer(s_upload.name, 0, total, s_upload.flash_used, s_upload.flash_total);
    set_response(PW_BLE_BEGIN, PW_BLE_OK);
    wr32(s_response + 2, 0);
    s_response_len = 6;
    ESP_LOGI(TAG, "BLE upload begin: %s (%lu bytes)", name, (unsigned long)total);
}

static void commit_upload(void)
{
    set_response(PW_BLE_COMMIT, PW_BLE_BAD_ARGUMENT);
    if (s_upload.fh == NULL || s_upload.received != s_upload.expected ||
            s_upload.crc != s_upload.expected_crc) {
        abort_upload();
        set_response(PW_BLE_COMMIT, PW_BLE_INVALID_PACK);
        return;
    }
    FILE *fh = s_upload.fh;
    s_upload.fh = NULL;
    uint32_t articles = 0;
    esp_err_t err = pack_store_finish_upload(fh, s_upload.temp_path, s_upload.name, &articles);
    if (err != ESP_OK) {
        memset(&s_upload, 0, sizeof s_upload);
        oled_clear_transfer();
        set_response(PW_BLE_COMMIT, PW_BLE_INVALID_PACK);
        return;
    }
    ESP_LOGI(TAG, "BLE upload installed: %s (%lu articles)", s_upload.name,
             (unsigned long)articles);
    memset(&s_upload, 0, sizeof s_upload);
    oled_clear_transfer();
    set_response(PW_BLE_COMMIT, PW_BLE_OK);
    wr32(s_response + 2, articles);
    s_response_len = 6;
}

static void delete_pack(const uint8_t *body, uint16_t len)
{
    set_response(PW_BLE_DELETE, PW_BLE_BAD_ARGUMENT);
    if (len < 3 || body[1] == 0 || body[1] > PW_PACK_NAME_MAX || len != (uint16_t)(2 + body[1])) return;
    char name[PW_PACK_NAME_MAX + 1];
    memcpy(name, body + 2, body[1]);
    name[body[1]] = 0;
    if (!pack_store_safe_name(name)) return;
    esp_err_t err = pack_store_remove(name);
    set_response(PW_BLE_DELETE, err == ESP_OK ? PW_BLE_OK : PW_BLE_NOT_FOUND);
}

static int control_access(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len == 0 || len > sizeof s_response) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    uint8_t body[256];
    if (ble_hs_mbuf_to_flat(ctxt->om, body, len, NULL) != 0) return BLE_ATT_ERR_UNLIKELY;
    switch (body[0]) {
        case PW_BLE_INFO: respond_info(); break;
        case PW_BLE_PACK_AT:
            if (len == 3) respond_pack_at(rd16(body + 1));
            else set_response(PW_BLE_PACK_AT, PW_BLE_BAD_ARGUMENT);
            break;
        case PW_BLE_BEGIN: begin_upload(body, len); break;
        case PW_BLE_COMMIT: commit_upload(); break;
        case PW_BLE_ABORT:
            abort_upload();
            set_response(PW_BLE_ABORT, PW_BLE_OK);
            break;
        case PW_BLE_DELETE: delete_pack(body, len); break;
        default: set_response(body[0], PW_BLE_BAD_COMMAND); break;
    }
    return 0;
}

static int data_access(uint16_t conn_handle, uint16_t attr_handle,
                       struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (s_upload.fh == NULL || len <= 4 || len > 256) return BLE_ATT_ERR_VALUE_NOT_ALLOWED;
    uint8_t body[256];
    if (ble_hs_mbuf_to_flat(ctxt->om, body, len, NULL) != 0) return BLE_ATT_ERR_UNLIKELY;
    uint32_t offset = rd32(body);
    size_t count = len - 4;
    if (offset != s_upload.received || count > s_upload.expected - s_upload.received) {
        set_response(PW_BLE_BEGIN, PW_BLE_BAD_OFFSET);
        return BLE_ATT_ERR_VALUE_NOT_ALLOWED;
    }
    if (fwrite(body + 4, 1, count, s_upload.fh) != count) {
        abort_upload();
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    s_upload.crc = esp_rom_crc32_le(s_upload.crc, body + 4, (uint32_t)count);
    s_upload.received += (uint32_t)count;
    oled_show_transfer(s_upload.name, s_upload.received, s_upload.expected,
                       s_upload.flash_used + s_upload.received, s_upload.flash_total);
    return 0;
}

static int response_access(uint16_t conn_handle, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;
    return os_mbuf_append(ctxt->om, s_response, s_response_len) == 0
        ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static const struct ble_gatt_svc_def services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &service_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = &command_uuid.u, .access_cb = wifi_access, .arg = (void *)1,
              .flags = BLE_GATT_CHR_F_WRITE },
            { .uuid = &status_uuid.u, .access_cb = wifi_access,
              .val_handle = &s_status_handle,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY },
            { .uuid = &control_uuid.u, .access_cb = control_access,
              .flags = BLE_GATT_CHR_F_WRITE },
            { .uuid = &data_uuid.u, .access_cb = data_access,
              .flags = BLE_GATT_CHR_F_WRITE },
            { .uuid = &response_uuid.u, .access_cb = response_access,
              .val_handle = &s_response_handle,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY },
            { 0 }
        }
    },
    { 0 }
};

static void advertise(void);

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    if (event->type == BLE_GAP_EVENT_CONNECT) {
        if (event->connect.status == 0) s_conn = event->connect.conn_handle;
        else advertise();
    } else if (event->type == BLE_GAP_EVENT_DISCONNECT) {
        abort_upload();
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        advertise();
    } else if (event->type == BLE_GAP_EVENT_ADV_COMPLETE) {
        advertise();
    }
    return 0;
}

static void advertise(void)
{
    if (s_stopping) return;       /* the host is going away, not failing */
    struct ble_hs_adv_fields fields = { 0 };
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = (ble_uuid128_t *)&service_uuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "could not set BLE advertisement: %d", rc);
        return;
    }
    const char *name = ble_svc_gap_device_name();
    struct ble_hs_adv_fields response = { 0 };
    response.name = (const uint8_t *)name;
    response.name_len = strlen(name);
    response.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&response);
    if (rc != 0) {
        ESP_LOGE(TAG, "could not set BLE scan response: %d", rc);
        return;
    }
    struct ble_gap_adv_params params = { 0 };
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
    if (rc != 0) ESP_LOGE(TAG, "could not start BLE advertisement: %d", rc);
}

static void on_sync(void)
{
    if (ble_hs_id_infer_auto(0, &s_own_addr_type) == 0) advertise();
    else ESP_LOGE(TAG, "could not infer BLE address type");
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* Bring the NimBLE host and the GATT service up. */
static esp_err_t bring_up(void)
{
#if CONFIG_POCKETWIKI_BLE_ENABLE
    if (s_ble_up) return ESP_OK;
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) return err;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set("PocketWiki Setup");
    if (ble_gatts_count_cfg(services) != 0 || ble_gatts_add_svcs(services) != 0) return ESP_FAIL;
    ble_hs_cfg.sync_cb = on_sync;
    nimble_port_freertos_init(host_task);
    s_ble_up = true;
    ESP_LOGI(TAG, "BLE provisioning and pack management ready");
#endif
    return ESP_OK;
}

/* Stop it and give its memory back. */
static esp_err_t bring_down(void)
{
#if CONFIG_POCKETWIKI_BLE_ENABLE
    if (!s_ble_up) return ESP_OK;
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) return ESP_ERR_INVALID_STATE;
    /* The host can re-sync while it shuts down, and on_sync() would then try
     * to advertise against a stack that is going away. */
    ble_hs_cfg.sync_cb = NULL;
    s_stopping = true;
    int rc = nimble_port_stop();
    if (rc != 0) {
        s_stopping = false;
        return ESP_FAIL;
    }
    esp_err_t err = nimble_port_deinit();
    s_stopping = false;
    if (err != ESP_OK) return err;
    s_conn = BLE_HS_CONN_HANDLE_NONE;
    s_ble_up = false;
    ESP_LOGI(TAG, "BLE stopped; heap=%lu", (unsigned long)esp_get_free_heap_size());
#endif
    return ESP_OK;
}

esp_err_t ble_provisioning_init(void)
{
    return bring_up();
}

esp_err_t ble_provisioning_suspend(void)
{
    /* Used around station-mode HTTPS transfers, which need the memory more
     * than an idle radio does. A connected client is rejected. */
    return bring_down();
}

esp_err_t ble_provisioning_resume(void)
{
    return bring_up();
}

bool ble_provisioning_active(void)
{
    return s_ble_up;
}
