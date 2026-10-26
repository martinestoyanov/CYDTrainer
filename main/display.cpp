#include "display.h"

#include <stdio.h>
#include <string.h>

#include "board.h"
#include "config.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_log.h"
#include "font5x7.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "shared_state.h"

static const char* TAG = "display";

#define LCD_H_RES 800
#define LCD_V_RES 480

// RGB565 colours
#define C_BLACK 0x0000
#define C_WHITE 0xFFFF
#define C_GRAY 0x8410
#define C_GREEN 0x07E0
#define C_RED 0xF800
#define C_AMBER 0xFD20
#define C_CYAN 0x07FF

#define MARGIN 20
#define TEXT_S 3             // body text scale: 18 x 21 px per character
#define CHAR_W(s) (6 * (s))  // 5 px glyph + 1 px gap
#define CHAR_H(s) (7 * (s))

static esp_lcd_panel_handle_t s_panel;
static uint16_t* s_buf;  // scratch, internal RAM, one text line tall
static volatile bool s_status[ST_COUNT];
static i2c_master_dev_handle_t s_touch;
static bool s_touch_ok;
static bool s_touch_down;
static uint16_t s_touch_x, s_touch_y;
static uint8_t s_touch_points;
// The RGB driver copies each draw into the PSRAM frame buffer before returning,
// so s_buf can be reused straight away.
static void fill_rect(int x, int y, int w, int h, uint16_t c) {
    for (int i = 0; i < w; i++) s_buf[i] = c;
    for (int r = 0; r < h; r++)
        esp_lcd_panel_draw_bitmap(s_panel, x, y + r, x + w, y + r + 1, s_buf);
}

// Draws one character cell (glyph + gap) at scale s.
static void draw_char(int x, int y, char ch, int s, uint16_t fg, uint16_t bg) {
    if (ch >= 'a' && ch <= 'z') ch -= 32;
    if (ch < 0x20 || ch > 0x5F) ch = '?';
    const uint8_t* g = FONT5X7[ch - 0x20];
    const int w = CHAR_W(s), h = CHAR_H(s);
    for (int py = 0; py < h; py++) {
        int row = py / s;
        for (int px = 0; px < w; px++) {
            int col = px / s;
            bool on = col < 5 && ((g[col] >> row) & 1);
            s_buf[py * w + px] = on ? fg : bg;
        }
    }
    esp_lcd_panel_draw_bitmap(s_panel, x, y, x + w, y + h, s_buf);
}

