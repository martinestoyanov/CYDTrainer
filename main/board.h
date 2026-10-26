#pragma once
#include "driver/i2c_master.h"
// CH422G IO expander. Mode register at 0x24, output register at 0x38.
//   EXIO1 CTP_RST, EXIO2 backlight (DISP), EXIO3 LCD_RST, EXIO4 SD_CS,
//   EXIO5 USB_SEL (0 = IO19/20 to the USB port, 1 = to the CAN transceiver)
#define EXIO_CTP_RST   (1 << 1)
#define EXIO_BACKLIGHT (1 << 2)
#define EXIO_LCD_RST   (1 << 3)
#define EXIO_SD_CS     (1 << 4)
#define EXIO_USB_SEL   (1 << 5)
// Selects native USB, holds LCD/touch out of reset, SD deselected, backlight off.
// Also records the bus handle so other drivers (touch) can share GPIO8/9.
esp_err_t board_io_init(i2c_master_bus_handle_t bus);
esp_err_t board_io_write(uint8_t outputs);
// Shared I2C bus (GPIO8/9), created by hw_task on Core 1. Valid once hw_start()
// has returned; nullptr before that. Add devices with i2c_master_bus_add_device().
i2c_master_bus_handle_t board_i2c_bus(void);