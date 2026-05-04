#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

// I²C addresses (DFRobot Gravity sensors via Sensirion + DFRobot silicon)
#define SCD41_ADDR    0x62  // SCD41 — real CO2 + temp + humidity (NDIR)
#define SGP40_ADDR    0x59  // SGP40 — VOC raw signal
#define CO_SEN0466_ADDR 0x74  // DFRobot factory-calibrated electrochemical CO

// ADC channel for MQ-6 propane / LPG (GPIO3 = ADC1_CH2 on ESP32-S3)
// Behind a 10k+15k voltage divider that scales the 5V output to 0-3V at the pin.
#define MQ6_VOLTAGE_DIVIDER_RATIO  (15.0f / (10.0f + 15.0f))  // 0.6

// ---------------------------------------------------------------------------
// Sensor result types
// ---------------------------------------------------------------------------

typedef struct {
    uint16_t co2_ppm;       // 0-40000 ppm (NDIR)
    float    temperature_c; // °C
    float    humidity;      // %RH (0-100)
    bool     valid;
} scd41_data_t;

typedef struct {
    uint16_t voc_raw;   // raw SGP40 reading (~30000 baseline)
    uint16_t voc_index; // 1-500 air-quality index
    bool     valid;
} sgp40_data_t;

typedef struct {
    float ppm;
    bool  valid;
} co_data_t;

typedef struct {
    float    voltage;       // measured voltage at MQ-6 SIG pin (after divider × correction)
    float    rs_over_r0;    // ratio; <1 means gas detected
    uint16_t rs_over_r0_x1000;
    bool     valid;
    bool     calibrated;    // true once R0 has been captured
} mq6_data_t;

// ---------------------------------------------------------------------------
// Bus initialization
// ---------------------------------------------------------------------------

/**
 * Initialize the I²C bus and add SCD41, SGP40, and SEN0466 devices.
 * Continues even if individual devices fail to attach.
 */
esp_err_t sensors_init_i2c(int sda_pin, int scl_pin);

/**
 * Initialize ADC for the MQ-6 sensor.
 * Loads the stored R0 (clean-air baseline) from NVS if present.
 */
esp_err_t sensors_init_mq6(int adc_gpio);

// ---------------------------------------------------------------------------
// SCD41 (real CO2 + temperature + humidity, NDIR)
// ---------------------------------------------------------------------------

/** Send the start-periodic-measurement command. Call once after sensors_init_i2c(). */
esp_err_t scd41_start_periodic(void);

/** Read the latest periodic measurement. Returns valid=false if no data ready. */
scd41_data_t scd41_read(void);

/** Set ambient pressure (in hPa) for compensation. Optional but improves accuracy. */
esp_err_t scd41_set_ambient_pressure(uint16_t pressure_hpa);

// ---------------------------------------------------------------------------
// SGP40 (VOC index)
// ---------------------------------------------------------------------------

/**
 * Trigger a measurement and read raw VOC signal, with humidity/temperature
 * compensation. Pass values from SCD41; pass NaN-equivalent (humidity=-1)
 * to use default 50% RH / 25°C reference.
 */
sgp40_data_t sgp40_measure(float humidity_pct, float temperature_c);

// ---------------------------------------------------------------------------
// SEN0466 (factory-calibrated electrochemical CO)
// ---------------------------------------------------------------------------

/** Set the sensor to active (continuous) acquisition mode. Call once at start. */
esp_err_t co_sen0466_set_active_mode(void);

/** Read the latest CO concentration in ppm. */
co_data_t co_sen0466_read(void);

// ---------------------------------------------------------------------------
// MQ-6 (propane / LPG via ADC)
// ---------------------------------------------------------------------------

/**
 * Sample the MQ-6 output. Reads ADC, undoes voltage divider, computes Rs/R0.
 * If R0 has not yet been calibrated, rs_over_r0 is set to 1.0 and
 * calibrated=false.
 */
mq6_data_t mq6_read(void);

/**
 * Capture the current Rs as the clean-air R0 baseline and store in NVS.
 * Should be called after the heater has stabilized in clean air (24h+ recommended
 * for first-time burn-in, ~3 minutes from cold boot otherwise).
 */
esp_err_t mq6_calibrate_r0(void);
