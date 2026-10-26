#pragma once
#include <cstring>
#include "esp_check.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"

#include "trainer_network_config.h"

// Call ONLY after usb_net_start(): it initializes esp_netif and the default
// event loop. Do not initialize either again here.
static esp_err_t trainer_wifi_start()
{
#if TRAINER_WIFI_ENABLED
    static const char *tag = "trainer_wifi";
    static_assert(sizeof(TRAINER_WIFI_PASSWORD) >= 9 &&
                  sizeof(TRAINER_WIFI_PASSWORD) <= 64,
                  "Wi-Fi password must contain 8 to 63 characters");
    static_assert(sizeof(TRAINER_WIFI_SSID) <= 33, "SSID too long");

    esp_netif_t *ap = esp_netif_create_default_wifi_ap();
    ESP_RETURN_ON_FALSE(ap, ESP_ERR_NO_MEM, tag, "AP netif");

    // Separate subnet from USB 192.168.7.1. No bridging or internet routing.
    ESP_RETURN_ON_ERROR(esp_netif_dhcps_stop(ap), tag, "stop DHCP");
    esp_netif_ip_info_t ip = {};
    ESP_RETURN_ON_ERROR(esp_netif_str_to_ip4("192.168.4.1", &ip.ip), tag, "IP");
    ESP_RETURN_ON_ERROR(esp_netif_str_to_ip4("192.168.4.1", &ip.gw), tag, "gateway");
    ESP_RETURN_ON_ERROR(esp_netif_str_to_ip4("255.255.255.0", &ip.netmask), tag, "mask");
    ESP_RETURN_ON_ERROR(esp_netif_set_ip_info(ap, &ip), tag, "set IP");
    ESP_RETURN_ON_ERROR(esp_netif_dhcps_start(ap), tag, "start DHCP");

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init), tag, "init");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), tag, "RAM config");
    wifi_config_t config = {};
    std::memcpy(config.ap.ssid, TRAINER_WIFI_SSID, sizeof(TRAINER_WIFI_SSID) - 1);
    std::memcpy(config.ap.password, TRAINER_WIFI_PASSWORD,
                sizeof(TRAINER_WIFI_PASSWORD) - 1);
    config.ap.ssid_len = sizeof(TRAINER_WIFI_SSID) - 1;
    config.ap.channel = 1;
    config.ap.max_connection = 4;
    config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    config.ap.pmf_cfg.required = false;
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), tag, "AP mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &config), tag, "AP config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), tag, "start");
    ESP_LOGI(tag, "AP ready: %s at 192.168.4.1", TRAINER_WIFI_SSID);
#endif
    return ESP_OK;
}
