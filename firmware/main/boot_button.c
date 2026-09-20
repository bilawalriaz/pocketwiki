#include "boot_button.h"

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "oled_display.h"

static const char *TAG = "button";

#define PW_BUTTON_POLL_MS 20
/* Consecutive equal samples required before a level change is accepted. */
#define PW_BUTTON_STABLE_SAMPLES 3
#define PW_BUTTON_TASK_STACK 4096

typedef enum {
    SCREEN_STATUS = 0,
    SCREEN_JOIN,
    SCREEN_READER,
} screen_t;

static screen_t s_screen = SCREEN_STATUS;

static void append_str(char *out, size_t cap, const char *value)
{
    size_t len = strlen(out);
    if (len >= cap) return;
    strlcpy(out + len, value, cap - len);
}

/* Escape the separators defined for the "WIFI:" payload so an SSID or password
 * containing them cannot break the field boundaries. */
static void append_escaped(char *out, size_t cap, const char *value)
{
    size_t len = strlen(out);
    for (const char *p = value; *p != '\0' && len + 2 < cap; p++) {
        if (*p == '\\' || *p == ';' || *p == ',' || *p == ':' || *p == '"') {
            out[len++] = '\\';
        }
        out[len++] = *p;
    }
    out[len] = '\0';
}

static void build_join_payload(char *out, size_t cap)
{
    bool secured = CONFIG_POCKETWIKI_AP_PASSWORD[0] != '\0';
    snprintf(out, cap, "WIFI:T:%s;S:", secured ? "WPA" : "nopass");
    append_escaped(out, cap, CONFIG_POCKETWIKI_AP_SSID);
    if (secured) {
        append_str(out, cap, ";P:");
        append_escaped(out, cap, CONFIG_POCKETWIKI_AP_PASSWORD);
    }
    append_str(out, cap, ";;");
}

static void build_reader_payload(char *out, size_t cap)
{
    snprintf(out, cap, "http://%s/", CONFIG_POCKETWIKI_AP_IP);
}

static bool show_screen(screen_t screen)
{
    char payload[256];
    switch (screen) {
    case SCREEN_JOIN:
        build_join_payload(payload, sizeof payload);
        break;
    case SCREEN_READER:
        build_reader_payload(payload, sizeof payload);
        break;
    default:
        oled_clear_qr();
        return true;
    }
    return oled_show_qr(payload);
}

static screen_t advance(screen_t screen)
{
    switch (screen) {
    case SCREEN_STATUS: return SCREEN_JOIN;
    case SCREEN_JOIN: return SCREEN_READER;
    default: return SCREEN_STATUS;
    }
}

static void handle_press(void)
{
    if (oled_transfer_active()) {
        ESP_LOGI(TAG, "press ignored: pack transfer owns the display");
        return;   /* a pack transfer owns the screen */
    }
    /* A payload too long for a scannable symbol (an AP password longer than
     * the symbol can hold) is skipped rather than shown as a bad code. */
    for (screen_t screen = advance(s_screen); screen != SCREEN_STATUS; screen = advance(screen)) {
        if (show_screen(screen)) {
            s_screen = screen;
            ESP_LOGI(TAG, "press -> %s", screen == SCREEN_JOIN ? "Wi-Fi join QR" : "reader QR");
            return;
        }
    }
    show_screen(SCREEN_STATUS);
    s_screen = SCREEN_STATUS;
    ESP_LOGI(TAG, "press -> status");
}

static void button_task(void *arg)
{
    (void)arg;
    const gpio_num_t pin = (gpio_num_t)CONFIG_POCKETWIKI_BUTTON_GPIO;
    int stable = gpio_get_level(pin);
    int candidate = stable;
    int count = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(PW_BUTTON_POLL_MS));
        int level = gpio_get_level(pin);
        if (level != candidate) {
            candidate = level;
            count = 0;
        } else if (count < PW_BUTTON_STABLE_SAMPLES) {
            count++;
        }
        if (count == PW_BUTTON_STABLE_SAMPLES && candidate != stable) {
            stable = candidate;
            if (stable == 0) handle_press();   /* the BOOT button is active low */
        }
    }
}

esp_err_t boot_button_init(void)
{
#if !CONFIG_POCKETWIKI_BUTTON_ENABLE
    ESP_LOGI(TAG, "boot button disabled by configuration");
    return ESP_ERR_NOT_SUPPORTED;
#else
    /* The default S3 BOOT pin (GPIO0) is also the default OLED SCL pin. The
     * panel wins: sharing it would put I2C traffic on the button line. */
    if (CONFIG_POCKETWIKI_BUTTON_GPIO == CONFIG_POCKETWIKI_OLED_SDA ||
        CONFIG_POCKETWIKI_BUTTON_GPIO == CONFIG_POCKETWIKI_OLED_SCL) {
        ESP_LOGW(TAG, "button GPIO %d is used by the OLED I2C bus; move the display "
                      "to GPIO1/GPIO2 or pick another button GPIO",
                 CONFIG_POCKETWIKI_BUTTON_GPIO);
        return ESP_ERR_INVALID_STATE;
    }

    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << CONFIG_POCKETWIKI_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "GPIO %d configuration failed: %s", CONFIG_POCKETWIKI_BUTTON_GPIO,
                 esp_err_to_name(err));
        return err;
    }
    if (xTaskCreate(button_task, "boot_button", PW_BUTTON_TASK_STACK, NULL, 2, NULL) != pdPASS) {
        ESP_LOGW(TAG, "could not start the button task");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "boot button on GPIO %d (idle level %d)", CONFIG_POCKETWIKI_BUTTON_GPIO,
             gpio_get_level((gpio_num_t)CONFIG_POCKETWIKI_BUTTON_GPIO));
    return ESP_OK;
#endif
}
