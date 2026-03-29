#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/twai.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_app_desc.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "led_strip.h"
#include "ota.h"
#include "discovery.h"
#include "sensors.h"

static const char *TAG = "borealis";

// Waveshare ESP32-S3-Zero pin assignments
#define I2C_SDA_PIN   5
#define I2C_SCL_PIN   6
#define CAN_TX_PIN    7
#define CAN_RX_PIN    8
#define RGB_LED_PIN   21

// CAN message identifiers
#define CAN_ID_SENSOR_DATA          0x1F
#define CAN_ID_BOREALIS_CALIBRATION 0x21

// Sensor read interval
#define SENSOR_READ_INTERVAL_MS  2000

// CAN transmit periods
#define CAN_STATUS_PERIOD_MS     33
#define TX_PROBE_INTERVAL_MS     2000

// ---------------------------------------------------------------------------
// WS2812 RGB LED (via led_strip driver)
// ---------------------------------------------------------------------------

static led_strip_handle_t s_led_strip = NULL;

static void led_init(void)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = RGB_LED_PIN,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
    };
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000,  // 10 MHz
    };
    led_strip_new_rmt_device(&strip_config, &rmt_config, &s_led_strip);
    led_strip_clear(s_led_strip);
}

static void led_set(uint8_t r, uint8_t g, uint8_t b)
{
    if (s_led_strip) {
        led_strip_set_pixel(s_led_strip, 0, r, g, b);
        led_strip_refresh(s_led_strip);
    }
}

// ---------------------------------------------------------------------------
// Temperature calibration offset (tenths of °C, from NVS / CAN)
// ---------------------------------------------------------------------------

#define NVS_NS_CALIBRATION  "calibration"
#define NVS_KEY_TEMP_OFFSET "temp_offset"

static volatile int16_t s_temp_offset_tenths = 0;  // e.g. -28 = -2.8°C

static void calibration_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS_CALIBRATION, NVS_READONLY, &h) != ESP_OK) return;
    int16_t val = 0;
    if (nvs_get_i16(h, NVS_KEY_TEMP_OFFSET, &val) == ESP_OK) {
        s_temp_offset_tenths = val;
        ESP_LOGI(TAG, "Loaded temp calibration offset: %d tenths C (%.1fC)",
                 val, val / 10.0f);
    }
    nvs_close(h);
}

static void calibration_save_temp_offset(int16_t tenths)
{
    s_temp_offset_tenths = tenths;
    nvs_handle_t h;
    if (nvs_open(NVS_NS_CALIBRATION, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for calibration write");
        return;
    }
    nvs_set_i16(h, NVS_KEY_TEMP_OFFSET, tenths);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "Saved temp calibration offset: %d tenths C (%.1fC)",
             tenths, tenths / 10.0f);
}

// ---------------------------------------------------------------------------
// Shared sensor data (written by main task, read by TWAI task)
// ---------------------------------------------------------------------------

static volatile int8_t   s_temp_c_int;
static volatile int8_t   s_temp_f_int;
static volatile uint16_t s_humidity_scaled;
static volatile uint16_t s_tvoc;
static volatile uint16_t s_eco2;
static volatile bool     s_sensor_valid = false;

// ---------------------------------------------------------------------------
// TWAI (CAN) task — runs independently so bus errors never stall sensors
// ---------------------------------------------------------------------------

