// Core 1: ADS1115 polling, pot ADC, 5 kHz LEDC output, MCPWM edge capture.
// Everything (including the I2C, ADC and capture ISRs) is initialised from the
// Core 1 task so those interrupts are allocated on Core 1.
#include <atomic>
#include <math.h>
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "driver/mcpwm_cap.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "soc/gpio_periph.h"
#include "soc/io_mux_reg.h"
#include "board.h"
#include "config.h"
#include "shared_state.h"

static const char *TAG = "hw";

static portMUX_TYPE s_telem_mux = portMUX_INITIALIZER_UNLOCKED;
static Telemetry s_telem;
static std::atomic<float> s_manual_duty{-1.0f}; // < 0: follow the pot
static std::atomic<float> s_qc_zero{0.0f};
static SemaphoreHandle_t s_ready;

Telemetry telemetry_get()
{
    Telemetry t;
    portENTER_CRITICAL(&s_telem_mux);
    t = s_telem;
    portEXIT_CRITICAL(&s_telem_mux);
    return t;
}

void pwm_set_manual(float pct)
{
    s_manual_duty.store(fminf(fmaxf(pct, 0.0f), 100.0f));
}

void pwm_follow_pot()
{
    s_manual_duty.store(-1.0f);
}

// Called from the HTTP task (Core 0), so the NVS flash write stays off Core 1.
float hw_zero_qc()
{
    Telemetry t = telemetry_get();
    if (t.sensors_sim) return 0; // simulated values must never be stored as a zero offset
    float z = t.pitot_kpa - t.static_kpa;
    s_qc_zero.store(z);
    nvs_handle_t h;
    if (nvs_open("bench", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, "qc_zero", &z, sizeof z);
        nvs_commit(h);
        nvs_close(h);
    }
    return z;
}

static void load_qc_zero()
{
    nvs_handle_t h;
    float z;
    size_t len = sizeof z;
    if (nvs_open("bench", NVS_READONLY, &h) != ESP_OK) return;
    if (nvs_get_blob(h, "qc_zero", &z, &len) == ESP_OK) s_qc_zero.store(z);
    nvs_close(h);
}

// ---- ADS1115 ----------------------------------------------------------
// +/-6.144 V range, single-shot, comparator off.
static constexpr float ADS_LSB_V = 6.144f / 32768.0f;
static constexpr uint16_t ADS_DR_128SPS = 0x4, ADS_DR_860SPS = 0x7;

static esp_err_t ads_write_cfg(i2c_master_dev_handle_t dev, uint16_t cfg)
{
    uint8_t buf[3] = {0x01, (uint8_t)(cfg >> 8), (uint8_t)cfg};
    return i2c_master_transmit(dev, buf, sizeof buf, 10);
}

static esp_err_t ads_read_reg(i2c_master_dev_handle_t dev, uint8_t reg, uint16_t *out)
{
    uint8_t rx[2];
    esp_err_t e = i2c_master_transmit_receive(dev, &reg, 1, rx, 2, 10);
    if (e == ESP_OK) *out = (rx[0] << 8) | rx[1];
    return e;
}

// Start a conversion, wait the minimum time for its data rate (the internal clock is
// +/-10%), then poll the OS bit until it's done.
//   860 SPS: nominal 1.16 ms, busy-wait.
//   128 SPS: nominal 7.8 ms = 39 cycles of the 5 kHz PWM, so the ADC's own filter
//            averages the raw square wave like a DMM's integrating ADC. Yields while waiting.
static esp_err_t ads_single(i2c_master_dev_handle_t dev, int ch, uint16_t dr, float *v)
{
    uint16_t cfg = 0x8000 | ((0x4 | ch) << 12) | 0x0100 | (dr << 5) | 0x0003;
    esp_err_t e = ads_write_cfg(dev, cfg);
    if (e != ESP_OK) return e;
    if (dr == ADS_DR_860SPS) esp_rom_delay_us(1100);
    else vTaskDelay(pdMS_TO_TICKS(7));
    for (int i = 0; i < 60; i++) {
        uint16_t st;
        if ((e = ads_read_reg(dev, 0x01, &st)) != ESP_OK) return e;
        if (st & 0x8000) {
            uint16_t raw;
            if ((e = ads_read_reg(dev, 0x00, &raw)) != ESP_OK) return e;
            *v = (int16_t)raw * ADS_LSB_V;
            return ESP_OK;
        }
        esp_rom_delay_us(50);
    }
    return ESP_ERR_TIMEOUT;
}

