# E175 Pitot-Static and PWM Test Bench: project handoff

Everything here was written without a toolchain: **the firmware has never been compiled or run on hardware.** Treat every API name and every estimated voltage as unverified until the first build and first power-up. Section 9 lists what to check first.

## 1. What it is

A hardwired test bench that:

1. Reads two NXP MPX5100AP absolute pressure sensors (pitot and static) through an ADS1115 and derives airspeed and pressure altitude.
2. Generates a 5 kHz PWM test signal whose duty comes from a pot or from the web UI, exposes it on a test-jack pair and an LED, and reads it back two ways: hardware edge capture (exact duty and frequency) and the ADS1115 (an averaged voltage, like a multimeter).
3. Serves a pre-gzipped WebGL/HTML5 EICAS page and a 10 Hz `/data` JSON endpoint to an iPad over **USB-C (TinyUSB network device)**. No Wi-Fi.

Hardware: **Waveshare ESP32-S3-LCD-4.3** (non-touch SKU 30493; ESP32-S3-WROOM-1-N16R8: 16 MB flash, 8 MB octal PSRAM). The local LCD is unused for now.

## 2. Hardware and wiring (final)

### Board jumpers (set first)

| Jumper | Setting | Why |
| --- | --- | --- |
| H8 (I2C voltage) | **5V** | I2C header VCC is the only 5V source on any header. Powers the ADS1115 and both MPX sensors. It ships on 3V3. |
| J1 (RS-485 120R termination) | **open (NC)** | Termination collapses the PWM line's swing. |

Warning: the board's "5V" rail is **not regulated**. It is USB VBUS through a Schottky diode (D9), so expect about 4.6-4.7 V. The MPX output is ratiometric to its supply, so the firmware measures the rail on ADS1115 A3 and corrects every pressure reading with it.

### ESP32-S3 pins used

| GPIO | Function | Reaches outside via |
| --- | --- | --- |
| 8 / 9 | I2C SDA / SCL (shared with CH422G and touch) | H7, level-shifted to 5V |
| 16 | PWM output, **inverted**, 5 kHz. Also read back by MCPWM capture | RS-485 transceiver, appears on J2 **B** |
| 6 | Pot wiper (ESP32 ADC1 channel 5) | J6 AD |
| 19 / 20 | Native USB (TinyUSB) | USB-C "USB" port, via FSUSB42 mux |
| 43 / 44 | UART0: logs and flashing (CH343) | USB-C "UART" port |
| CH422G (I2C 0x24 mode, 0x38 outputs) | EXIO5 held **low** to route GPIO19/20 to USB, not CAN | internal |

Every other GPIO is taken by the RGB LCD, SD card, RS-485, PSRAM or touch. The only free GPIOs are IO11/12/13 (microSD slot), which this design deliberately does not use (no extra parts). CAN is unusable while USB networking runs (shared pins).

### How the PWM gets out (RS-485 trick)

GPIO16 is the transceiver's TX control. On this board it switches the SP3485's DE/RE through transistor S1 with DI tied to GND:

- GPIO16 **low**: driver on, **B driven high** (about 3.5-4 V), A driven low.
- GPIO16 **high**: driver off, B weakly pulled low (R63 10k to GND, against the receiver input, about 0.3-0.8 V), A weakly pulled high.

LEDC is configured with `output_invert`, so J2 **B** carries the true duty. B is driven hard when high, so a 5V LED with built-in resistor can sit on it without distorting the signal. A is left unconnected. These levels are estimates from the schematic.

### Cables

| Cable | Pin | Goes to |
| --- | --- | --- |
| **H7** (I2C header, 4 leads) | VCC | sensor-board 5V bus |
| | GND | sensor-board GND bus |
| | SDA / SCL | ADS1115 SDA / SCL. SDA/SCL silkscreen order is swapped relative to ADS1115 convention, so wire by label |
| **J2** (RS-485, 2 leads) | B | panel red jack + LED anode + ADS1115 A2 |
| | A | not connected (insulate) |
| **J6** (Sensor AD, 3 leads) | 3V3 | pot one end |
| | AD (GPIO6) | pot wiper |
| | GND | pot other end, panel black jack, LED cathode |

### Sensor board (hand-wired, off-board from the Waveshare PCB)

