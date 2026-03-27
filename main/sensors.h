#pragma once

#include "esp_err.h"
#include "driver/i2c_master.h"
#include <stdbool.h>
#include <stdint.h>

// I2C addresses
#define SHT31_ADDR  0x44
#define SGP30_ADDR  0x58

// Sensor data
typedef struct {
    float temperature_c;
    float humidity;
    bool valid;
} sht31_data_t;

typedef struct {
    uint16_t tvoc;
    uint16_t eco2;
    uint16_t raw_h2;
    uint16_t raw_ethanol;
    bool iaq_valid;
    bool raw_valid;
} sgp30_data_t;

/**
 * Initialize the I2C bus and add SHT31 and SGP30 devices.
 * Returns the bus handle (also used internally).
 */
esp_err_t sensors_init(int sda_pin, int scl_pin);

/**
 * Read temperature and humidity from SHT31-D.
 */
sht31_data_t sht31_read(void);

/**
 * Initialize SGP30 IAQ algorithm. Call once after sensors_init().
 */
esp_err_t sgp30_iaq_init(void);

/**
 * Measure IAQ (eCO2 and TVOC) from SGP30.
 */
sgp30_data_t sgp30_measure(void);

/**
 * Measure raw H2 and Ethanol signals from SGP30.
 */
void sgp30_measure_raw(sgp30_data_t *data);

/**
 * Set humidity compensation on SGP30.
 * ah_scaled: absolute humidity in 8.8 fixed-point format (g/m^3 * 256).
 */
esp_err_t sgp30_set_humidity(uint16_t ah_scaled);