// MPX5100AP absolute: Vout = Vs * (0.009 * P - 0.095)
static float mpx_kpa(float v, float vs)
{
    return (v / vs + 0.095f) / 0.009f;
}

// Calibrated airspeed (kt) from impact pressure, subsonic ISA.
static float ias_kt(float qc_kpa)
{
    if (qc_kpa <= 0) return 0;
    return 661.4788f * sqrtf(5.0f * (powf(qc_kpa / 101.325f + 1.0f, 2.0f / 7.0f) - 1.0f));
}

static float pressure_alt_ft(float p_kpa)
{
    return 145366.45f * (1.0f - powf(p_kpa / 101.325f, 0.190284f));
}

// Inverses of the two above, used to build the simulated sensor values.
static float pressure_for_alt(float alt_ft)
{
    return 101.325f * powf(1.0f - alt_ft / 145366.45f, 1.0f / 0.190284f);
}

static float qc_for_ias(float kt)
{
    float m = kt / 661.4788f;
    return 101.325f * (powf(1.0f + 0.2f * m * m, 3.5f) - 1.0f);
}

// ---- Pot on J6 (ESP32 ADC1) -------------------------------------------
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;

static void pot_init()
{
    adc_oneshot_unit_init_cfg_t uc = {};
    uc.unit_id = ADC_UNIT_1;
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&uc, &s_adc));
    adc_oneshot_chan_cfg_t cc = {};
    cc.atten = ADC_ATTEN_DB_12;
    cc.bitwidth = ADC_BITWIDTH_DEFAULT;
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc, POT_ADC_CHANNEL, &cc));
    adc_cali_curve_fitting_config_t cal = {};
    cal.unit_id = ADC_UNIT_1;
    cal.chan = POT_ADC_CHANNEL;
    cal.atten = ADC_ATTEN_DB_12;
    cal.bitwidth = ADC_BITWIDTH_DEFAULT;
    if (adc_cali_create_scheme_curve_fitting(&cal, &s_cali) != ESP_OK) s_cali = nullptr;
}

static bool pot_read_mv(int *mv)
{
    int raw;
    if (adc_oneshot_read(s_adc, POT_ADC_CHANNEL, &raw) != ESP_OK) return false;
    if (s_cali) return adc_cali_raw_to_voltage(s_cali, raw, mv) == ESP_OK;
    *mv = raw * 3100 / 4095;
    return true;
}

// ---- MCPWM capture ----------------------------------------------------
static portMUX_TYPE s_cap_mux = portMUX_INITIALIZER_UNLOCKED;
// Accessed only while s_cap_mux is held, so volatile is unnecessary and causes
// C++20 deprecation warnings for arithmetic/assignment on volatile objects.
static uint64_t s_sum_period, s_sum_high;
static uint32_t s_cycles;
static uint32_t s_last_rise, s_high_pending;
static bool s_have_rise, s_have_high;

static bool IRAM_ATTR on_capture(mcpwm_cap_channel_handle_t, const mcpwm_capture_event_data_t *e, void *)
{
    uint32_t v = e->cap_value;
    portENTER_CRITICAL_ISR(&s_cap_mux);
    if (e->cap_edge == MCPWM_CAP_EDGE_POS) {
        if (s_have_rise && s_have_high) {
            s_sum_period += (uint32_t)(v - s_last_rise);
            s_sum_high += s_high_pending;
            s_cycles++;
        }
        s_last_rise = v;
        s_have_rise = true;
        s_have_high = false;
    } else if (s_have_rise) {
        s_high_pending = v - s_last_rise;
        s_have_high = true;
    }
    portEXIT_CRITICAL_ISR(&s_cap_mux);
    return false;
}

