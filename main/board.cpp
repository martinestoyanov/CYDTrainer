#include "board.h"
#include "esp_check.h"
static const char *TAG = "board";
static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_mode, s_out;
i2c_master_bus_handle_t board_i2c_bus(void)
{
    return s_bus;
}
esp_err_t board_io_write(uint8_t outputs)
{
    return i2c_master_transmit(s_out, &outputs, 1, 50);
}
esp_err_t board_io_init(i2c_master_bus_handle_t bus)
{
    s_bus = bus;
    // The CH422G answers on fixed addresses per register rather than using a
    // register pointer, so each register is its own I2C "device".
    i2c_device_config_t dc = {};
    dc.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dc.scl_speed_hz = 100000;
    dc.device_address = 0x24;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &dc, &s_mode), TAG, "add 0x24");
    dc.device_address = 0x38;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &dc, &s_out), TAG, "add 0x38");
    uint8_t mode = 0x01; // IO0-7 as outputs
    ESP_RETURN_ON_ERROR(i2c_master_transmit(s_mode, &mode, 1, 50), TAG, "CH422G not responding");
    return board_io_write(EXIO_CTP_RST | EXIO_LCD_RST | EXIO_SD_CS);
}
