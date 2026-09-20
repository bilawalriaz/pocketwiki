#include "oled_display.h"

#include <stdio.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_lcd_io_i2c.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_ssd1306.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "qr_code.h"

static const char *TAG = "oled";

static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_io;
static uint8_t s_fb[128 * 64 / 8];   /* 1 KB framebuffer, static */
static bool s_ok;

/* QR screen state: the matrix is kept out of the framebuffer so a symbol can be
 * redrawn whenever the button asks for it. */
static bool s_qr_active;
static uint8_t s_qr[QR_MAX_MODULES * QR_MAX_MODULES];

/* Transfer-screen state: the flag keeps the status loop from overwriting a
 * pack transfer, and the throttle caps redraws so a BLE upload (one small
 * chunk per acknowledged write) never stalls on framebuffer I2C traffic. */
static bool s_transfer_active;
static int64_t s_last_transfer_us;
static int s_last_transfer_pct = -1;

/* 5x7 font, glyphs 0x20..0x5A (space, punctuation, digits, uppercase).
 * Column-major: 5 bytes per glyph, MSB = top row. Uppercase-only rendering
 * keeps the table compact; the welcome screen has no lowercase needs. */
static const uint8_t FONT5x7[0x5B - 0x20][5] = {
    {0x00,0x00,0x00,0x00,0x00}, /*   */
    {0x00,0x00,0x5F,0x00,0x00}, /* ! */
    {0x00,0x07,0x00,0x07,0x00}, /* " */
    {0x14,0x7F,0x14,0x7F,0x14}, /* # */
    {0x24,0x2A,0x7F,0x2A,0x12}, /* $ */
    {0x23,0x13,0x08,0x64,0x62}, /* % */
    {0x36,0x49,0x55,0x22,0x50}, /* & */
    {0x00,0x05,0x03,0x00,0x00}, /* ' */
    {0x00,0x1C,0x22,0x41,0x00}, /* ( */
    {0x00,0x41,0x22,0x1C,0x00}, /* ) */
    {0x08,0x2A,0x1C,0x2A,0x08}, /* * */
    {0x08,0x08,0x3E,0x08,0x08}, /* + */
    {0x00,0x50,0x30,0x00,0x00}, /* , */
    {0x08,0x08,0x08,0x08,0x08}, /* - */
    {0x00,0x60,0x60,0x00,0x00}, /* . */
    {0x20,0x10,0x08,0x04,0x02}, /* / */
    {0x3E,0x51,0x49,0x45,0x3E}, /* 0 */
    {0x00,0x42,0x7F,0x40,0x00}, /* 1 */
    {0x42,0x61,0x51,0x49,0x46}, /* 2 */
    {0x21,0x41,0x45,0x4B,0x31}, /* 3 */
    {0x18,0x14,0x12,0x7F,0x10}, /* 4 */
    {0x27,0x45,0x45,0x45,0x39}, /* 5 */
    {0x3C,0x4A,0x49,0x49,0x30}, /* 6 */
    {0x01,0x71,0x09,0x05,0x03}, /* 7 */
    {0x36,0x49,0x49,0x49,0x36}, /* 8 */
    {0x06,0x49,0x49,0x29,0x1E}, /* 9 */
    {0x00,0x36,0x36,0x00,0x00}, /* : */
    {0x00,0x56,0x36,0x00,0x00}, /* ; */
    {0x00,0x08,0x14,0x22,0x41}, /* < */
    {0x14,0x14,0x14,0x14,0x14}, /* = */
    {0x41,0x22,0x14,0x08,0x00}, /* > */
    {0x02,0x01,0x51,0x09,0x06}, /* ? */
    {0x32,0x49,0x79,0x41,0x3E}, /* @ */
    {0x7E,0x11,0x11,0x11,0x7E}, /* A */
    {0x7F,0x49,0x49,0x49,0x36}, /* B */
    {0x3E,0x41,0x41,0x41,0x22}, /* C */
    {0x7F,0x41,0x41,0x22,0x1C}, /* D */
    {0x7F,0x49,0x49,0x49,0x41}, /* E */
    {0x7F,0x09,0x09,0x09,0x01}, /* F */
    {0x3E,0x41,0x49,0x49,0x7A}, /* G */
    {0x7F,0x08,0x08,0x08,0x7F}, /* H */
    {0x00,0x41,0x7F,0x41,0x00}, /* I */
    {0x20,0x40,0x41,0x3F,0x01}, /* J */
    {0x7F,0x08,0x14,0x22,0x41}, /* K */
    {0x7F,0x40,0x40,0x40,0x40}, /* L */
    {0x7F,0x02,0x0C,0x02,0x7F}, /* M */
    {0x7F,0x04,0x08,0x10,0x7F}, /* N */
    {0x3E,0x41,0x41,0x41,0x3E}, /* O */
    {0x7F,0x09,0x09,0x09,0x06}, /* P */
    {0x3E,0x41,0x51,0x21,0x5E}, /* Q */
    {0x7F,0x09,0x19,0x29,0x46}, /* R */
    {0x46,0x49,0x49,0x49,0x31}, /* S */
    {0x01,0x01,0x7F,0x01,0x01}, /* T */
    {0x3F,0x40,0x40,0x40,0x3F}, /* U */
    {0x1F,0x20,0x40,0x20,0x1F}, /* V */
    {0x3F,0x40,0x38,0x40,0x3F}, /* W */
    {0x63,0x14,0x08,0x14,0x63}, /* X */
    {0x07,0x08,0x70,0x08,0x07}, /* Y */
    {0x61,0x51,0x49,0x45,0x43}, /* Z */
};