static uint32_t capture_init()
{
    mcpwm_cap_timer_handle_t timer;
    mcpwm_capture_timer_config_t tc = {};
    tc.group_id = 0;
    tc.clk_src = MCPWM_CAPTURE_CLK_SRC_DEFAULT;
    ESP_ERROR_CHECK(mcpwm_new_capture_timer(&tc, &timer));

    mcpwm_cap_channel_handle_t ch;
    mcpwm_capture_channel_config_t cc = {};
    cc.gpio_num = PIN_PWM_IN;
    cc.prescale = 1;
    cc.flags.pos_edge = 1;
    cc.flags.neg_edge = 1;
    ESP_ERROR_CHECK(mcpwm_new_capture_channel(timer, &cc, &ch));

    mcpwm_capture_event_callbacks_t cbs = {};
    cbs.on_cap = on_capture;
    ESP_ERROR_CHECK(mcpwm_capture_channel_register_event_callbacks(ch, &cbs, nullptr));
    ESP_ERROR_CHECK(mcpwm_capture_channel_enable(ch));
    ESP_ERROR_CHECK(mcpwm_capture_timer_enable(timer));
    ESP_ERROR_CHECK(mcpwm_capture_timer_start(timer));

    uint32_t res = 0;
    ESP_ERROR_CHECK(mcpwm_capture_timer_get_resolution(timer, &res));
    ESP_LOGI(TAG, "capture timer %u Hz on GPIO%d", (unsigned)res, PIN_PWM_IN);
    return res;
}

// ---- LEDC -------------------------------------------------------------
static void pwm_init()
{
    ledc_timer_config_t t = {};
    t.speed_mode = LEDC_LOW_SPEED_MODE;
    t.duty_resolution = (ledc_timer_bit_t)PWM_DUTY_BITS;
    t.timer_num = LEDC_TIMER_0;
    t.freq_hz = PWM_FREQ_HZ;
    t.clk_cfg = LEDC_AUTO_CLK;
    ESP_ERROR_CHECK(ledc_timer_config(&t));

    ledc_channel_config_t c = {};
    c.gpio_num = PIN_PWM_OUT;
    c.speed_mode = LEDC_LOW_SPEED_MODE;
    c.channel = LEDC_CHANNEL_0;
    c.timer_sel = LEDC_TIMER_0;
    // Inverted: GPIO16 low switches the RS-485 driver on, which drives J2 B high.
    // So B carries the true duty, with its strong edge on the high state.
    c.flags.output_invert = 1;
    ESP_ERROR_CHECK(ledc_channel_config(&c));
}

