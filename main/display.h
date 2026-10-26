#pragma once
// Status screen for the Waveshare ESP32-S3-Touch-LCD-4.3 (800x480 RGB panel).
// Built-in 5x7 font, no LVGL.
#include "esp_err.h"

enum status_item_t { ST_BOOT = 0, ST_USB, ST_WEB, ST_COUNT };

// Call after hw_start() so the CH422G (backlight) is already initialised.
esp_err_t display_start(void);

// Thread-safe: only sets a flag; the display task redraws.
void display_set_status(status_item_t item, bool ok);
