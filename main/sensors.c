#include "sensors.h"

#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "sensors";

static i2c_master_bus_handle_t s_i2c_bus = NULL;
static i2c_master_dev_handle_t s_sht31_dev = NULL;
static i2c_master_dev_handle_t s_sgp30_dev = NULL;

// ---------------------------------------------------------------------------
// CRC-8 (polynomial 0x31, init 0xFF) — used by both SHT31 and SGP30
// ---------------------------------------------------------------------------

static uint8_t crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            if (crc & 0x80) {
                crc = (crc << 1) ^ 0x31;
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

// ---------------------------------------------------------------------------
// I2C bus and device initialization
// ---------------------------------------------------------------------------

esp_err_t sensors_init(int sda_pin, int scl_pin)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = sda_pin,
        .scl_io_num = scl_pin,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t ret = i2c_new_master_bus(&bus_cfg, &s_i2c_bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Add SHT31 device — continue even if it fails
    i2c_device_config_t sht31_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = SHT31_ADDR,
        .scl_speed_hz = 100000,
    };
    ret = i2c_master_bus_add_device(s_i2c_bus, &sht31_cfg, &s_sht31_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SHT31 device add failed: %s", esp_err_to_name(ret));
        s_sht31_dev = NULL;
    } else {
        ESP_LOGI(TAG, "SHT31-D initialized (0x%02X)", SHT31_ADDR);
    }

    // Add SGP30 device — continue even if it fails
    i2c_device_config_t sgp30_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = SGP30_ADDR,
        .scl_speed_hz = 100000,
    };
    ret = i2c_master_bus_add_device(s_i2c_bus, &sgp30_cfg, &s_sgp30_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SGP30 device add failed: %s", esp_err_to_name(ret));
        s_sgp30_dev = NULL;
    } else {
        ESP_LOGI(TAG, "SGP30 initialized (0x%02X)", SGP30_ADDR);
    }

    return ESP_OK;
}

// ---------------------------------------------------------------------------
// SHT31-D — single-shot measurement, high repeatability
// ---------------------------------------------------------------------------

sht31_data_t sht31_read(void)
{
    sht31_data_t result = { .valid = false };

    if (s_sht31_dev == NULL) return result;

    // Command: 0x2400 — single shot, high repeatability, no clock stretching
    uint8_t cmd[2] = { 0x24, 0x00 };
    esp_err_t ret = i2c_master_transmit(s_sht31_dev, cmd, sizeof(cmd), -1);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SHT31 command failed: %s", esp_err_to_name(ret));
        return result;
    }

    vTaskDelay(pdMS_TO_TICKS(20));

    uint8_t data[6];
    ret = i2c_master_receive(s_sht31_dev, data, sizeof(data), -1);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SHT31 read failed: %s", esp_err_to_name(ret));
        return result;
    }

    // Verify CRCs
    if (crc8(&data[0], 2) != data[2] || crc8(&data[3], 2) != data[5]) {
        ESP_LOGW(TAG, "SHT31 CRC error");
        return result;
    }

    uint16_t raw_temp = ((uint16_t)data[0] << 8) | data[1];
    uint16_t raw_hum  = ((uint16_t)data[3] << 8) | data[4];

    result.temperature_c = -45.0f + 175.0f * ((float)raw_temp / 65535.0f);
    result.humidity      = 100.0f * ((float)raw_hum / 65535.0f);
    result.valid = true;

    return result;
}

// ---------------------------------------------------------------------------
// SGP30 — IAQ measurement
// ---------------------------------------------------------------------------

esp_err_t sgp30_iaq_init(void)
{
    if (s_sgp30_dev == NULL) return ESP_ERR_INVALID_STATE;

    uint8_t cmd[2] = { 0x20, 0x03 };
    esp_err_t ret = i2c_master_transmit(s_sgp30_dev, cmd, sizeof(cmd), -1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SGP30 IAQ init failed: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "SGP30 IAQ algorithm initialized");
    }
    vTaskDelay(pdMS_TO_TICKS(10));
    return ret;
}

sgp30_data_t sgp30_measure(void)
{
    sgp30_data_t result = { .iaq_valid = false, .raw_valid = false };

    if (s_sgp30_dev == NULL) return result;

    // Measure IAQ: command 0x2008
    uint8_t cmd[2] = { 0x20, 0x08 };
    esp_err_t ret = i2c_master_transmit(s_sgp30_dev, cmd, sizeof(cmd), -1);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SGP30 IAQ command failed: %s", esp_err_to_name(ret));
        return result;
    }

    vTaskDelay(pdMS_TO_TICKS(15));

    uint8_t data[6];
    ret = i2c_master_receive(s_sgp30_dev, data, sizeof(data), -1);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SGP30 IAQ read failed: %s", esp_err_to_name(ret));
        return result;
    }

    // Verify CRCs
    if (crc8(&data[0], 2) != data[2] || crc8(&data[3], 2) != data[5]) {
        ESP_LOGW(TAG, "SGP30 IAQ CRC error");
        return result;
    }

    result.eco2 = ((uint16_t)data[0] << 8) | data[1];
    result.tvoc = ((uint16_t)data[3] << 8) | data[4];
    result.iaq_valid = true;

    return result;
}

void sgp30_measure_raw(sgp30_data_t *data)
{
    if (s_sgp30_dev == NULL) return;

    // Measure raw signals: command 0x2050
    uint8_t cmd[2] = { 0x20, 0x50 };
    esp_err_t ret = i2c_master_transmit(s_sgp30_dev, cmd, sizeof(cmd), -1);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SGP30 raw command failed: %s", esp_err_to_name(ret));
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(30));

    uint8_t raw[6];
    ret = i2c_master_receive(s_sgp30_dev, raw, sizeof(raw), -1);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SGP30 raw read failed: %s", esp_err_to_name(ret));
        return;
    }

    if (crc8(&raw[0], 2) != raw[2] || crc8(&raw[3], 2) != raw[5]) {
        ESP_LOGW(TAG, "SGP30 raw CRC error");
        return;
    }

    data->raw_h2      = ((uint16_t)raw[0] << 8) | raw[1];
    data->raw_ethanol = ((uint16_t)raw[3] << 8) | raw[4];
    data->raw_valid = true;
}

esp_err_t sgp30_set_humidity(uint16_t ah_scaled)
{
    if (s_sgp30_dev == NULL) return ESP_ERR_INVALID_STATE;

    uint8_t cmd[5];
    cmd[0] = 0x20;
    cmd[1] = 0x61;
    cmd[2] = (ah_scaled >> 8) & 0xFF;
    cmd[3] = ah_scaled & 0xFF;
    cmd[4] = crc8(&cmd[2], 2);

    esp_err_t ret = i2c_master_transmit(s_sgp30_dev, cmd, sizeof(cmd), -1);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SGP30 set humidity failed: %s", esp_err_to_name(ret));
    }
    vTaskDelay(pdMS_TO_TICKS(10));
    return ret;
}
