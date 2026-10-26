#include "display.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <atomic>

#include "board.h"
#include "config.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "font5x7.h"
#include "font_lowercase.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "shared_state.h"
#include "trainer_network_config.h"
#include "wifi_qr.h"

static const char* TAG = "display";

#define LCD_H_RES 800
#define LCD_V_RES 480

// RGB565 palette
#define C_BG 0x10A2      // charcoal
#define C_HEADER 0x18E4  // dark blue-gray
#define C_CARD 0x2126    // raised panel
#define C_CARD_ALT 0x2948
#define C_LINE 0x4208
#define C_WHITE 0xFFFF
#define C_MUTED 0x9CF3
#define C_GREEN 0x2EE5
#define C_GREEN_DIM 0x1503
#define C_AMBER 0xFD20
#define C_RED 0xF9E7
#define C_CYAN 0x2E9F
#define C_BLACK 0x0000

#define CHAR_W(scale) (6 * (scale))
#define CHAR_H(scale) (7 * (scale))

static esp_lcd_panel_handle_t s_panel;
static uint16_t* s_buf;
static std::atomic<bool> s_status[ST_COUNT];

static i2c_master_dev_handle_t s_touch;
static bool s_touch_ok;
static bool s_touch_down;
static uint16_t s_touch_x;
static uint16_t s_touch_y;
static uint8_t s_touch_points;

// Separate page-composition buffer, allocated once in PSRAM.
// This is not a second driver framebuffer or a VSYNC page flip.
static uint16_t* s_page_canvas = nullptr;
static bool s_composing_page = false;

static esp_err_t render_bitmap(esp_lcd_panel_handle_t panel, int x0, int y0,
                               int x1, int y1, const void* pixels) {
    if (!s_composing_page) {
        return esp_lcd_panel_draw_bitmap(panel, x0, y0, x1, y1, pixels);
    }

    if (!pixels || x0 < 0 || y0 < 0 || x1 > LCD_H_RES || y1 > LCD_V_RES ||
        x1 <= x0 || y1 <= y0) {
        return ESP_ERR_INVALID_ARG;
    }

    const int width = x1 - x0;
    const uint16_t* source = static_cast<const uint16_t*>(pixels);

    for (int y = y0; y < y1; ++y) {
        memcpy(s_page_canvas + y * LCD_H_RES + x0, source + (y - y0) * width,
               static_cast<size_t>(width) * sizeof(uint16_t));
    }

    return ESP_OK;
}