// Draws text and blanks the rest of the line up to the right margin, so a
// shorter string fully replaces a longer one.
static void draw_line(int y, const char* txt, int s, uint16_t fg, uint16_t bg) {
    int x = MARGIN;
    for (const char* p = txt; *p && x + CHAR_W(s) <= LCD_H_RES - MARGIN;
         p++, x += CHAR_W(s))
        draw_char(x, y, *p, s, fg, bg);
    if (x < LCD_H_RES - MARGIN)
        fill_rect(x, y, LCD_H_RES - MARGIN - x, CHAR_H(s), bg);
}
// GT911 uses a 16-bit register address, MSB first.
// Waveshare maps the touch controller to I2C address 0x5D.
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

    i2c_device_config_t dc = {};
    dc.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dc.device_address = 0x5D;
    dc.scl_speed_hz = 400000;

    if (i2c_master_bus_add_device(bus, &dc, &s_touch) != ESP_OK) {
        ESP_LOGE(TAG, "touch: could not add GT911 at 0x5D");
        return;
    }

    // Allow the GT911 time after reset release before reading it.
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
    if (!s_touch_ok) {
        return;
    }

    uint8_t status = 0;

    if (gt911_read(0x814E, &status, 1) != ESP_OK) {
        s_touch_ok = false;
        s_touch_down = false;
        s_touch_points = 0;

        ESP_LOGE(TAG, "touch: status read failed");
        return;
    }

    // Bit 7 means a new coordinate packet is ready.
    if (!(status & 0x80)) {
        return;
    }

    uint8_t n = status & 0x0F;

    s_touch_points = n <= 5 ? n : 0;
    s_touch_down = false;

    if (s_touch_points > 0) {
        uint8_t p[6] = {};

        /*
         * Point 1 layout used by this panel:
         *
         * 0x814F = track ID
         * 0x8150 = X low
         * 0x8151 = X high
         * 0x8152 = Y low
         * 0x8153 = Y high
         * 0x8154 = touch size low
         * 0x8155 = touch size high
         *
         * Read from 0x8150 so the coordinate bytes begin at p[0].
         */
        if (gt911_read(0x8150, p, sizeof(p)) == ESP_OK) {
            uint16_t x = (uint16_t)p[0] | ((uint16_t)p[1] << 8);

            uint16_t y = (uint16_t)p[2] | ((uint16_t)p[3] << 8);

            /*
             * Valid panel coordinates should be well below 4096.
             * Reject obviously misaligned or corrupt packets instead of
             * displaying wrapped values such as 65000.
             */
            if (x < 4096 && y < 4096) {
                s_touch_x = x;
                s_touch_y = y;
                s_touch_down = true;
            } else {
                ESP_LOGW(TAG, "touch: invalid packet x=%u y=%u", (unsigned)x,
                         (unsigned)y);
            }
        } else {
            ESP_LOGW(TAG, "touch: coordinate read failed");
        }
    }

    // Clear the ready flag after consuming the coordinate packet.
    if (gt911_write8(0x814E, 0) != ESP_OK) {
        ESP_LOGW(TAG, "touch: status clear failed");
    }
}
static void backlight_on(void) {
    // board_io_write() sets ALL CH422G outputs, so keep the bits
    // board_io_init() set (touch + LCD out of reset, SD deselected) and leave
    // USB_SEL = 0 (USB).
    const uint8_t out =
        EXIO_CTP_RST | EXIO_LCD_RST | EXIO_SD_CS | EXIO_BACKLIGHT;
    for (int tries = 0; tries < 20; tries++) {
        if (board_io_write(out) == ESP_OK) {
            ESP_LOGI(TAG, "backlight on");
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_LOGE(TAG, "backlight: CH422G not responding");
}

// Formats v (may be negative) with one decimal place using integers only.
static void fmt1(char* out, size_t n, float v) {
    int t = (int)(v * 10.0f + (v < 0 ? -0.5f : 0.5f));
    snprintf(out, n, "%s%d.%d", t < 0 ? "-" : "", (t < 0 ? -t : t) / 10,
             (t < 0 ? -t : t) % 10);
}

static void draw_status(void) {
    char line[48], a[12], b[12];
    const int step = CHAR_H(TEXT_S) + 14;  // 35 px between lines
    int y = 90;
    Telemetry t = telemetry_get();

    draw_line(y, s_status[ST_BOOT] ? "BOOT     OK" : "BOOT     ...", TEXT_S,
              s_status[ST_BOOT] ? C_GREEN : C_AMBER, C_BLACK);
    y += step;
    snprintf(line, sizeof line, "USB NET  %s",
             s_status[ST_USB] ? NET_IP : "STARTING");
    draw_line(y, line, TEXT_S, s_status[ST_USB] ? C_GREEN : C_AMBER, C_BLACK);
    y += step;
    draw_line(y, s_status[ST_WEB] ? "WEB      UP" : "WEB      STARTING", TEXT_S,
              s_status[ST_WEB] ? C_GREEN : C_AMBER, C_BLACK);
    y += step;
    draw_line(
        y, t.sensors_sim ? "SENSORS  SIMULATED (NO ADS1115)" : "SENSORS  LIVE",
        TEXT_S, t.sensors_sim ? C_AMBER : C_GREEN, C_BLACK);
    y += step;
    fmt1(a, sizeof a, t.pwm_set_pct);
    fmt1(b, sizeof b, t.pwm_duty_pct);
    snprintf(line, sizeof line, "PWM      SET %s%%  MEAS %s%%", a, b);
    draw_line(y, line, TEXT_S, C_WHITE, C_BLACK);
    y += step;
    snprintf(line, sizeof line, "         %d HZ  %s",
             (int)(t.pwm_freq_hz + 0.5f), t.pwm_manual ? "MANUAL" : "POT");
    draw_line(y, line, TEXT_S, C_WHITE, C_BLACK);
    y += step;
    snprintf(line, sizeof line, "IAS      %d KT   ALT %d FT",
             (int)(t.ias_kt + 0.5f),
             (int)(t.alt_ft + (t.alt_ft < 0 ? -0.5f : 0.5f)));
    draw_line(y, line, TEXT_S, C_WHITE, C_BLACK);
    y += step;
    uint32_t s = t.uptime_ms / 1000;
    snprintf(line, sizeof line, "UPTIME   %02u:%02u:%02u", (unsigned)(s / 3600),
             (unsigned)(s / 60 % 60), (unsigned)(s % 60));

    draw_line(y, line, TEXT_S, C_CYAN, C_BLACK);

    y += step;

    if (!s_touch_ok) {
        draw_line(y, "TOUCH    NOT FOUND", TEXT_S, C_RED, C_BLACK);
    } else {
        snprintf(line, sizeof line, "TOUCH %s X %u Y %u",
                 s_touch_down ? "DOWN" : "UP  ", (unsigned)s_touch_x,
                 (unsigned)s_touch_y);

        draw_line(y, line, TEXT_S, s_touch_down ? C_GREEN : C_CYAN, C_BLACK);
    }
}

static void display_task(void*) {
    backlight_on();
    fill_rect(0, 0, LCD_H_RES, LCD_V_RES, C_BLACK);
    fill_rect(0, 0, LCD_H_RES, 64, C_WHITE);
    draw_line(18, "E175 TEST BENCH", 4, C_BLACK, C_WHITE);

    gt911_start();

    bool beat = false;
    int redraw_ticks = 0;

    while (true) {
        gt911_poll();

        // Touch is checked every 20 ms.
        // The status screen itself still redraws every 500 ms.
        if (++redraw_ticks >= 25) {
            redraw_ticks = 0;

            draw_status();

            fill_rect(LCD_H_RES - 70, LCD_V_RES - 70, 40, 40,
                      beat ? C_AMBER : C_RED);

            beat = !beat;
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void display_set_status(status_item_t item, bool ok) {
    if (item < ST_COUNT) s_status[item] = ok;
}

esp_err_t display_start(void) {
    // Timing values from Waveshare ESP-IDF demo 08_lvgl_Porting
    // (waveshare_rgb_lcd_port.c / .h). Pins from the Waveshare pin table.
    esp_lcd_rgb_panel_config_t cfg = {};
    cfg.clk_src = LCD_CLK_SRC_DEFAULT;
    cfg.timings.pclk_hz = 16 * 1000 * 1000;
    cfg.timings.h_res = LCD_H_RES;
    cfg.timings.v_res = LCD_V_RES;
    cfg.timings.hsync_pulse_width = 4;
    cfg.timings.hsync_back_porch = 8;
    cfg.timings.hsync_front_porch = 8;
    cfg.timings.vsync_pulse_width = 4;
    cfg.timings.vsync_back_porch = 8;
    cfg.timings.vsync_front_porch = 8;
    cfg.timings.flags.pclk_active_neg = 1;
    cfg.data_width = 16;  // RGB565
    cfg.hsync_gpio_num = 46;
    cfg.vsync_gpio_num = 3;
    cfg.de_gpio_num = 5;
    cfg.pclk_gpio_num = 7;
    cfg.disp_gpio_num = -1;  // backlight is on the CH422G, not a GPIO
    const int data[16] = {
        14, 38, 18, 17, 10,      // B3..B7
        39, 0,  45, 48, 47, 21,  // G2..G7
        1,  2,  42, 41, 40       // R3..R7
    };
    for (int i = 0; i < 16; i++) cfg.data_gpio_nums[i] = data[i];
    cfg.flags.fb_in_psram = 1;  // 800*480*2 = 750 KB frame buffer
    cfg.bounce_buffer_size_px =
        LCD_H_RES *
        10;  // 10 lines in internal RAM; feeds the panel during flash access

    ESP_RETURN_ON_ERROR(esp_lcd_new_rgb_panel(&cfg, &s_panel), TAG,
                        "new panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "init");

    // Largest single draw is one title character: 24 x 28 px. Body line rows
    // are LCD_H_RES wide. Size for whichever is bigger.
    const size_t px =
        LCD_H_RES > CHAR_W(4) * CHAR_H(4) ? LCD_H_RES : CHAR_W(4) * CHAR_H(4);
    s_buf =
        (uint16_t*)heap_caps_malloc(px * sizeof(uint16_t), MALLOC_CAP_INTERNAL);
    ESP_RETURN_ON_FALSE(s_buf, ESP_ERR_NO_MEM, TAG, "draw buffer");

    BaseType_t ok = xTaskCreatePinnedToCore(display_task, "display", 4096,
                                            nullptr, 2, nullptr, 0);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_FAIL, TAG, "task");
    ESP_LOGI(TAG, "RGB panel up %dx%d", LCD_H_RES, LCD_V_RES);
    return ESP_OK;
}