- **ADS1115** at **0x4B** (ADDR wired to SCL): VDD=5V, GND, SCL, SDA, ALRT unused, **A0** pitot MPX pin 1, **A1** static MPX pin 1, **A2** J2 B (PWM line), **A3** 5V rail.
- **MPX5100AP x2**: pin 1 (notched) Vout, 2 GND, 3 Vs=5V, **4-6 factory trim pins, leave unconnected**. Mounted lying flat with leads bent 90 degrees at 3.5 mm and M3 screws through the ears (drill 3.2 mm).
- The board connects to the Waveshare board only through the H7 cable and one wire from J2 B. The pot is not on it.
- No external components of any kind (no pull-ups, caps or resistors). Do not add I2C pull-ups; the board and the ADS1115 module have them.
- The PWM average is measured with the ADS1115 at 128 SPS (7.8 ms = about 39 PWM cycles), so its delta-sigma filter averages the raw square wave. No RC filter.

Panel: 10 kOhm pot (3V3 / wiper / GND from J6), 5V LED (anode J2 B, cathode GND), red jack on J2 B, black jack on GND. Fluke DC volts on the jacks reads the averaged PWM (`V_low + duty*(V_high - V_low)`, about 0.5 V at 0%), and a Fluke duty-% function reads the duty directly.

## 3. Software architecture (ESP-IDF 5.2+, C++)

FreeRTOS dual-core split:

- **Core 0**: TinyUSB device task, lwIP, esp_http_server (pinned), LittleFS. `app_main` runs here.
- **Core 1**: one task, `hw_core1`, priority `configMAX_PRIORITIES - 2`. It owns the I2C bus, the ADS1115, the pot ADC, LEDC and the MCPWM capture ISR. All of these are *initialised inside that task* so their interrupts land on Core 1. It yields with `vTaskDelay(1)` each loop (needs `CONFIG_FREERTOS_HZ=1000`).
- Core 1 publishes a `Telemetry` struct under a `portMUX` spinlock; Core 0 reads a copy. Control from Core 0 goes through `std::atomic` values (manual duty, zero offset).

Startup order matters: `hw_start()` blocks until Core 1 has initialised the I2C bus and CH422G (USB selected), and only then does `usb_net_start()` run.

## 4. File-by-file spec

```
CMakeLists.txt              project(e175_testbench)
partitions.csv              nvs 0x6000, phy_init, factory 3 MB, storage (littlefs) 4 MB
sdkconfig.defaults          see section 5
main/CMakeLists.txt         SRCS main, usb_net, web_server, hw_task, board;
                            littlefs_create_partition_image(storage ../data FLASH_IN_PROJECT)
main/idf_component.yml      idf>=5.2, espressif/esp_tinyusb ~1.7.0, joltwallet/littlefs ^1.14.8
main/config.h               pins, addresses, constants (the only file to touch for rewiring)
main/shared_state.h         Telemetry struct, hw_start(), telemetry_get(), pwm_set_manual(),
                            pwm_follow_pot(), hw_zero_qc()
main/board.h/.cpp           CH422G: mode reg 0x24 <- 0x01 (outputs); output reg 0x38 <- bit mask.
                            EXIO1 CTP_RST, EXIO2 backlight, EXIO3 LCD_RST, EXIO4 SD_CS, EXIO5 USB_SEL.
                            Init writes CTP_RST|LCD_RST|SD_CS high, EXIO5 low, backlight off.
main/hw_task.cpp            the Core 1 task (section 6)
main/usb_net.h/.cpp         TinyUSB NCM + esp_netif + DHCP server (section 7)
main/web_server.h/.cpp      LittleFS mount at /www; static handler; /data, /pwm, /zero
main/main.cpp               nvs_flash_init; hw_start(); usb_net_start(); web_server_start()
data/                       web files, pre-gzipped (index.html.gz is a placeholder that polls /data)
```

## 5. sdkconfig.defaults

```
CONFIG_IDF_TARGET="esp32s3"
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_FREERTOS_HZ=1000
CONFIG_ESP_CONSOLE_UART_DEFAULT=y        # native USB belongs to TinyUSB
CONFIG_ESP_CONSOLE_SECONDARY_NONE=y
CONFIG_SPIRAM=y  CONFIG_SPIRAM_MODE_OCT=y  CONFIG_SPIRAM_SPEED_80M=y
CONFIG_TINYUSB_NET_MODE_NCM=y            # fallback: ECM (iPadOS has no RNDIS)
CONFIG_TINYUSB_CDC_ENABLED=n
CONFIG_TINYUSB_TASK_AFFINITY_CPU0=y
CONFIG_LWIP_TCPIP_TASK_AFFINITY_CPU0=y
```

## 6. Core 1 task behaviour (hw_task.cpp)