#define OLED_W CONFIG_POCKETWIKI_OLED_WIDTH
#define OLED_H CONFIG_POCKETWIKI_OLED_HEIGHT
#define CHAR_W 6

static void fb_set_px(int x, int y, bool on)
{
    if (x < 0 || x >= OLED_W || y < 0 || y >= OLED_H) return;
    uint8_t *b = &s_fb[(y >> 3) * OLED_W + x];
    if (on) {
        *b |= (uint8_t)(1u << (y & 7));
    } else {
        *b &= (uint8_t)~(1u << (y & 7));
    }
}

static void draw_char(int x, int y, char c)
{
    if (c >= 'a' && c <= 'z') c = (char)(c - 0x20);   /* uppercase-only font */
    if (c < 0x20 || c > 0x5A) return;
    const uint8_t *g = FONT5x7[c - 0x20];
    for (int col = 0; col < 5; col++) {
        for (int row = 0; row < 7; row++) {
            /* Conventional 5x7 tables store the top pixel in bit 0. */
            fb_set_px(x + col, y + row, (g[col] >> row) & 1);
        }
    }
}

static int draw_text(int x, int y, const char *s)
{
    int xx = x;
    for (; *s; s++) {
        if (xx + CHAR_W > OLED_W) break;
        draw_char(xx, y, *s);
        xx += CHAR_W;
    }
    return xx;
}

static void draw_progress(int x, int y, int width, int height, size_t value, size_t total)
{
    for (int xx = 0; xx < width; xx++) {
        fb_set_px(x + xx, y, true);
        fb_set_px(x + xx, y + height - 1, true);
    }
    for (int yy = 0; yy < height; yy++) {
        fb_set_px(x, y + yy, true);
        fb_set_px(x + width - 1, y + yy, true);
    }
    int fill = total > 0 ? (int)(((uint64_t)(width - 4) * value) / total) : 0;
    if (fill > width - 4) fill = width - 4;
    for (int xx = 0; xx < fill; xx++) {
        for (int yy = 2; yy < height - 2; yy++) fb_set_px(x + 2 + xx, y + yy, true);
    }
}

static void flush(void)
{
    if (s_ok && s_panel != NULL) {
        esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, 0, 0, OLED_W, OLED_H, s_fb);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "framebuffer transfer failed: %s", esp_err_to_name(err));
        }
    }
}