static void twai_task(void *arg)
{
    twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(
        (gpio_num_t)CAN_TX_PIN, (gpio_num_t)CAN_RX_PIN, TWAI_MODE_NORMAL);
    twai_timing_config_t t_config = TWAI_TIMING_CONFIG_500KBITS();
    twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    if (twai_driver_install(&g_config, &t_config, &f_config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install TWAI driver");
        vTaskDelete(NULL);
        return;
    }
    if (twai_start() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start TWAI driver");
        vTaskDelete(NULL);
        return;
    }

    // Broadcast firmware version on CAN 0x04 at startup
    {
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        const esp_app_desc_t *app = esp_app_get_description();
        unsigned maj = 0, min = 0, pat = 0;
        sscanf(app->version, "%u.%u.%u", &maj, &min, &pat);
        twai_message_t ver_msg = {
            .identifier = 0x04,
            .data_length_code = 6,
            .data = { mac[3], mac[4], mac[5], maj, min, pat }
        };
        twai_transmit(&ver_msg, pdMS_TO_TICKS(50));
        ESP_LOGI(TAG, "Version broadcast: %s (CAN 0x04)", app->version);
    }

    uint32_t alerts = TWAI_ALERT_RX_DATA | TWAI_ALERT_ERR_PASS |
                      TWAI_ALERT_BUS_ERROR | TWAI_ALERT_RX_QUEUE_FULL |
                      TWAI_ALERT_BUS_OFF | TWAI_ALERT_BUS_RECOVERED |
                      TWAI_ALERT_ERR_ACTIVE | TWAI_ALERT_TX_FAILED |
                      TWAI_ALERT_TX_SUCCESS;
    twai_reconfigure_alerts(alerts, NULL);
    ESP_LOGI(TAG, "TWAI driver started (NORMAL mode, 500 kbps)");

    typedef enum { TX_ACTIVE, TX_PROBING } tx_state_t;
    bool bus_off = false;
    tx_state_t tx_state = TX_ACTIVE;
    int tx_fail_count = 0;
    const int TX_FAIL_THRESHOLD = 3;
    int64_t last_tx_us = 0;
    const int64_t tx_period_us = CAN_STATUS_PERIOD_MS * 1000LL;
    const int64_t tx_probe_period_us = TX_PROBE_INTERVAL_MS * 1000LL;

    while (1) {
        uint32_t triggered;
        twai_read_alerts(&triggered, pdMS_TO_TICKS(CAN_STATUS_PERIOD_MS));

        if (triggered & TWAI_ALERT_BUS_OFF) {
            ESP_LOGE(TAG, "TWAI bus-off, initiating recovery");
            bus_off = true;
            twai_initiate_recovery();
            continue;
        }

        if (triggered & TWAI_ALERT_BUS_RECOVERED) {
            ESP_LOGI(TAG, "TWAI bus recovered, restarting");
            twai_start();
            bus_off = false;
            tx_fail_count = 0;
            tx_state = TX_PROBING;
        }

        if (triggered & TWAI_ALERT_ERR_PASS) {
            ESP_LOGW(TAG, "TWAI error passive (no peers ACKing?)");
        }
        if (triggered & TWAI_ALERT_TX_FAILED) {
            if (tx_state == TX_ACTIVE) {
                tx_fail_count++;
                if (tx_fail_count >= TX_FAIL_THRESHOLD) {
                    tx_state = TX_PROBING;
                    ESP_LOGW(TAG, "TWAI no peers detected, entering slow probe");
                }
            }
        }
        if (triggered & TWAI_ALERT_TX_SUCCESS) {
            if (tx_state == TX_PROBING) {
                tx_state = TX_ACTIVE;
                tx_fail_count = 0;
                ESP_LOGI(TAG, "TWAI probe ACK'd, peer detected, resuming normal TX");
            }
            tx_fail_count = 0;
        }

        // Drain received messages and dispatch
        if (triggered & TWAI_ALERT_RX_DATA) {
            if (tx_state == TX_PROBING) {
                tx_state = TX_ACTIVE;
                tx_fail_count = 0;
                ESP_LOGI(TAG, "TWAI peer detected via RX, resuming normal TX");
            }
            twai_message_t msg;
            while (twai_receive(&msg, 0) == ESP_OK) {
                if (msg.rtr) continue;

                if (msg.identifier == CAN_ID_OTA_TRIGGER) {
                    ota_handle_trigger(msg.data, msg.data_length_code);
                } else if (msg.identifier == CAN_ID_WIFI_CONFIG) {
                    ota_handle_wifi_config(msg.data, msg.data_length_code);
                } else if (msg.identifier == CAN_ID_DISCOVERY_TRIGGER) {
                    discovery_handle_trigger();
                } else if (msg.identifier == CAN_ID_BOREALIS_CALIBRATION) {
                    if (msg.data_length_code >= 2) {
                        int16_t offset = (int16_t)((msg.data[0] << 8) | msg.data[1]);
                        calibration_save_temp_offset(offset);
                    }
                }
            }
        }

        // Periodic sensor data transmit (skip if bus is down)
        int64_t now_us = esp_timer_get_time();
        int64_t effective_period = (tx_state == TX_PROBING) ? tx_probe_period_us : tx_period_us;
        if (!bus_off && s_sensor_valid && (now_us - last_tx_us >= effective_period)) {
            last_tx_us = now_us;

            uint16_t hum = s_humidity_scaled;
            uint16_t tvoc = s_tvoc;
            uint16_t eco2 = s_eco2;

            twai_message_t m = {
                .identifier = CAN_ID_SENSOR_DATA,
                .data_length_code = 8,
                .data = {
                    (uint8_t)s_temp_c_int,
                    (uint8_t)s_temp_f_int,
                    (hum >> 8) & 0xFF,
                    hum & 0xFF,
                    (tvoc >> 8) & 0xFF,
                    tvoc & 0xFF,
                    (eco2 >> 8) & 0xFF,
                    eco2 & 0xFF,
                }
            };
            twai_transmit(&m, 0);
        }
    }
}

// ---------------------------------------------------------------------------
// Air quality classification (for logging)
// ---------------------------------------------------------------------------

static const char *tvoc_rating(uint16_t tvoc)
{
    if (tvoc < 65)   return "Excellent";
    if (tvoc < 220)  return "Good";
    if (tvoc < 660)  return "Moderate";
    if (tvoc < 2200) return "Poor";
    return "Unhealthy";
}

static const char *co2_level(uint16_t eco2)
{
    if (eco2 < 400)  return "Low";
    if (eco2 < 1000) return "Normal";
    if (eco2 < 2000) return "High";
    return "ALARM";
}

// ---------------------------------------------------------------------------
// Main application
// ---------------------------------------------------------------------------

void app_main(void)
{
    // Initialize LED first for immediate visual feedback
    led_init();
    led_set(0, 40, 0);  // Green: starting up

    ESP_LOGI(TAG, "=== TrailCurrent Borealis ===");

    // OTA / WiFi config / Discovery subsystems
    ota_init();
    discovery_init();
    ESP_LOGI(TAG, "Hostname: %s", ota_get_hostname());

    // CAN runs in its own task — start it before sensors so bus errors
    // or I2C hangs never prevent CAN from operating
    xTaskCreatePinnedToCore(twai_task, "twai", 4096, NULL, 5, NULL, 1);

    // Load temperature calibration offset from NVS
    calibration_load();

    // Initialize I2C sensors
    esp_err_t ret = sensors_init(I2C_SDA_PIN, I2C_SCL_PIN);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Sensor I2C init failed");
    }

    sgp30_iaq_init();

    ESP_LOGI(TAG, "=== Setup complete ===");

    // Main loop: read sensors periodically.
    // Each sensor is read independently — if one fails or is absent,
    // the other's data still transmits (failed fields send as 0).
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(SENSOR_READ_INTERVAL_MS));

        // Read SHT31-D (temperature + humidity)
        sht31_data_t sht = sht31_read();
        float tempC = 0.0f;
        float humidity = 0.0f;

        if (sht.valid) {
            tempC = sht.temperature_c + (s_temp_offset_tenths / 10.0f);
            humidity = sht.humidity;
            float tempF = tempC * 9.0f / 5.0f + 32.0f;
            ESP_LOGI(TAG, "SHT31: %.1fC / %.1fF, Humidity: %.1f%%", tempC, tempF, humidity);
        } else {
            ESP_LOGW(TAG, "SHT31: read failed");
        }

        // Read SGP30 IAQ (TVOC + eCO2)
        sgp30_data_t sgp = sgp30_measure();
        uint16_t tvoc = 0;
        uint16_t eco2 = 0;

        if (sgp.iaq_valid) {
            tvoc = sgp.tvoc;
            eco2 = sgp.eco2;
            ESP_LOGI(TAG, "SGP30: TVOC %d ppb (%s), eCO2 %d ppm (%s)",
                     tvoc, tvoc_rating(tvoc), eco2, co2_level(eco2));

            // Only attempt raw measurement if IAQ succeeded
            sgp30_measure_raw(&sgp);
            if (sgp.raw_valid) {
                ESP_LOGD(TAG, "SGP30 Raw: H2 %d, Ethanol %d", sgp.raw_h2, sgp.raw_ethanol);
            }
        } else {
            ESP_LOGW(TAG, "SGP30: IAQ read failed");
        }

        // Humidity compensation for SGP30 (only if SHT31 data is available)
        if (sht.valid) {
            float absHumidity = 216.7f * (humidity / 100.0f) * 6.112f *
                expf(17.62f * tempC / (243.12f + tempC)) / (273.15f + tempC);
            uint16_t ah_scaled = (uint16_t)(absHumidity * 256.0f);
            sgp30_set_humidity(ah_scaled);
        }

        // Always update shared data and transmit — zero for any failed sensor
        s_temp_c_int = (int8_t)(tempC + 0.5f);
        s_temp_f_int = (int8_t)(tempC * 9.0f / 5.0f + 32.0f + 0.5f);
        s_humidity_scaled = (uint16_t)(humidity * 100.0f);
        s_tvoc = tvoc;
        s_eco2 = eco2;
        s_sensor_valid = true;
    }
}
