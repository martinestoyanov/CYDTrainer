#include <stdlib.h>
#include <string.h>
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "tinyusb.h"
#include "tinyusb_net.h"
#include "config.h"
#include "usb_net.h"

static const char *TAG = "usb_net";
static esp_netif_t *s_netif;

// netif -> USB
static esp_err_t netif_transmit(void *, void *buffer, size_t len)
{
    return tinyusb_net_send_sync(buffer, len, nullptr, pdMS_TO_TICKS(100));
}

static void netif_free_rx(void *, void *buffer)
{
    free(buffer);
}

// USB -> netif. TinyUSB owns `buffer`, so hand lwIP a copy.
static esp_err_t usb_recv(void *buffer, uint16_t len, void *)
{
    if (!s_netif) return ESP_OK;
    void *copy = malloc(len);
    if (!copy) return ESP_ERR_NO_MEM;
    memcpy(copy, buffer, len);
    esp_err_t e = esp_netif_receive(s_netif, copy, len, nullptr);
    if (e != ESP_OK) free(copy);
    return e;
}

void usb_net_start()
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Device-side MAC, then the (different) MAC the host sees on its adapter.
    uint8_t dev_mac[6] = {0x02, 0x00, 0x00, 0x17, 0x50, 0x01};
    uint8_t host_mac[6] = {0x02, 0x00, 0x00, 0x17, 0x50, 0x02};

    esp_netif_ip_info_t ip = {};
    ESP_ERROR_CHECK(esp_netif_str_to_ip4(NET_IP, &ip.ip));
    ESP_ERROR_CHECK(esp_netif_str_to_ip4(NET_MASK, &ip.netmask));
    ip.gw = ip.ip;

    esp_netif_inherent_config_t base = {};
    base.flags = (esp_netif_flags_t)(ESP_NETIF_DHCP_SERVER | ESP_NETIF_FLAG_AUTOUP);
    base.ip_info = &ip;
    base.if_key = "usb_ncm";
    base.if_desc = "usb ncm";
    base.route_prio = 10;

    static esp_netif_driver_ifconfig_t drv = {};
    drv.handle = (void *)1;
    drv.transmit = netif_transmit;
    drv.driver_free_rx_buffer = netif_free_rx;

    esp_netif_config_t cfg = {};
    cfg.base = &base;
    cfg.driver = &drv;
    cfg.stack = ESP_NETIF_NETSTACK_DEFAULT_ETH;
    s_netif = esp_netif_new(&cfg);
    ESP_ERROR_CHECK(esp_netif_set_mac(s_netif, dev_mac));

    tinyusb_config_t tusb = {};
    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb));

    tinyusb_net_config_t net = {};
    memcpy(net.mac_addr, host_mac, 6);
    net.on_recv_callback = usb_recv;
    ESP_ERROR_CHECK(tinyusb_net_init(TINYUSB_USBDEV_0, &net));

    // Starts the netif (AUTOUP) and, via the flag above, the DHCP server.
    esp_netif_action_start(s_netif, nullptr, 0, nullptr);

    // Offer no router or DNS. Otherwise iPadOS prefers this wired link as its default
    // route and loses internet over Wi-Fi/cellular while the bench is plugged in.
    uint8_t offer = 0;
    esp_netif_dhcps_stop(s_netif);
    esp_netif_dhcps_option(s_netif, ESP_NETIF_OP_SET, ESP_NETIF_ROUTER_SOLICITATION_ADDRESS, &offer, sizeof offer);
    ESP_ERROR_CHECK(esp_netif_dhcps_start(s_netif));
    ESP_LOGI(TAG, "USB NCM up, %s", NET_IP);
}