static void pwm_apply(float pct)
{
    // Full scale is 2^bits (not 2^bits - 1) so 100% is a solid high.
    uint32_t duty = (uint32_t)(pct * (1u << PWM_DUTY_BITS) / 100.0f + 0.5f);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

// ---- Task -------------------------------------------------------------
enum { CH_PITOT, CH_STATIC, CH_PWM, CH_RAIL };

static void hw_task(void *)
{
    i2c_master_bus_handle_t bus;
    i2c_master_bus_config_t bc = {};
    bc.i2c_port = -1;
    bc.sda_io_num = (gpio_num_t)PIN_I2C_SDA;
    bc.scl_io_num = (gpio_num_t)PIN_I2C_SCL;
    bc.clk_source = I2C_CLK_SRC_DEFAULT;
    bc.glitch_ignore_cnt = 7;
    bc.flags.enable_internal_pullup = true;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bc, &bus));

    if (board_io_init(bus) != ESP_OK)
        ESP_LOGE(TAG, "CH422G init failed: USB may still be routed to CAN");
    xSemaphoreGive(s_ready);

    i2c_master_dev_handle_t adc;
    i2c_device_config_t dc = {};
    dc.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dc.scl_speed_hz = I2C_SPEED_HZ;
    dc.device_address = ADS_ADDR;
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &dc, &adc));

    pot_init();

    // Capture first, then LEDC. LEDC leaves the pad output-only, so when both share
    // a pin, re-enable its input buffer; the capture's GPIO-matrix input stays routed.
    uint32_t cap_hz = capture_init();
    pwm_init();
    if (PIN_PWM_IN == PIN_PWM_OUT) PIN_INPUT_ENABLE(GPIO_PIN_MUX_REG[PIN_PWM_IN]);
    float duty_set = 0;
    pwm_apply(duty_set);

    Telemetry t = {};
    float raw[4] = {};
    bool first = true, pwm_ok = false;
    uint32_t loop = 0;
    int64_t win_start = esp_timer_get_time();

    // The ADS1115 may be absent (bench not wired yet) or drop out. While it's missing the
    // sensor values are simulated and flagged, and it's probed again every 2 s.
    bool present = i2c_master_probe(bus, ADS_ADDR, ADS_PROBE_MS) == ESP_OK;
    int fails = 0;
    int64_t next_probe = esp_timer_get_time() + ADS_REPROBE_US;
    ESP_LOGI(TAG, "ADS1115 at 0x%02X: %s", ADS_ADDR, present ? "found" : "not found, simulating sensors");

    // Persistent pot filter state. -1 means "not primed yet"; the first valid
    // ADC sample initializes the filter without a startup ramp.
    float pot_filtered_mv = -1.0f;

    for (;;) {
        bool ok = false;
        if (!present && esp_timer_get_time() >= next_probe) {
            next_probe = esp_timer_get_time() + ADS_REPROBE_US;
            if (i2c_master_probe(bus, ADS_ADDR, ADS_PROBE_MS) == ESP_OK) {
                present = true; first = true; fails = 0;
                ESP_LOGI(TAG, "ADS1115 found");
            }
        }
        if (present) {
            ok = true;
            ok &= ads_single(adc, CH_PITOT, ADS_DR_860SPS, &raw[CH_PITOT]) == ESP_OK;
            ok &= ads_single(adc, CH_STATIC, ADS_DR_860SPS, &raw[CH_STATIC]) == ESP_OK;
            ok &= ads_single(adc, CH_RAIL, ADS_DR_860SPS, &raw[CH_RAIL]) == ESP_OK;
            if (++loop % PWM_AVG_EVERY == 0)
                pwm_ok = ads_single(adc, CH_PWM, ADS_DR_128SPS, &raw[CH_PWM]) == ESP_OK;
            fails = ok ? 0 : fails + 1;
            if (fails >= ADS_MAX_FAILS) { present = false; pwm_ok = false; ESP_LOGW(TAG, "ADS1115 lost, simulating sensors"); }
        }

        int pot_mv = 0;
        bool pot_ok = pot_read_mv(&pot_mv);

        if (present) {
            // MPX outputs are ratiometric to the (unregulated) 5V rail.
            float vs = raw[CH_RAIL] > 4.0f ? raw[CH_RAIL] : MPX_VS_NOMINAL;
            if (ok) {
                float p = mpx_kpa(raw[CH_PITOT], vs), s = mpx_kpa(raw[CH_STATIC], vs);
                t.pitot_kpa = first ? p : t.pitot_kpa + PRESSURE_ALPHA * (p - t.pitot_kpa);
                t.static_kpa = first ? s : t.static_kpa + PRESSURE_ALPHA * (s - t.static_kpa);
                first = false;
            }
            t.qc_kpa = t.pitot_kpa - t.static_kpa - s_qc_zero.load();
            t.ias_kt = ias_kt(t.qc_kpa);
            t.alt_ft = pressure_alt_ft(t.static_kpa);
        } else {
            // Simulated: sea-level static pressure and enough impact pressure for SIM_IAS_KT.
            const float qc = qc_for_ias(SIM_IAS_KT);
            t.static_kpa = pressure_for_alt(SIM_ALT_FT);
            t.pitot_kpa = t.static_kpa + qc;
            t.qc_kpa = qc;
            t.ias_kt = SIM_IAS_KT;
            t.alt_ft = SIM_ALT_FT;
            raw[CH_RAIL] = MPX_VS_NOMINAL;
            raw[CH_PITOT] = MPX_VS_NOMINAL * (0.009f * t.pitot_kpa - 0.095f);
            raw[CH_STATIC] = MPX_VS_NOMINAL * (0.009f * t.static_kpa - 0.095f);
        }

        float manual = s_manual_duty.load();

        // Smooth only the physical pot. Manual duty from the web API stays immediate.
        if (pot_ok) {
            if (pot_filtered_mv < 0.0f) {
                pot_filtered_mv = (float)pot_mv;
            } else {
                pot_filtered_mv += POT_FILTER_ALPHA * ((float)pot_mv - pot_filtered_mv);
            }
        }

        // Calibrated electrical position, before nonlinear mapping.
        float pot_position = 0.0f;
        if (pot_filtered_mv >= 0.0f) {
            pot_position = 100.0f * (pot_filtered_mv - POT_MV_MIN) /
                           (float)(POT_MV_MAX - POT_MV_MIN);
            pot_position = fminf(fmaxf(pot_position, 0.0f), 100.0f);
        }

        // Bottom 5% is OFF. Remaining travel follows a quadratic curve.
        // No additional 1% duty snap: that would enlarge the intended OFF zone.
        const float u = fminf(fmaxf((pot_position - 5.0f) / 95.0f, 0.0f), 1.0f);
        const float pot_duty = 100.0f * u * u;
        const bool following_pot = manual < 0.0f;
        const bool valid_pot = pot_ok && pot_filtered_mv >= 0.0f;
        const float target = following_pot
                                 ? (valid_pot ? pot_duty : duty_set)
                                 : manual;

        // 0.05 percentage points preserves useful low-duty adjustment.
        // Endpoints bypass the deadband, but avoid redundant LEDC writes.
        if (fabsf(target - duty_set) >= 0.05f ||
            ((target == 0.0f || target == 100.0f) && target != duty_set)) {
            duty_set = target;
            pwm_apply(duty_set);
        }

        int64_t now = esp_timer_get_time();
        if (now - win_start >= PWM_WINDOW_US) {
            uint64_t sp, sh;
            uint32_t n;
            portENTER_CRITICAL(&s_cap_mux);
            sp = s_sum_period; sh = s_sum_high; n = s_cycles;
            s_sum_period = s_sum_high = 0; s_cycles = 0;
            portEXIT_CRITICAL(&s_cap_mux);
            win_start = now;
            // The pad is inverted relative to J2 B: pad high time is B's low time.
            if (n && sp) {
                t.pwm_freq_hz = (float)cap_hz * n / (float)sp;
                t.pwm_duty_pct = 100.0f - 100.0f * (float)sh / (float)sp;
            } else {
                // No edges: line is stuck high or low
                t.pwm_freq_hz = 0;
                t.pwm_duty_pct = gpio_get_level((gpio_num_t)PIN_PWM_IN) ? 0.0f : 100.0f;
            }
        }

        t.pitot_v = raw[CH_PITOT];
        t.static_v = raw[CH_STATIC];
        t.rail_v = raw[CH_RAIL];
        // Without a measurement the "Fluke" reading is calculated from the duty the capture
        // hardware measured, using the levels J2 B swings between.
        t.pwm_avg_sim = !(present && pwm_ok);
        t.fluke_v = t.pwm_avg_sim ? PWM_V_LOW + t.pwm_duty_pct / 100.0f * (PWM_V_HIGH - PWM_V_LOW) : raw[CH_PWM];
        t.pot_v = (pot_filtered_mv >= 0.0f ? pot_filtered_mv : (float)pot_mv) / 1000.0f;
        t.pwm_set_pct = duty_set;
        t.pwm_manual = manual >= 0;
        t.sensors_sim = !present;
        t.adc_sensors_ok = present && ok;
        t.adc_fluke_ok = present && pwm_ok;
        t.uptime_ms = (uint32_t)(now / 1000);
        t.seq++;

        portENTER_CRITICAL(&s_telem_mux);
        s_telem = t;
        portEXIT_CRITICAL(&s_telem_mux);

        vTaskDelay(1); // lets the idle task run so the task watchdog stays quiet
    }
}

void hw_start()
{
    load_qc_zero();
    s_ready = xSemaphoreCreateBinary();
    xTaskCreatePinnedToCore(hw_task, "hw_core1", 6144, nullptr, configMAX_PRIORITIES - 2, nullptr, 1);
    xSemaphoreTake(s_ready, portMAX_DELAY);
}
