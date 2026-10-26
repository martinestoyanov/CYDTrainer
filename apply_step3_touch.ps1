$ErrorActionPreference = 'Stop'
$path = Join-Path $PSScriptRoot 'main\display.cpp'
if (-not (Test-Path $path)) { throw "Cannot find $path. Put this script in the CYDTrainer project root." }
$backup = "$path.step2.bak"
Copy-Item $path $backup -Force
$text = [IO.File]::ReadAllText($path).Replace("`r`n", "`n")

function Replace-ExactlyOnce([string]$old, [string]$new, [string]$name) {
    $script:count = ([regex]::Matches($script:text, [regex]::Escape($old))).Count
    if ($script:count -ne 1) { throw "Patch anchor '$name' occurred $script:count times; display.cpp was not changed. Backup: $backup" }
    $script:text = $script:text.Replace($old, $new)
}

Replace-ExactlyOnce @'
static volatile bool s_status[ST_COUNT];
'@ @'
static volatile bool s_status[ST_COUNT];
static i2c_master_dev_handle_t s_touch;
static bool s_touch_ok;
static bool s_touch_down;
static uint16_t s_touch_x, s_touch_y;
static uint8_t s_touch_points;
'@ 'touch state'

Replace-ExactlyOnce @'
static void backlight_on(void)
{
'@ @'
// GT911 protocol: 16-bit register address, MSB first. The selected 7-bit
// address is 0x5D on this board (the Waveshare I2C map shows 0x5D).
static esp_err_t gt911_read(uint16_t reg, uint8_t *data, size_t len)
{
    uint8_t addr[2] = { (uint8_t)(reg >> 8), (uint8_t)reg };
    return i2c_master_transmit_receive(s_touch, addr, sizeof(addr), data, len, 50);
}

static esp_err_t gt911_write8(uint16_t reg, uint8_t value)
{
    uint8_t tx[3] = { (uint8_t)(reg >> 8), (uint8_t)reg, value };
    return i2c_master_transmit(s_touch, tx, sizeof(tx), 50);
}

static void gt911_start(void)
{
    i2c_master_bus_handle_t bus = board_i2c_bus();
    if (!bus) { ESP_LOGE(TAG, "touch: shared I2C bus unavailable"); return; }

    i2c_device_config_t dc = {};
    dc.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dc.device_address = 0x5D;
    dc.scl_speed_hz = 400000;
    if (i2c_master_bus_add_device(bus, &dc, &s_touch) != ESP_OK) {
        ESP_LOGE(TAG, "touch: could not add GT911 at 0x5D");
        return;
    }

    // GT911 initialization can take up to 200 ms after reset release.
    vTaskDelay(pdMS_TO_TICKS(250));
    uint8_t id[4] = {};
    if (gt911_read(0x8140, id, sizeof(id)) != ESP_OK) {
        ESP_LOGE(TAG, "touch: GT911 not responding at 0x5D");
        return;
    }
    s_touch_ok = true;
    ESP_LOGI(TAG, "touch: GT911 product ID %.4s", (char *)id);
}

static void gt911_poll(void)
{
    if (!s_touch_ok) return;
    uint8_t status = 0;
    if (gt911_read(0x814E, &status, 1) != ESP_OK) {
        s_touch_ok = false;
        s_touch_down = false;
        ESP_LOGE(TAG, "touch: read failed");
        return;
    }
    if (!(status & 0x80)) return;

    uint8_t n = status & 0x0F;
    s_touch_points = n <= 5 ? n : 0;
    if (s_touch_points) {
        uint8_t p[8] = {};
        if (gt911_read(0x8150, p, sizeof(p)) == ESP_OK) {
            s_touch_x = (uint16_t)p[1] | ((uint16_t)p[2] << 8);
            s_touch_y = (uint16_t)p[3] | ((uint16_t)p[4] << 8);
            s_touch_down = true;
        }
    } else {
        s_touch_down = false;
    }
    (void)gt911_write8(0x814E, 0);
}

static void backlight_on(void)
{
'@ 'GT911 functions'

Replace-ExactlyOnce @'
    snprintf(line, sizeof line, "UPTIME   %02u:%02u:%02u", (unsigned)(s / 3600),
             (unsigned)(s / 60 % 60), (unsigned)(s % 60));
    draw_line(y, line, TEXT_S, C_CYAN, C_BLACK);
}
'@ @'
    snprintf(line, sizeof line, "UPTIME   %02u:%02u:%02u", (unsigned)(s / 3600),
             (unsigned)(s / 60 % 60), (unsigned)(s % 60));
    draw_line(y, line, TEXT_S, C_CYAN, C_BLACK);
    y += step;
    if (!s_touch_ok) {
        draw_line(y, "TOUCH    NOT FOUND", TEXT_S, C_RED, C_BLACK);
    } else if (s_touch_down) {
        snprintf(line, sizeof line, "TOUCH    X %u  Y %u  PTS %u",
                 (unsigned)s_touch_x, (unsigned)s_touch_y, (unsigned)s_touch_points);
        draw_line(y, line, TEXT_S, C_GREEN, C_BLACK);
    } else {
        draw_line(y, "TOUCH    READY", TEXT_S, C_CYAN, C_BLACK);
    }
}
'@ 'touch display line'

Replace-ExactlyOnce @'
    bool beat = false;
    while (true) {
        draw_status();
        // Heartbeat square: alternates every 500 ms while the firmware is alive.
        fill_rect(LCD_H_RES - 70, LCD_V_RES - 70, 40, 40, beat ? C_AMBER : C_RED);
        beat = !beat;
        vTaskDelay(pdMS_TO_TICKS(500));
    }
'@ @'
    gt911_start();
    bool beat = false;
    int redraw_ticks = 0;
    while (true) {
        gt911_poll();
        // Poll touch every 20 ms; redraw text and heartbeat every 500 ms.
        if (++redraw_ticks >= 25) {
            redraw_ticks = 0;
            draw_status();
            fill_rect(LCD_H_RES - 70, LCD_V_RES - 70, 40, 40, beat ? C_AMBER : C_RED);
            beat = !beat;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
'@ 'display task loop'

[IO.File]::WriteAllText($path, $text, [Text.UTF8Encoding]::new($false))
Write-Host "Step 3 touch patch applied to $path"
Write-Host "Backup saved as $backup"
Write-Host "No CMakeLists.txt or sdkconfig files were changed."
