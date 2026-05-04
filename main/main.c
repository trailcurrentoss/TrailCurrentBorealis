#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/twai.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "can_common.h"
#include "wifi_config.h"
#include "ota.h"
#include "discovery.h"
#include "sensors.h"

static const char *TAG = "borealis";

// Waveshare ESP32-S3-RS485-CAN pin assignments
#define I2C_SDA_PIN          5
#define I2C_SCL_PIN          6
#define MQ6_ADC_GPIO         3   // ADC1_CH2 via 10k+15k divider
#define CAN_RX_PIN           16  // hardwired internal
#define CAN_TX_PIN           15  // hardwired internal

// CAN protocol IDs (match sibling-project conventions)
#define CAN_ID_OTA               0x00
#define CAN_ID_WIFI_CONFIG       0x01
#define CAN_ID_DISCOVERY_TRIGGER 0x02
#define CAN_ID_ENVIRONMENTAL     0x1F
#define CAN_ID_SAFETY            0x20

// Periods
#define CAN_STATUS_PERIOD_MS  1000
#define TX_PROBE_INTERVAL_MS  2000
#define SENSOR_READ_INTERVAL_MS 5000  // SCD41 periodic = 5s

// Alarm thresholds (match README)
#define CO_PPM_WARNING        70
#define CO_PPM_ALARM          200
#define LPG_RATIO_WARNING_X1000  500   // Rs/R0 < 0.5
#define LPG_RATIO_ALARM_X1000    300   // Rs/R0 < 0.3
#define CO2_PPM_WARNING       1500
#define CO2_PPM_ALARM         2500
#define VOC_INDEX_ALARM       400

#define ALARM_FLAG_CO_WARN     0x01
#define ALARM_FLAG_CO_ALARM    0x02
#define ALARM_FLAG_LPG_WARN    0x04
#define ALARM_FLAG_LPG_ALARM   0x08
#define ALARM_FLAG_CO2_WARN    0x10
#define ALARM_FLAG_CO2_ALARM   0x20
#define ALARM_FLAG_VOC_ALARM   0x40

// ---------------------------------------------------------------------------
// Shared sensor data (written by main task, read by TWAI task)
// ---------------------------------------------------------------------------

static volatile int8_t   s_temp_c_int;
static volatile int8_t   s_temp_f_int;
static volatile uint16_t s_humidity_scaled;
static volatile uint16_t s_co2_ppm;
static volatile uint16_t s_voc_index;

static volatile uint16_t s_co_ppm;
static volatile uint16_t s_lpg_rs_r0_x1000;
static volatile uint8_t  s_alarm_flags;

static volatile bool s_env_valid    = false;
static volatile bool s_safety_valid = false;

// ---------------------------------------------------------------------------
// TWAI (CAN) task — independent FreeRTOS task following the canonical
// TX_ACTIVE / TX_PROBING state machine used across all TrailCurrent modules.
// ---------------------------------------------------------------------------