1. Create I2C master bus (GPIO8/9, internal pull-ups on, 400 kHz). `board_io_init()`; release the startup semaphore.
2. Add the ADS1115 device at `ADS_ADDR` (0x4B). Init the pot ADC (ADC1 ch5, 12 dB, curve-fitting calibration).
3. Init MCPWM capture on `PIN_PWM_IN` (= GPIO16): both edges, 80 MHz capture timer, ISR accumulates `sum_period`, `sum_high`, `cycles` under a spinlock. **Capture before LEDC**, then re-enable the pad's input buffer (`PIN_INPUT_ENABLE`) because LEDC leaves it output-only.
4. LEDC: 5 kHz, 12-bit, **`flags.output_invert = 1`** on GPIO16. Duty counts are `pct * 4096 / 100` (full scale 2^12 so 100% is solid).
5. Loop:
   - If the ADS1115 is present: single-shot reads of A0 (pitot), A1 (static), A3 (rail) at 860 SPS (busy-wait 1.1 ms, then poll the OS bit), and A2 (PWM line) at **128 SPS every 4th loop**. Config word: `0x8000 | ((0x4|ch)<<12) | 0x0100 | (dr<<5) | 0x0003`, range +/-6.144 V (LSB 6.144/32768 V).
   - MPX5100AP absolute transfer function: `Vout = Vs*(0.009*P - 0.095)`, so `P = (Vout/Vs + 0.095)/0.009` kPa, with `Vs` = measured A3 (fall back to 5.0 V if A3 < 4 V). Smooth with an EMA, alpha 0.1.
   - `qc = pitot - static - zero` (zero from NVS blob `bench/qc_zero`, set by `/zero`).
   - IAS (kt) = `661.4788 * sqrt(5*((qc/101.325 + 1)^(2/7) - 1))`. Pressure altitude (ft) = `145366.45*(1 - (P/101.325)^0.190284)`.
   - Pot: `pct = (mV - POT_MV_MIN)/(POT_MV_MAX - POT_MV_MIN)`, clamped 0-100. A manual `/pwm?duty=` overrides it; `/pwm?src=pot` returns to the pot. Only re-apply LEDC when the target moves by 0.05%.
   - Every 100 ms, snapshot-and-reset the capture sums. `freq = cap_hz*cycles/sum_period`. **Because the pad is inverted relative to J2 B, `duty = 100 - 100*sum_high/sum_period`.** With no edges, a stuck-high pad reports 0% and a stuck-low pad 100%.
   - Publish Telemetry; `vTaskDelay(1)`.

### No-ADS1115 mode (bench not wired yet)

The ADS1115 is probed at start (`i2c_master_probe`, 20 ms) and re-probed every 2 s while absent. It is declared lost after 5 consecutive failed loops. While it is absent:

| Value | Simulated as |
| --- | --- |
| Altitude | **0 ft** (`SIM_ALT_FT`); static pressure = 101.325 kPa |
| Airspeed | **30 kt** (`SIM_IAS_KT`); `qc` computed by inverting the IAS formula |
| Pitot kPa | static + that `qc` |
| `pitot_v`, `static_v`, `rail_v` | back-computed from the pressures at 5.0 V |
| `fluke_v` (the PWM readback) | **calculated**: `PWM_V_LOW + duty/100*(PWM_V_HIGH - PWM_V_LOW)` using the capture-measured duty (0.5 V and 3.6 V by default) |

The duty and frequency readback stay *measured* because they come from the ESP32's capture hardware, not the ADS1115. `/data` carries `sensors_sim` and `pwm_avg_sim` so the UI can show a banner. `/zero` is a no-op while simulated, so simulated values never end up stored as the zero offset. If the chip appears later (hot-plugged) it switches to real readings on the next probe.

## 7. Networking (usb_net.cpp)

- `tinyusb_driver_install()` with default config, then `tinyusb_net_init(TINYUSB_USBDEV_0, &cfg)` with NCM, two distinct locally-administered MACs (device and host), and an `on_recv_callback` that **copies** the frame and calls `esp_netif_receive()`.
- `esp_netif` with a custom driver: `transmit` calls `tinyusb_net_send_sync()`; `driver_free_rx_buffer` frees the copy. Static IP **192.168.7.1/24**, flags `ESP_NETIF_DHCP_SERVER | ESP_NETIF_FLAG_AUTOUP`. Start with `esp_netif_action_start()`.
- DHCP is restarted with **no router option** (`ESP_NETIF_ROUTER_SOLICITATION_ADDRESS` = 0), otherwise iPadOS routes all traffic over the bench link and loses internet.
- Plain HTTP on 192.168.7.1, so it is not a secure context: no service worker. Add-to-Home-Screen still works as a standalone web clip.

## 8. Web server and API (web_server.cpp)