esp_err_t oled_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = CONFIG_POCKETWIKI_OLED_SDA,
        .scl_io_num = CONFIG_POCKETWIKI_OLED_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus = NULL;
    if (i2c_new_master_bus(&bus_cfg, &bus) != ESP_OK) {
        ESP_LOGW(TAG, "I2C bus init failed; continuing without OLED");
        return ESP_FAIL;
    }

    /* Probe the configured address first, then the common alternative. */
    uint8_t addr = CONFIG_POCKETWIKI_OLED_I2C_ADDR;
    if (i2c_master_probe(bus, addr, 50) != ESP_OK) {
        addr = (addr == 0x3C) ? 0x3D : 0x3C;
        if (i2c_master_probe(bus, addr, 50) != ESP_OK) {
            ESP_LOGW(TAG, "no display found on I2C; continuing without OLED");
            return ESP_FAIL;
        }
    }

    esp_lcd_panel_io_i2c_config_t io_cfg = {
        .dev_addr = addr,
        .scl_speed_hz = 400000,
        .control_phase_bytes = 1,   /* SSD1306: first byte = command/data flag */
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .dc_bit_offset = 6,
    };
    esp_lcd_panel_io_handle_t io = NULL;
    if (esp_lcd_new_panel_io_i2c(bus, &io_cfg, &io) != ESP_OK) {
        ESP_LOGW(TAG, "LCD IO init failed; continuing without OLED");
        return ESP_FAIL;
    }
    s_io = io;

    esp_lcd_panel_ssd1306_config_t ssd_cfg = { .height = OLED_H };
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = -1,       /* no reset line */
        .bits_per_pixel = 1,        /* monochrome */
        .vendor_config = &ssd_cfg,
    };
    esp_err_t err = esp_lcd_new_panel_ssd1306(io, &panel_cfg, &s_panel);
    if (err == ESP_OK) err = esp_lcd_panel_reset(s_panel);
    if (err == ESP_OK) err = esp_lcd_panel_init(s_panel);
    if (err == ESP_OK) err = esp_lcd_panel_disp_on_off(s_panel, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SSD1306 init failed (%s); continuing without OLED",
                 esp_err_to_name(err));
        s_panel = NULL;
        return ESP_FAIL;
    }

    /* The SSD1306 powers up at contrast 0x7F and the esp_lcd driver leaves it
     * there, so the panel runs at roughly half brightness. Small QR modules
     * need every photon the panel can emit for a phone camera to resolve them. */
    if (esp_lcd_panel_io_tx_param(s_io, 0x81, (uint8_t[]){ 0xFF }, 1) != ESP_OK) {
        ESP_LOGW(TAG, "could not raise display contrast");
    }

    s_ok = true;
    ESP_LOGI(TAG, "SSD1306 ready at 0x%02x", addr);
    return ESP_OK;
}

void oled_show_boot(const char *line)
{
    if (!s_ok) return;
    memset(s_fb, 0, sizeof s_fb);
    draw_text(0, 0, "POCKETWIKI");
    draw_text(0, 16, line == NULL ? "STARTING" : line);
    if (line != NULL && strcmp(line, "STARTING") == 0) {
        draw_text(0, 32, "PLEASE WAIT");
    } else {
        draw_text(0, 32, "OPEN: 192.168.4.1");
        draw_text(0, 44, "FOR SETUP");
    }
    flush();
}

void oled_show_status(const char *ssid, const char *ip, uint32_t article_count,
                      uint32_t pack_count, size_t flash_used, size_t flash_total,
                      bool online, unsigned cpu_pct)
{
    if (!s_ok) return;
    (void)cpu_pct;
    s_qr_active = false;   /* a direct status draw takes the screen back */
    memset(s_fb, 0, sizeof s_fb);
    draw_text(0, 0, "POCKETWIKI");

    char buf[40];
    snprintf(buf, sizeof buf, online ? "ONLINE: %s" : "WIFI: %s",
             ssid == NULL ? "POCKETWIKI" : ssid);
    draw_text(0, 10, buf);

    draw_text(0, 20, "READ: 192.168.4.1");

    snprintf(buf, sizeof buf, online ? "LAN: %s" : "OPEN: %s",
             ip == NULL ? "192.168.4.1" : ip);
    draw_text(0, 30, buf);

    snprintf(buf, sizeof buf, "ART %lu / %lu PACK", (unsigned long)article_count,
             (unsigned long)pack_count);
    draw_text(0, 40, buf);

    draw_progress(0, 52, OLED_W, 10, flash_used, flash_total);

    flush();
}

