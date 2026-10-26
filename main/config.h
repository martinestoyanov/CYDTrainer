#pragma once

// Wiring (Waveshare ESP32-S3-LCD-4.3, from the board schematic):
//
//   I2C header H7 (VCC GND SDA SCL) - level-shifted by M1 to I2C_VCC; set solder jumper H8 to 5V
//     ADS1115 @0x4B (ADDR->SCL):  A0 Pitot MPX5100AP   A1 Static MPX5100AP
//                                 A2 PWM line from J2 "B" (the "Fluke" average)
//                                 A3 5V rail (ratiometric reference for the MPX sensors)
//
//   RS-485 header J2 (B A) - the 5 kHz PWM leaves on B. GPIO16 drives the SP3485's
//     DE/RE through transistor S1 with DI tied to GND: GPIO16 low -> driver on, B driven
//     high (~3.5-4 V); GPIO16 high -> driver off, B pulled low by R63 (10k to GND) against
//     the receiver input (~0.3-0.8 V). LEDC is inverted so B carries the true duty.
//     B also feeds the panel's red test jack and the 5V LED (anode). A is unconnected.
//     Leave the 120R termination jumper J1 open (NC).
//
//   Sensor AD header J6 (3V3 GND AD) - to the panel: the duty pot (3V3 and GND across it,
//     wiper on AD = GPIO6 = ADC1 channel 5); GND also serves the black jack and LED cathode.
//
//   USB-C "USB" port = native USB (IO19/20) through the FSUSB42 mux; CH422G EXIO5 must be
//   low to select it. USB-C "UART" port = CH343 -> UART0: flashing and logs.

// ---- Pins -------------------------------------------------------------
#define PIN_I2C_SDA        8
#define PIN_I2C_SCL        9
#define PIN_PWM_OUT        16          // RS-485 transmitter control (Waveshare's RS485 TX pin)
#define PIN_PWM_IN         PIN_PWM_OUT // capture reads back the same pad
#define POT_ADC_CHANNEL    ADC_CHANNEL_5 // GPIO6 on ADC1

// ---- ADS1115 ----------------------------------------------------------
#define ADS_ADDR           0x4B
#define I2C_SPEED_HZ       400000

// ---- MPX5100AP (absolute, 15-115 kPa): Vout = Vs * (0.009 * P - 0.095) -
#define MPX_VS_NOMINAL     5.0f   // used if the A3 rail reading is missing
#define PRESSURE_ALPHA     0.1f   // EMA per sample (~180 Hz) -> ~55 ms time constant

// ---- No ADS1115 fitted: simulated sensor values --------------------------
#define ADS_PROBE_MS       20
#define ADS_REPROBE_US     2000000 // look for the chip again every 2 s
#define ADS_MAX_FAILS      5       // failed loops in a row before it counts as lost
#define SIM_ALT_FT         0.0f
#define SIM_IAS_KT         30.0f

// ---- Levels J2 B swings between (measure yours, then tune) --------------
#define PWM_V_LOW          0.5f
#define PWM_V_HIGH         3.6f

// ---- Pot on J6 (ESP32 ADC, millivolts at the knob's ends) --------------
#define POT_MV_MIN         40
#define POT_MV_MAX         3100   // ADC1 at 12 dB reads up to about 3.1 V

// ---- PWM --------------------------------------------------------------
#define PWM_FREQ_HZ        5000
#define PWM_DUTY_BITS      12
#define PWM_WINDOW_US      100000 // capture averaging window
#define PWM_AVG_EVERY      4      // read the PWM average every Nth sensor loop

// ---- USB network ------------------------------------------------------
#define NET_IP             "192.168.7.1"
#define NET_MASK           "255.255.255.0"
#define NET_MOUNT          "/www"

// Potentiometer conditioning
#define POT_FILTER_ALPHA       0.08f
#define POT_DEADBAND_PCT       0.5f
#define POT_END_SNAP_PCT       1.0f