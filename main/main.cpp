// app_main runs on Core 0: USB network + HTTP/LittleFS live here.
// Core 1 gets only the hardware task (hw_task.cpp).
#include "nvs_flash.h"
#include "esp_log.h"
#include "shared_state.h"
#include "usb_net.h"
#include "web_server.h"
#include "display.h"

extern "C" void app_main(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        e = nvs_flash_init();
    }
    ESP_ERROR_CHECK(e);
    hw_start();         // Core 1: I2C + CH422G (selects native USB), ADS1115, LEDC, capture

    // Status screen. A display failure is logged but does not stop the bench.
    if (display_start() != ESP_OK) ESP_LOGE("main", "display failed to start");
    display_set_status(ST_BOOT, true);

    usb_net_start();    // Core 0: TinyUSB NCM + netif + DHCP (aborts on failure)
    display_set_status(ST_USB, true);
    web_server_start(); // Core 0: LittleFS + httpd (aborts on failure)
    display_set_status(ST_WEB, true);
}