static bool begin_page_composition() {
    if (!s_page_canvas) {
        s_page_canvas = static_cast<uint16_t*>(heap_caps_malloc(
            static_cast<size_t>(LCD_H_RES) * LCD_V_RES * sizeof(uint16_t),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }

    if (!s_page_canvas) {
        ESP_LOGE(TAG, "page composition buffer allocation failed");
        return false;
    }

    s_composing_page = true;
    return true;
}

static void present_composed_page() {
    s_composing_page = false;

    esp_err_t error = esp_lcd_panel_draw_bitmap(s_panel, 0, 0, LCD_H_RES,
                                                LCD_V_RES, s_page_canvas);

    if (error != ESP_OK) {
        ESP_LOGE(TAG, "page presentation failed: %s", esp_err_to_name(error));
    }
}

// The RGB driver copies each draw into its PSRAM frame buffer before returning,
// so this internal-RAM scratch buffer can be reused immediately.
static void fill_rect(int x, int y, int w, int h, uint16_t color) {
    if (w <= 0 || h <= 0) return;

    for (int i = 0; i < w; ++i) s_buf[i] = color;
    for (int row = 0; row < h; ++row) {
        render_bitmap(s_panel, x, y + row, x + w, y + row + 1, s_buf);
    }
}

static void outline_rect(int x, int y, int w, int h, uint16_t color) {
    fill_rect(x, y, w, 1, color);
    fill_rect(x, y + h - 1, w, 1, color);
    fill_rect(x, y, 1, h, color);
    fill_rect(x + w - 1, y, 1, h, color);
}

static void draw_char(int x, int y, char ch, int scale, uint16_t fg,
                      uint16_t bg) {
    const uint8_t* glyph;
    if (ch >= 'a' && ch <= 'z')
        glyph = FONT_LOWERCASE[ch - 'a'];
    else {
        if (ch < 0x20 || ch > 0x5F) ch = '?';
        glyph = FONT5X7[ch - 0x20];
    }
    const int w = CHAR_W(scale);
    const int h = CHAR_H(scale);

    for (int py = 0; py < h; ++py) {
        const int glyph_row = py / scale;
        for (int px = 0; px < w; ++px) {
            const int glyph_col = px / scale;
            const bool on =
                glyph_col < 5 && ((glyph[glyph_col] >> glyph_row) & 1);
            s_buf[py * w + px] = on ? fg : bg;
        }
    }

    render_bitmap(s_panel, x, y, x + w, y + h, s_buf);
}

static void draw_text(int x, int y, const char* text, int scale, uint16_t fg,
                      uint16_t bg) {
    while (*text && x + CHAR_W(scale) <= LCD_H_RES) {
        draw_char(x, y, *text++, scale, fg, bg);
        x += CHAR_W(scale);
    }
}

static int clamp_int(float value, int low, int high) {
    if (!isfinite(value)) return 0;
    if (value < (float)low) return low;
    if (value > (float)high) return high;
    return (int)(value + (value < 0.0f ? -0.5f : 0.5f));
}

static void fmt1(char* out, size_t out_size, float value) {
    if (!isfinite(value)) {
        snprintf(out, out_size, "--.-");
        return;
    }

    // Clamp before conversion so formatting has a known, small upper bound.
    if (value > 999.9f) value = 999.9f;
    if (value < -99.9f) value = -99.9f;

    int tenths = (int)(value * 10.0f + (value < 0.0f ? -0.5f : 0.5f));
    int magnitude = tenths < 0 ? -tenths : tenths;
    snprintf(out, out_size, "%s%d.%d", tenths < 0 ? "-" : "", magnitude / 10,
             magnitude % 10);
}

// -----------------------------------------------------------------------------
// GT911 touch diagnostics
// -----------------------------------------------------------------------------

static esp_err_t gt911_read(uint16_t reg, uint8_t* data, size_t len) {
    uint8_t addr[2] = {(uint8_t)(reg >> 8), (uint8_t)reg};
    return i2c_master_transmit_receive(s_touch, addr, sizeof(addr), data, len,
                                       50);
}

static esp_err_t gt911_write8(uint16_t reg, uint8_t value) {
    uint8_t tx[3] = {(uint8_t)(reg >> 8), (uint8_t)reg, value};
    return i2c_master_transmit(s_touch, tx, sizeof(tx), 50);
}

static void gt911_start(void) {
    i2c_master_bus_handle_t bus = board_i2c_bus();
    if (!bus) {
        ESP_LOGE(TAG, "touch: shared I2C bus unavailable");
        return;
    }

    i2c_device_config_t config = {};
    config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    config.device_address = 0x5D;
    config.scl_speed_hz = 400000;

    if (i2c_master_bus_add_device(bus, &config, &s_touch) != ESP_OK) {
        ESP_LOGE(TAG, "touch: could not add GT911 at 0x5D");
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(250));

    uint8_t id[4] = {};
    if (gt911_read(0x8140, id, sizeof(id)) != ESP_OK) {
        ESP_LOGE(TAG, "touch: GT911 not responding at 0x5D");
        return;
    }

    s_touch_ok = true;
    ESP_LOGI(TAG, "touch: GT911 product ID %.4s", (char*)id);
}

static void gt911_poll(void) {
    if (!s_touch_ok) return;

    uint8_t status = 0;
    if (gt911_read(0x814E, &status, 1) != ESP_OK) {
        s_touch_ok = false;
        s_touch_down = false;
        s_touch_points = 0;
        ESP_LOGE(TAG, "touch: status read failed");
        return;
    }

    if (!(status & 0x80)) return;

    const uint8_t points = status & 0x0F;
    s_touch_points = points <= 5 ? points : 0;
    s_touch_down = false;

    if (s_touch_points > 0) {
        // Point 1 on this panel: 0x814F track ID, then X/Y beginning at 0x8150.
        uint8_t point[6] = {};
        if (gt911_read(0x8150, point, sizeof(point)) == ESP_OK) {
            const uint16_t x = (uint16_t)point[0] | ((uint16_t)point[1] << 8);
            const uint16_t y = (uint16_t)point[2] | ((uint16_t)point[3] << 8);

            // Keep the last valid point for diagnostics after release.
            if (x < 4096 && y < 4096) {
                s_touch_x = x;
                s_touch_y = y;
                s_touch_down = true;
            } else {
                ESP_LOGW(TAG, "touch: invalid packet x=%u y=%u", (unsigned)x,
                         (unsigned)y);
            }
        }
    }

    if (gt911_write8(0x814E, 0) != ESP_OK) {
        ESP_LOGW(TAG, "touch: status clear failed");
    }
}

// -----------------------------------------------------------------------------
// Status dashboard
// -----------------------------------------------------------------------------

enum Page { PAGE_PWM, PAGE_AIR, PAGE_STATUS, PAGE_WIFI };
static Page s_page = PAGE_PWM;
static bool s_page_dirty = true;
static char s_cache[12][81];
static uint16_t s_cache_color[12];

// Full-page clears happen only on tab changes. Values are padded and only
// redrawn when their text/color changes, avoiding the previous strobing.
static void field(int slot, int x, int y, int width, const char* text,
                  uint16_t color, int scale = 2, uint16_t bg = C_CARD) {
    char padded[81];
    if (width < 1 || width > 80) return;
    memset(padded, ' ', (size_t)width);
    size_t len = strlen(text);
    if (len > (size_t)width) len = (size_t)width;
    memcpy(padded, text, len);
    padded[width] = 0;
    if (!strcmp(s_cache[slot], padded) && s_cache_color[slot] == color) return;
    memcpy(s_cache[slot], padded, (size_t)width + 1);
    s_cache_color[slot] = color;
    draw_text(x, y, padded, scale, color, bg);
}

static void draw_qr() {
    // Includes the four-module white quiet zone generated with the QR matrix.
    const int scale = 6;
    const int x = 450, y = 112;
    for (int row = 0; row < WIFI_QR_SIZE; ++row) {
        for (int col = 0; col < WIFI_QR_SIZE; ++col) {
            fill_rect(x + col * scale, y + row * scale, scale, scale,
                      WIFI_QR[row][col] == '1' ? C_BLACK : C_WHITE);
        }
    }
}

static void draw_static_layout() {
    memset(s_cache, 0, sizeof(s_cache));
    fill_rect(0, 0, 800, 480, C_BG);
    static const char* const tabs[] = {"PWM", "AIR DATA", "STATUS",
                                       "WIFI/SETTINGS"};
    for (int i = 0; i < 4; ++i) {
        const uint16_t bg = i == (int)s_page ? C_CARD : C_HEADER;
        fill_rect(i * 200, 0, 200, 64, bg);
        int x = i * 200 + (200 - (int)strlen(tabs[i]) * 12) / 2;
        draw_text(x, 24, tabs[i], 2, i == (int)s_page ? C_CYAN : C_MUTED, bg);
        fill_rect(i * 200, 61, 200, 3, i == (int)s_page ? C_CYAN : C_LINE);
    }
    fill_rect(20, 82, 370, 326, C_CARD);
    outline_rect(20, 82, 370, 326, C_LINE);
    const char* title = s_page == PAGE_PWM      ? "PWM OUTPUT"
                        : s_page == PAGE_AIR    ? "AIR DATA"
                        : s_page == PAGE_STATUS ? "SYSTEM STATUS"
                                                : "WIFI HOTSPOT";
    draw_text(38, 100, title, 3, C_CYAN, C_CARD);
    fill_rect(38, 130, 334, 1, C_LINE);
    if (s_page == PAGE_PWM) {
        draw_text(38, 150, "SET", 2, C_MUTED, C_CARD);
        draw_text(38, 186, "MEASURED", 2, C_MUTED, C_CARD);
        draw_text(38, 222, "FREQUENCY", 2, C_MUTED, C_CARD);
        draw_text(38, 258, "SOURCE", 2, C_MUTED, C_CARD);
        draw_text(38, 294, "POT INPUT", 2, C_MUTED, C_CARD);
    } else if (s_page == PAGE_AIR) {
        draw_text(38, 150, "IAS", 2, C_MUTED, C_CARD);
        draw_text(38, 186, "ALTITUDE", 2, C_MUTED, C_CARD);
        draw_text(38, 222, "SENSORS", 2, C_MUTED, C_CARD);
    } else if (s_page == PAGE_STATUS) {
        draw_text(38, 150, "USB STACK", 2, C_MUTED, C_CARD);
        draw_text(38, 186, "WEB SERVER", 2, C_MUTED, C_CARD);
        draw_text(38, 222, "TOUCH", 2, C_MUTED, C_CARD);
        draw_text(38, 258, "UPTIME", 2, C_MUTED, C_CARD);
        draw_text(38, 294, "WIFI AP", 2, C_MUTED, C_CARD);
    } else {
        // Lowercase is preserved for the password using case-aware rendering.
        draw_text(38, 148, "NETWORK", 2, C_MUTED, C_CARD);
        draw_text(38, 173, TRAINER_WIFI_SSID, 2, C_WHITE, C_CARD);
        draw_text(38, 211, "PASSWORD (CASE SENSITIVE)", 1, C_MUTED, C_CARD);
        draw_text(38, 233, TRAINER_WIFI_PASSWORD, 2, C_WHITE, C_CARD);
        draw_text(38, 273, "BROWSER: 192.168.4.1", 2, C_CYAN, C_CARD);
        draw_text(38, 307, "SECURITY: WPA2", 2, C_MUTED, C_CARD);
        draw_text(38, 341, "NO INTERNET / LOCAL TRAINER ONLY", 1, C_MUTED,
                  C_CARD);
        draw_qr();
        draw_text(450, 378, "SCAN TO JOIN WIFI", 2, C_WHITE, C_BG);
        draw_text(450, 403, "THEN OPEN 192.168.4.1", 1, C_MUTED, C_BG);
    }
    draw_text(20, 452, "CYDTRAINER / STATUS DISPLAY", 1, C_MUTED, C_BG);
}

static bool wifi_ready() {
    wifi_mode_t mode = WIFI_MODE_NULL;
    return esp_wifi_get_mode(&mode) == ESP_OK &&
           (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA);
}

static void draw_dashboard(bool heartbeat) {
    Telemetry t = telemetry_get();
    char text[64], number[16];
    if (s_page == PAGE_PWM) {
        fmt1(number, sizeof(number), t.pwm_set_pct);
        snprintf(text, sizeof(text), "%.15s %%", number);
        field(0, 176, 150, 16, text, C_WHITE);
        fmt1(number, sizeof(number), t.pwm_duty_pct);
        snprintf(text, sizeof(text), "%.15s %%", number);
        const bool mismatch =
            t.pwm_freq_hz > 0 && fabsf(t.pwm_duty_pct - t.pwm_set_pct) > 2;
        field(1, 176, 186, 16, text, mismatch ? C_AMBER : C_GREEN);
        snprintf(text, sizeof(text), "%d HZ",
                 clamp_int(t.pwm_freq_hz, 0, 99999));
        field(2, 176, 222, 16, text, C_WHITE);
        field(3, 176, 258, 16, t.pwm_manual ? "MANUAL" : "POT", C_GREEN);
        fmt1(number, sizeof(number), t.pot_v);
        snprintf(text, sizeof(text), "%.15s V", number);
        field(4, 176, 294, 16, text, C_MUTED);
    } else if (s_page == PAGE_AIR) {
        snprintf(text, sizeof(text), "%d KT", clamp_int(t.ias_kt, 0, 9999));
        field(0, 176, 150, 16, text, t.sensors_sim ? C_AMBER : C_WHITE);
        snprintf(text, sizeof(text), "%d FT",
                 clamp_int(t.alt_ft, -99999, 99999));
        field(1, 176, 186, 16, text, t.sensors_sim ? C_AMBER : C_WHITE);
        field(2, 176, 222, 16,
              t.sensors_sim      ? "SIMULATED"
              : t.adc_sensors_ok ? "LIVE"
                                 : "READ ERROR",
              t.sensors_sim      ? C_AMBER
              : t.adc_sensors_ok ? C_GREEN
                                 : C_RED);
    } else if (s_page == PAGE_STATUS) {
        field(0, 176, 150, 16, s_status[ST_USB].load() ? "STARTED" : "STARTING",
              C_GREEN);
        field(1, 176, 186, 16, s_status[ST_WEB].load() ? "UP" : "STARTING",
              C_GREEN);
        field(2, 176, 222, 16, s_touch_ok ? "READY" : "NOT FOUND",
              s_touch_ok ? C_GREEN : C_RED);
        uint32_t sec = t.uptime_ms / 1000;
        snprintf(text, sizeof(text), "%02u:%02u:%02u", (unsigned)(sec / 3600),
                 (unsigned)(sec / 60 % 60), (unsigned)(sec % 60));
        field(3, 176, 258, 16, text, C_WHITE);
        field(4, 176, 294, 16, wifi_ready() ? "CONFIGURED" : "UNAVAILABLE",
              C_CYAN);
        snprintf(text, sizeof(text), "LAST TOUCH X%u Y%u", (unsigned)s_touch_x,
                 (unsigned)s_touch_y);
        field(5, 38, 350, 30, text, C_MUTED, 1);
    } else {
        field(0, 38, 373, 27,
              wifi_ready() ? "AP CONFIGURED" : "AP NOT AVAILABLE", C_CYAN, 1);
    }
    fill_rect(758, 450, 10, 10, heartbeat ? C_GREEN : C_GREEN_DIM);
}

static void backlight_on(void) {
    // Preserve touch/LCD reset release and SD deselect; keep USB_SEL low.
    const uint8_t outputs =
        EXIO_CTP_RST | EXIO_LCD_RST | EXIO_SD_CS | EXIO_BACKLIGHT;

    for (int attempt = 0; attempt < 20; ++attempt) {
        if (board_io_write(outputs) == ESP_OK) {
            ESP_LOGI(TAG, "backlight on");
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    ESP_LOGE(TAG, "backlight: CH422G not responding");
}

static void display_task(void*) {
    // Initial screen, drawn directly to the panel.
    s_composing_page = false;
    draw_static_layout();
    backlight_on();
    gt911_start();

    bool was_down = false;
    bool heartbeat = false;

    TickType_t last_refresh = xTaskGetTickCount();
    TickType_t last_heartbeat = last_refresh;

    s_page_dirty = true;

    while (true) {
        gt911_poll();

        // Change tabs only on a new press, not continuously while held.
        if (s_touch_down && !was_down && s_touch_y < 64 &&
            s_touch_x < LCD_H_RES) {
            const Page next = static_cast<Page>(s_touch_x / 200);

            if (next != s_page) {
                s_page = next;
                s_page_dirty = true;
            }
        }

        was_down = s_touch_down;

        const TickType_t now = xTaskGetTickCount();
        bool refresh_due = false;

        // Heartbeat timing is independent of page changes.
        if ((TickType_t)(now - last_heartbeat) >= pdMS_TO_TICKS(1000)) {
            last_heartbeat = now;
            heartbeat = !heartbeat;
            refresh_due = true;
        }

        // Refresh telemetry, including uptime, every 200 ms.
        if ((TickType_t)(now - last_refresh) >= pdMS_TO_TICKS(200)) {
            last_refresh = now;
            refresh_due = true;
        }

        if (s_page_dirty) {
            if (begin_page_composition()) {
                draw_static_layout();
                draw_dashboard(heartbeat);
                present_composed_page();
            } else {
                s_composing_page = false;
                draw_static_layout();
                draw_dashboard(heartbeat);
            }

            // All subsequent live updates go directly to the panel.
            s_composing_page = false;
            s_page_dirty = false;
            last_refresh = now;
        } else if (refresh_due) {
            s_composing_page = false;
            draw_dashboard(heartbeat);
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
void display_set_status(status_item_t item, bool ok) {
    if ((unsigned)item < (unsigned)ST_COUNT) s_status[item].store(ok);
}

esp_err_t display_start(void) {
    esp_lcd_rgb_panel_config_t config = {};
    config.clk_src = LCD_CLK_SRC_DEFAULT;
    config.timings.pclk_hz = 16 * 1000 * 1000;
    config.timings.h_res = LCD_H_RES;
    config.timings.v_res = LCD_V_RES;
    config.timings.hsync_pulse_width = 4;
    config.timings.hsync_back_porch = 8;
    config.timings.hsync_front_porch = 8;
    config.timings.vsync_pulse_width = 4;
    config.timings.vsync_back_porch = 8;
    config.timings.vsync_front_porch = 8;
    config.timings.flags.pclk_active_neg = 1;

    config.data_width = 16;
    config.hsync_gpio_num = 46;
    config.vsync_gpio_num = 3;
    config.de_gpio_num = 5;
    config.pclk_gpio_num = 7;
    config.disp_gpio_num = -1;

    const int data_gpio[16] = {
        14, 38, 18, 17, 10,      // B3..B7
        39, 0,  45, 48, 47, 21,  // G2..G7
        1,  2,  42, 41, 40       // R3..R7
    };

    for (int i = 0; i < 16; ++i) {
        config.data_gpio_nums[i] = data_gpio[i];
    }

    config.flags.fb_in_psram = 1;

    // Required on this board to prevent RGB drift while flash/PSRAM are active.
    config.bounce_buffer_size_px = LCD_H_RES * 10;

    ESP_RETURN_ON_ERROR(esp_lcd_new_rgb_panel(&config, &s_panel), TAG,
                        "new panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "init");

    // Enough for either one full scan line (800 pixels) or one scale-4 glyph.
    const size_t scanline_pixels = LCD_H_RES;
    const size_t glyph_pixels = CHAR_W(4) * CHAR_H(4);
    const size_t scratch_pixels =
        scanline_pixels > glyph_pixels ? scanline_pixels : glyph_pixels;

    s_buf = (uint16_t*)heap_caps_malloc(scratch_pixels * sizeof(uint16_t),
                                        MALLOC_CAP_INTERNAL);
    ESP_RETURN_ON_FALSE(s_buf, ESP_ERR_NO_MEM, TAG, "draw buffer");

    BaseType_t created = xTaskCreatePinnedToCore(display_task, "display", 4096,
                                                 nullptr, 2, nullptr, 0);
    ESP_RETURN_ON_FALSE(created == pdPASS, ESP_FAIL, TAG, "task");

    ESP_LOGI(TAG, "RGB panel up %dx%d", LCD_H_RES, LCD_V_RES);
    return ESP_OK;
}