void oled_show_transfer(const char *pack, size_t received, size_t total,
                        size_t flash_used, size_t flash_total)
{
    if (!s_ok) return;
    s_qr_active = false;   /* a transfer owns the screen */
    unsigned pct = total > 0 ? (unsigned)(((uint64_t)received * 100u) / total) : 0;
    if (pct > 100) pct = 100;
    bool force = s_last_transfer_pct < 0 || received >= total;
    if (!force && (int)pct == s_last_transfer_pct &&
            esp_timer_get_time() - s_last_transfer_us < 250000) {
        return;   /* same percent and < 250 ms since the last frame */
    }
    s_transfer_active = true;
    s_last_transfer_pct = (int)pct;
    s_last_transfer_us = esp_timer_get_time();
    memset(s_fb, 0, sizeof s_fb);
    draw_text(0, 0, "INSTALLING PACK");
    draw_text(0, 11, pack == NULL ? "LIBRARY" : pack);
    char buf[40];
    snprintf(buf, sizeof buf, "%u%% %lu/%luK", pct,
             (unsigned long)(received / 1024), (unsigned long)(total / 1024));
    draw_text(0, 24, buf);
    draw_progress(0, 35, OLED_W, 11, received, total);
    snprintf(buf, sizeof buf, "FLASH %lu/%luK", (unsigned long)(flash_used / 1024),
             (unsigned long)(flash_total / 1024));
    draw_text(0, 52, buf);
    flush();
}

bool oled_transfer_active(void)
{
    return s_transfer_active;
}

void oled_clear_transfer(void)
{
    s_transfer_active = false;
    s_last_transfer_pct = -1;
    s_last_transfer_us = 0;
}

bool oled_show_qr(const char *payload)
{
    if (!s_ok || payload == NULL || s_transfer_active) return false;
    int size = qr_encode(payload, s_qr);
    if (size == 0) {
        ESP_LOGW(TAG, "QR payload of %u bytes does not fit any symbol",
                 (unsigned)strlen(payload));
        return false;
    }
    /* Two pixels per module is the smallest that scans reliably, which caps
     * this panel at version 3 (29 modules). */
    if (size * 2 > OLED_H) {
        ESP_LOGW(TAG, "QR version %d needs %d px; too large for this display",
                 (size - 17) / 4, size * 2);
        return false;
    }

    /* A QR symbol is dark modules on a light field. On this panel "light" is a
     * lit pixel, so the symbol sits on a lit card: the card is the quiet zone,
     * and everything outside it stays dark. Keeping the lit area small makes
     * the phone meter for the symbol instead of a screenful of light, and cuts
     * the glare a glossy cover returns at close range. */
    int quiet = 4;
    while (quiet > 1 && (size + 2 * quiet) * 2 > OLED_H) quiet--;
    int card = (size + 2 * quiet) * 2;
    int x0 = (OLED_W - card) / 2;
    int y0 = (OLED_H - card) / 2;

    memset(s_fb, 0, sizeof s_fb);
    for (int y = 0; y < card; y++) {
        for (int x = 0; x < card; x++) fb_set_px(x0 + x, y0 + y, true);
    }
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            if (!s_qr[y * size + x]) continue;   /* light module: card shows through */
            int px = x0 + (quiet + x) * 2;
            int py = y0 + (quiet + y) * 2;
            for (int dy = 0; dy < 2; dy++) {
                for (int dx = 0; dx < 2; dx++) fb_set_px(px + dx, py + dy, false);
            }
        }
    }
    ESP_LOGI(TAG, "QR version %d at %d,%d: %d px card, quiet %d modules", (size - 17) / 4,
             x0, y0, card, quiet);
    s_qr_active = true;
    flush();
    return true;
}

bool oled_qr_active(void)
{
    return s_qr_active;
}

void oled_clear_qr(void)
{
    s_qr_active = false;
}