- Wildcard handler serves `/www<uri>.gz` with `Content-Encoding: gzip` if it exists, else the raw file; `index.html` default; rejects `..`; 4 KB chunks; MIME by extension. `core_id=0`, 8 KB stack, wildcard URI matching, handlers registered `/data`, `/pwm`, `/zero`, then `/*` last.
- `GET /data` JSON, `Cache-Control: no-store`: `seq, t, pitot_v, static_v, pot_v, rail_v, fluke_v, pitot_kpa, static_kpa, qc_kpa, ias_kt, alt_ft, pwm_hz, pwm_duty, pwm_set, pwm_manual, adc_sensors_ok, adc_fluke_ok, sensors_sim, pwm_avg_sim`.
- `GET /pwm?duty=0..100` manual duty; `GET /pwm?src=pot` follow the pot.
- `GET /zero` (both ports open to ambient) stores the pitot-static offset in NVS. Needed because absolute MPX5100AP parts are only accurate to about +/-2.5 kPa each.

## 9. Build, flash, and first checks

```
idf.py set-target esp32s3
idf.py build
idf.py -p COMx flash monitor     # COMx = the "UART" USB-C port
```

Then plug the **"USB" port** into the iPad. Gzip the EICAS files into `data/` first (`gzip -9n file`).

Verify in this order (each is a known risk):

1. **It compiles.** esp_tinyusb is pinned to ~1.7: its net API (`tinyusb_net_init`, `tinyusb_net_send_sync`, config field names, `CONFIG_TINYUSB_NET_MODE_NCM`) changed in 2.x. Fix signatures against the installed version.
2. **Console shows "ADS1115 ... not found, simulating sensors"** with the I2C parts unwired, and `/data` returns `alt_ft` 0, `ias_kt` 30, `sensors_sim: true`.
3. **iPadOS accepts the USB NCM adapter** and gets 192.168.7.x by DHCP. This is the biggest open risk. Fallback: switch to ECM, or add a Wi-Fi SoftAP fallback serving the same page (planned, not written).
4. **PWM line**: Fluke on J2 B to GND with `/pwm?duty=0` then `100` gives the real `V_low`/`V_high`. Put them in `PWM_V_LOW`/`PWM_V_HIGH`. At 50% the LED is half bright and a duty-% meter reads about 50%. Confirm `pwm_duty` agrees (this checks the inversion).
5. **With the ADS1115 wired**: `adc_sensors_ok: true`, `rail_v` about 4.6-4.7, pressures near local ambient (about 101 kPa), then `/zero`. Check the ADS1115 module's pin order is `VDD GND SCL SDA ADDR ALRT A0 A1 A2 A3`.
6. **Confirm the board matches the schematic**: CH422G answers at 0x24/0x38; H8 reads 5V at H7 VCC; J2 B levels as above.

## 10. Key decisions and why (so they are not undone by mistake)

- **USB networking, not Wi-Fi**: zero-latency, powers the board from the iPad, no captive portal. iPadOS has no RNDIS, hence NCM/ECM.
- **ADS1115 for everything analog except the pot**: the ESP32 ADC is only about +/-2%. The pot can use it because duty precision does not matter.
- **ADS1115 address 0x4B (ADDR to SCL)** was chosen to simplify the hand wiring; changing it means changing `ADS_ADDR` and the wiring together.
- **PWM on RS-485, inverted, output on B** because GPIO16 is the only free output with a header, B is the driven-high side (so an LED does not distort it), and the 3.3 V Sensor AD pin (GPIO6) is needed for the pot.
- **No Wi-Fi coexistence concerns** for the ADC: the pot is on ADC1.
- **Pressure sensors are absolute** (MPX5100AP), not differential. Airspeed from `pitot - static` needs `/zero`. A differential sensor (e.g. MPXV7002DP) would be far better below about 60 kt.
- Unregulated 5V rail: always correct pressure with the measured rail on A3. Do not assume 5.00 V.

## 11. Source references

- Waveshare ESP32-S3-Touch-LCD-4.3 schematic and Demo.zip (RS-485 example: TX=16, RX=15; CH422G addresses 0x24/0x38).
- NXP MPX5100 datasheet (transfer function for the AP part, case 98ASB42796B / 867B-04), SP3485 and SMAJ12CA datasheets.

## 12. Not done yet

- Compile and bring-up (section 9).
- Pot smoothing and a small dead band (a longer pot cable picks up noise and can make the duty jitter).
- Wi-Fi SoftAP fallback if iPadOS refuses the USB network.
- The EICAS front end itself (`data/` holds only a placeholder that polls `/data`).
- The local 4.3" display UI (reserved for later; the I2C bus handle created in `hw_task.cpp` must be shared with any touch/LCD driver, since GPIO8/9 are the same bus).
