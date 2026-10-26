#pragma once
#include <stdint.h>

// Core 1 publishes, Core 0 (HTTP) reads. Copy-under-spinlock; the struct is small.
struct Telemetry {
    float pitot_v;
    float static_v;
    float pot_v;
    float rail_v;           // measured 5V reference
    float fluke_v;
    float pitot_kpa;        // filtered, absolute
    float static_kpa;       // filtered, absolute
    float qc_kpa;           // pitot - static - zero offset
    float ias_kt;
    float alt_ft;           // pressure altitude from static
    float pwm_freq_hz;
    float pwm_duty_pct;     // measured by MCPWM capture
    float pwm_set_pct;      // commanded
    bool pwm_manual;        // false: duty follows the pot
    uint32_t seq;
    uint32_t uptime_ms;
    bool adc_sensors_ok;
    bool adc_fluke_ok;
    bool sensors_sim;       // ADS1115 absent: pitot/static/IAS/altitude/rail are simulated
    bool pwm_avg_sim;       // fluke_v is calculated from the measured duty, not read
};

// Spawns the Core 1 task and returns once the I2C bus and the board's IO
// expander are configured (native USB selected), so USB can start safely.
void hw_start();

Telemetry telemetry_get();
void pwm_set_manual(float pct);
void pwm_follow_pot();
float hw_zero_qc();        // stores current pitot-static difference as zero; returns it