static void twai_task(void *arg)
{
    // Configure alerts BEFORE any bus activity so no error transitions are missed.
    twai_reconfigure_alerts(CAN_COMMON_ALERTS, NULL);

    // Alerts armed — version broadcast TX failure is caught by the state machine.
    can_common_version_broadcast();

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

        // Bus-off: stop transmitting, initiate recovery
        if (triggered & TWAI_ALERT_BUS_OFF) {
            ESP_LOGE(TAG, "TWAI bus-off, initiating recovery");
            bus_off = true;
            twai_initiate_recovery();
            // No continue — fall through so RX_DATA in the same poll is still processed.
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
                can_common_version_broadcast();
                ESP_LOGI(TAG, "TWAI probe ACK'd, peer detected, resuming normal TX");
            }
            tx_fail_count = 0;
        }

        // Drain received messages and dispatch
        if (triggered & TWAI_ALERT_RX_DATA) {
            if (tx_state == TX_PROBING) {
                tx_state = TX_ACTIVE;
                tx_fail_count = 0;
                can_common_version_broadcast();
                ESP_LOGI(TAG, "TWAI peer detected via RX, resuming normal TX");
            }
            twai_message_t msg;
            while (twai_receive(&msg, 0) == ESP_OK) {
                if (msg.rtr) continue;

                switch (msg.identifier) {
                case CAN_ID_OTA:
                    if (msg.data_length_code >= 3) {
                        ota_handle_trigger(msg.data, msg.data_length_code);
                    }
                    break;

                case CAN_ID_WIFI_CONFIG:
                    if (msg.data_length_code >= 1) {
                        wifi_config_handle_can(msg.data, msg.data_length_code);
                    }
                    break;

                case CAN_ID_DISCOVERY_TRIGGER:
                    discovery_handle_trigger();
                    break;

                default:
                    break;
                }
            }
        }

        wifi_config_check_timeout();

        // Periodic transmit
        int64_t now_us = esp_timer_get_time();
        int64_t effective_period = (tx_state == TX_PROBING) ? tx_probe_period_us : tx_period_us;
        if (!bus_off && (now_us - last_tx_us >= effective_period)) {
            last_tx_us = now_us;

            if (s_env_valid) {
                uint16_t hum  = s_humidity_scaled;
                uint16_t co2  = s_co2_ppm;
                uint16_t voc  = s_voc_index;
                twai_message_t m = {
                    .identifier = CAN_ID_ENVIRONMENTAL,
                    .data_length_code = 8,
                    .data = {
                        (uint8_t)s_temp_c_int,
                        (uint8_t)s_temp_f_int,
                        (hum >> 8) & 0xFF,  hum & 0xFF,
                        (co2 >> 8) & 0xFF,  co2 & 0xFF,
                        (voc >> 8) & 0xFF,  voc & 0xFF,
                    }
                };
                twai_transmit(&m, 0);
            }

            if (s_safety_valid) {
                uint16_t co  = s_co_ppm;
                uint16_t lpg = s_lpg_rs_r0_x1000;
                twai_message_t m = {
                    .identifier = CAN_ID_SAFETY,
                    .data_length_code = 8,
                    .data = {
                        (co  >> 8) & 0xFF, co  & 0xFF,
                        (lpg >> 8) & 0xFF, lpg & 0xFF,
                        s_alarm_flags,
                        0, 0, 0,
                    }
                };
                twai_transmit(&m, 0);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Compose alarm flags from the latest sensor state
// ---------------------------------------------------------------------------

static uint8_t compute_alarm_flags(uint16_t co_ppm, uint16_t lpg_x1000,
                                   uint16_t co2_ppm, uint16_t voc_index)
{
    uint8_t f = 0;
    if (co_ppm >= CO_PPM_ALARM)         f |= ALARM_FLAG_CO_ALARM;
    else if (co_ppm >= CO_PPM_WARNING)  f |= ALARM_FLAG_CO_WARN;

    if (lpg_x1000 < LPG_RATIO_ALARM_X1000)        f |= ALARM_FLAG_LPG_ALARM;
    else if (lpg_x1000 < LPG_RATIO_WARNING_X1000) f |= ALARM_FLAG_LPG_WARN;

    if (co2_ppm >= CO2_PPM_ALARM)         f |= ALARM_FLAG_CO2_ALARM;
    else if (co2_ppm >= CO2_PPM_WARNING)  f |= ALARM_FLAG_CO2_WARN;

    if (voc_index >= VOC_INDEX_ALARM) f |= ALARM_FLAG_VOC_ALARM;
    return f;
}

// ---------------------------------------------------------------------------
// Main application
// ---------------------------------------------------------------------------

void app_main(void)
{
    ESP_LOGI(TAG, "=== TrailCurrent Borealis ===");

    // Initialize NVS, hostname, and load WiFi credentials
    ESP_ERROR_CHECK(wifi_config_init());

    char ssid[33] = {0};
    char password[64] = {0};
    if (wifi_config_load(ssid, sizeof(ssid), password, sizeof(password))) {
        ESP_LOGI(TAG, "WiFi credentials loaded from NVS");
    } else {
        ESP_LOGI(TAG, "No WiFi credentials — OTA disabled until provisioned via CAN");
    }

    discovery_init();
    ota_init();
    ESP_LOGI(TAG, "Hostname: %s", wifi_config_get_hostname());

    // CAN runs in its own task before sensors so bus errors or I²C hangs
    // never prevent CAN from operating.
    ESP_ERROR_CHECK(can_common_init(CAN_TX_PIN, CAN_RX_PIN));
    xTaskCreatePinnedToCore(twai_task, "twai", 4096, NULL, 5, NULL, 1);

    // Sensors
    if (sensors_init_i2c(I2C_SDA_PIN, I2C_SCL_PIN) != ESP_OK) {
        ESP_LOGE(TAG, "I²C init failed");
    }
    scd41_start_periodic();
    co_sen0466_set_active_mode();
    sensors_init_mq6(MQ6_ADC_GPIO);

    ESP_LOGI(TAG, "=== Setup complete ===");

    // Main loop: read sensors periodically, update shared values for CAN task.
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(SENSOR_READ_INTERVAL_MS));

        scd41_data_t scd = scd41_read();
        if (scd.valid) {
            float tF = scd.temperature_c * 9.0f / 5.0f + 32.0f;
            ESP_LOGI(TAG, "SCD41: %.1f°C / %.1f°F  RH %.1f%%  CO2 %u ppm",
                     scd.temperature_c, tF, scd.humidity, scd.co2_ppm);

            s_temp_c_int = (int8_t)(scd.temperature_c + 0.5f);
            s_temp_f_int = (int8_t)(tF + 0.5f);
            s_humidity_scaled = (uint16_t)(scd.humidity * 100.0f);
            s_co2_ppm = scd.co2_ppm;
        }

        sgp40_data_t sgp = sgp40_measure(
            scd.valid ? scd.humidity : -1.0f,
            scd.valid ? scd.temperature_c : 25.0f);
        if (sgp.valid) {
            ESP_LOGI(TAG, "SGP40: VOC raw %u  index %u", sgp.voc_raw, sgp.voc_index);
            s_voc_index = sgp.voc_index;
        }

        if (scd.valid && sgp.valid) {
            s_env_valid = true;
        }

        co_data_t co = co_sen0466_read();
        if (co.valid) {
            ESP_LOGI(TAG, "CO:    %.1f ppm", co.ppm);
            s_co_ppm = (uint16_t)(co.ppm + 0.5f);
        }

        mq6_data_t mq = mq6_read();
        if (mq.valid) {
            ESP_LOGI(TAG, "MQ-6:  V=%.2fV  Rs/R0=%.2f%s",
                     mq.voltage, mq.rs_over_r0,
                     mq.calibrated ? "" : " (uncalibrated)");
            s_lpg_rs_r0_x1000 = mq.rs_over_r0_x1000;
        }

        if (co.valid || mq.valid) {
            s_alarm_flags = compute_alarm_flags(s_co_ppm, s_lpg_rs_r0_x1000,
                                                s_co2_ppm, s_voc_index);
            s_safety_valid = true;
        }
    }
}
