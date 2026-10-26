#include "nvs_flash.h"
#include "esp_log.h"
#include "shared_state.h"
#include "usb_net.h"
#include "web_server.h"
#include "display.h"
#include "trainer_wifi.h"

extern "C" void app_main(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        e = nvs_flash_init();
    }
    ESP_ERROR_CHECK(e);
    hw_start();
    if (display_start() != ESP_OK) ESP_LOGE("main", "display failed to start");

    // Creates the network stack and event loop once, shared with Wi-Fi.
    usb_net_start();
    display_set_status(ST_USB, true);
    web_server_start();
    display_set_status(ST_WEB, true);

    // A Wi-Fi startup error is logged without aborting USB/web operation.
    e = trainer_wifi_start();
    if (e != ESP_OK) ESP_LOGE("main", "Wi-Fi startup failed: %s", esp_err_to_name(e));
}
