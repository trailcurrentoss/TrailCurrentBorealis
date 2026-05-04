#include "sensors.h"

#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "sensors";

static i2c_master_bus_handle_t s_i2c_bus = NULL;
static i2c_master_dev_handle_t s_scd41_dev = NULL;
static i2c_master_dev_handle_t s_sgp40_dev = NULL;
static i2c_master_dev_handle_t s_co_dev = NULL;

// MQ-6 ADC state
static adc_oneshot_unit_handle_t s_adc_handle = NULL;
static adc_cali_handle_t s_adc_cali = NULL;
static adc_channel_t s_mq6_channel = ADC_CHANNEL_2;  // GPIO3
static bool s_mq6_initialized = false;
static float s_mq6_r0 = 0.0f;        // Rs in clean air, captured during calibration
static bool s_mq6_calibrated = false;

#define NVS_NS_MQ6        "mq6"
#define NVS_KEY_R0        "r0"

// Reference voltage / resistor used in Rs calculation. The DFRobot SEN0131
// uses a fixed RL (with potentiometer trim) on its onboard divider; we don't
// know the exact value so we use a relative ratio instead — Rs is computed
// from the assumption RL = 10kΩ (the MQ-6 datasheet typical) and then we
// rely on the Rs/R0 ratio for gas detection, which cancels out RL.
#define MQ6_VCC          5.0f
#define MQ6_RL_KOHMS     10.0f

// ADC sampling parameters (chosen, not defaulted)
//   Oversampling N=64: noise reduction ∝ √N → ~8× reduction from the ~10-20 mV
//     ADC noise floor at DB_12, leaving ~1-3 mV residual noise.  Total sample
//     time ~1.6 ms, negligible at the 5-second sensor cadence.  256 would gain
//     ~2× more but adds no practical value for this slow signal.
//   Trim N=4: discard the highest 4 and lowest 4 samples before averaging.
//     A trimmed mean is robust to occasional outliers (WiFi TX bursts, ESD,
//     supply transients) that pure mean would let propagate.  Critical for a
//     safety sensor where a single bad reading could trigger a false alarm.
//   Warmup reads N=2: the ESP32-S3 sample-and-hold capacitor charges on first
//     read after idle; the very first sample can be off.  Discard before
//     starting the measurement set.
//   V_MAX_MV=3300: 12 dB attenuation has a typical max of ~3.1V; 3.3V is a
//     conservative upper bound for sanity-rejecting nonsense readings.
#define MQ6_ADC_SAMPLES        64
#define MQ6_ADC_TRIM           4
#define MQ6_ADC_WARMUP_READS   2
#define MQ6_ADC_V_MAX_MV       3300

// ---------------------------------------------------------------------------
// CRC-8 (polynomial 0x31, init 0xFF) — used by SCD41, SGP40, and SHT-family
// ---------------------------------------------------------------------------

static uint8_t sensirion_crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? (crc << 1) ^ 0x31 : (crc << 1);
        }
    }
    return crc;
}

// Send a 16-bit Sensirion command (no parameters)
static esp_err_t sensirion_send_cmd(i2c_master_dev_handle_t dev, uint16_t cmd)
{
    uint8_t buf[2] = { (cmd >> 8) & 0xFF, cmd & 0xFF };
    return i2c_master_transmit(dev, buf, 2, 1000);
}

// Send a 16-bit Sensirion command with a 16-bit parameter (with CRC)
static esp_err_t sensirion_send_cmd_with_arg(i2c_master_dev_handle_t dev,
                                             uint16_t cmd, uint16_t arg)
{
    uint8_t buf[5];
    buf[0] = (cmd >> 8) & 0xFF;
    buf[1] = cmd & 0xFF;
    buf[2] = (arg >> 8) & 0xFF;
    buf[3] = arg & 0xFF;
    buf[4] = sensirion_crc8(&buf[2], 2);
    return i2c_master_transmit(dev, buf, sizeof(buf), 1000);
}

// ---------------------------------------------------------------------------
// I²C bus setup
// ---------------------------------------------------------------------------

esp_err_t sensors_init_i2c(int sda_pin, int scl_pin)
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
        ESP_LOGE(TAG, "I²C bus init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    i2c_device_config_t scd41_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = SCD41_ADDR,
        .scl_speed_hz = 100000,
    };
    if (i2c_master_bus_add_device(s_i2c_bus, &scd41_cfg, &s_scd41_dev) != ESP_OK) {
        ESP_LOGE(TAG, "SCD41 attach failed");
        s_scd41_dev = NULL;
    } else {
        ESP_LOGI(TAG, "SCD41 attached (0x%02X)", SCD41_ADDR);
    }

    i2c_device_config_t sgp40_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = SGP40_ADDR,
        .scl_speed_hz = 100000,
    };
    if (i2c_master_bus_add_device(s_i2c_bus, &sgp40_cfg, &s_sgp40_dev) != ESP_OK) {
        ESP_LOGE(TAG, "SGP40 attach failed");
        s_sgp40_dev = NULL;
    } else {
        ESP_LOGI(TAG, "SGP40 attached (0x%02X)", SGP40_ADDR);
    }

    i2c_device_config_t co_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CO_SEN0466_ADDR,
        .scl_speed_hz = 100000,
    };
    if (i2c_master_bus_add_device(s_i2c_bus, &co_cfg, &s_co_dev) != ESP_OK) {
        ESP_LOGE(TAG, "SEN0466 attach failed");
        s_co_dev = NULL;
    } else {
        ESP_LOGI(TAG, "SEN0466 CO sensor attached (0x%02X)", CO_SEN0466_ADDR);
    }

    return ESP_OK;
}

// ---------------------------------------------------------------------------
// SCD41 — Sensirion photoacoustic NDIR CO2 + T + RH
// ---------------------------------------------------------------------------

#define SCD41_CMD_START_PERIODIC  0x21B1
#define SCD41_CMD_READ_MEAS       0xEC05
#define SCD41_CMD_GET_DATA_READY  0xE4B8
#define SCD41_CMD_SET_PRESSURE    0xE000
#define SCD41_CMD_STOP_PERIODIC   0x3F86

esp_err_t scd41_start_periodic(void)
{
    if (s_scd41_dev == NULL) return ESP_ERR_INVALID_STATE;
    esp_err_t ret = sensirion_send_cmd(s_scd41_dev, SCD41_CMD_START_PERIODIC);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SCD41 start_periodic failed: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "SCD41 periodic measurement started (5s update)");
    }
    return ret;
}

esp_err_t scd41_set_ambient_pressure(uint16_t pressure_hpa)
{
    if (s_scd41_dev == NULL) return ESP_ERR_INVALID_STATE;
    return sensirion_send_cmd_with_arg(s_scd41_dev, SCD41_CMD_SET_PRESSURE, pressure_hpa);
}

static bool scd41_data_ready(void)
{
    if (s_scd41_dev == NULL) return false;
    if (sensirion_send_cmd(s_scd41_dev, SCD41_CMD_GET_DATA_READY) != ESP_OK) return false;
    vTaskDelay(pdMS_TO_TICKS(2));
    uint8_t resp[3];
    if (i2c_master_receive(s_scd41_dev, resp, 3, 1000) != ESP_OK) return false;
    if (sensirion_crc8(resp, 2) != resp[2]) return false;
    // Lower 11 bits == 0 means no data ready
    uint16_t status = ((uint16_t)resp[0] << 8) | resp[1];
    return (status & 0x07FF) != 0;
}

scd41_data_t scd41_read(void)
{
    scd41_data_t out = { .valid = false };
    if (s_scd41_dev == NULL) return out;

    if (!scd41_data_ready()) {
        return out;
    }

    if (sensirion_send_cmd(s_scd41_dev, SCD41_CMD_READ_MEAS) != ESP_OK) return out;
    vTaskDelay(pdMS_TO_TICKS(2));

    uint8_t buf[9];
    if (i2c_master_receive(s_scd41_dev, buf, sizeof(buf), 1000) != ESP_OK) return out;

    if (sensirion_crc8(&buf[0], 2) != buf[2] ||
        sensirion_crc8(&buf[3], 2) != buf[5] ||
        sensirion_crc8(&buf[6], 2) != buf[8]) {
        ESP_LOGW(TAG, "SCD41 CRC error");
        return out;
    }

    uint16_t raw_co2 = ((uint16_t)buf[0] << 8) | buf[1];
    uint16_t raw_t   = ((uint16_t)buf[3] << 8) | buf[4];
    uint16_t raw_rh  = ((uint16_t)buf[6] << 8) | buf[7];

    out.co2_ppm = raw_co2;
    out.temperature_c = -45.0f + 175.0f * ((float)raw_t / 65535.0f);
    out.humidity      = 100.0f * ((float)raw_rh / 65535.0f);
    out.valid = true;
    return out;
}

// ---------------------------------------------------------------------------
// SGP40 — Sensirion VOC sensor (raw signal + simple index estimator)
// ---------------------------------------------------------------------------

#define SGP40_CMD_MEASURE_RAW  0x260F

// Simplified VOC index estimator with running baseline.
// The official Sensirion gas_index_algorithm (BSD-3) provides better adaptation;
// drop it in via component manager when tuning becomes important. This stub is
// good enough for "is air quality changing" trending out of the box.
static uint16_t sgp40_baseline_sraw = 30000;
static bool sgp40_baseline_seeded = false;

static uint16_t sgp40_estimate_index(uint16_t sraw)
{
    if (!sgp40_baseline_seeded) {
        sgp40_baseline_sraw = sraw;
        sgp40_baseline_seeded = true;
    }
    // Slow EMA of baseline (~24h time constant at 1 Hz: alpha = 1/86400 ≈ 1.2e-5)
    // We approximate with a 1/4096 step which is ~17 minutes — useful for short-term
    // tuning while testing, replace with the Sensirion lib for long-term operation.
    int32_t delta_b = (int32_t)sraw - (int32_t)sgp40_baseline_sraw;
    sgp40_baseline_sraw = (uint16_t)((int32_t)sgp40_baseline_sraw + (delta_b / 4096));

    // VOC Index increases when sraw rises above baseline.
    int32_t deviation = (int32_t)sraw - (int32_t)sgp40_baseline_sraw;
    int32_t idx = 100 + deviation / 50;
    if (idx < 1) idx = 1;
    if (idx > 500) idx = 500;
    return (uint16_t)idx;
}

sgp40_data_t sgp40_measure(float humidity_pct, float temperature_c)
{
    sgp40_data_t out = { .valid = false };
    if (s_sgp40_dev == NULL) return out;

    // Encode RH and T per Sensirion datasheet: rh_ticks = rh * 65535/100
    uint16_t rh_ticks, t_ticks;
    if (humidity_pct < 0.0f || humidity_pct > 100.0f) {
        rh_ticks = 0x8000;  // 50% default
        t_ticks  = 0x6666;  // 25°C default
    } else {
        rh_ticks = (uint16_t)((humidity_pct * 65535.0f) / 100.0f);
        t_ticks  = (uint16_t)(((temperature_c + 45.0f) * 65535.0f) / 175.0f);
    }

    uint8_t cmd[8];
    cmd[0] = (SGP40_CMD_MEASURE_RAW >> 8) & 0xFF;
    cmd[1] = SGP40_CMD_MEASURE_RAW & 0xFF;
    cmd[2] = (rh_ticks >> 8) & 0xFF;
    cmd[3] = rh_ticks & 0xFF;
    cmd[4] = sensirion_crc8(&cmd[2], 2);
    cmd[5] = (t_ticks >> 8) & 0xFF;
    cmd[6] = t_ticks & 0xFF;
    cmd[7] = sensirion_crc8(&cmd[5], 2);

    if (i2c_master_transmit(s_sgp40_dev, cmd, sizeof(cmd), 1000) != ESP_OK) return out;
    vTaskDelay(pdMS_TO_TICKS(35));  // datasheet: 30 ms measurement time

    uint8_t resp[3];
    if (i2c_master_receive(s_sgp40_dev, resp, sizeof(resp), 1000) != ESP_OK) return out;
    if (sensirion_crc8(resp, 2) != resp[2]) {
        ESP_LOGW(TAG, "SGP40 CRC error");
        return out;
    }

    out.voc_raw = ((uint16_t)resp[0] << 8) | resp[1];
    out.voc_index = sgp40_estimate_index(out.voc_raw);
    out.valid = true;
    return out;
}

// ---------------------------------------------------------------------------
// DFRobot SEN0466 — factory-calibrated electrochemical CO sensor (I²C)
// Uses DFRobot's 9-byte command frame protocol; checksum is 2's complement
// of the sum of bytes 1-7.
// ---------------------------------------------------------------------------

static uint8_t sen0466_checksum(const uint8_t *frame)
{
    uint16_t sum = 0;
    for (int i = 1; i < 8; i++) sum += frame[i];
    return (uint8_t)((~sum + 1) & 0xFF);
}

static esp_err_t sen0466_send_cmd(uint8_t cmd_byte,
                                  uint8_t b3, uint8_t b4, uint8_t b5,
                                  uint8_t b6, uint8_t b7,
                                  uint8_t *resp_out)
{
    if (s_co_dev == NULL) return ESP_ERR_INVALID_STATE;

    uint8_t frame[9] = { 0xFF, 0x01, cmd_byte, b3, b4, b5, b6, b7, 0 };
    frame[8] = sen0466_checksum(frame);

    if (i2c_master_transmit(s_co_dev, frame, 9, 1000) != ESP_OK) return ESP_FAIL;
    vTaskDelay(pdMS_TO_TICKS(100));

    uint8_t resp[9];
    if (i2c_master_receive(s_co_dev, resp, 9, 1000) != ESP_OK) return ESP_FAIL;
    if (resp[0] != 0xFF) return ESP_FAIL;
    if (sen0466_checksum(resp) != resp[8]) return ESP_FAIL;
    if (resp_out) memcpy(resp_out, resp, 9);
    return ESP_OK;
}

esp_err_t co_sen0466_set_active_mode(void)
{
    if (s_co_dev == NULL) return ESP_ERR_INVALID_STATE;
    // 0x78: set mode; 0x03 = active (continuous), 0x04 = passive
    esp_err_t ret = sen0466_send_cmd(0x78, 0x03, 0x00, 0x00, 0x00, 0x00, NULL);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "SEN0466 set to active acquisition mode");
    } else {
        ESP_LOGW(TAG, "SEN0466 set_active_mode failed");
    }
    return ret;
}

co_data_t co_sen0466_read(void)
{
    co_data_t out = { .valid = false };
    if (s_co_dev == NULL) return out;

    uint8_t resp[9];
    // 0x86: read concentration. Response[2..3] = high/low byte, units 0.1 ppm.
    if (sen0466_send_cmd(0x86, 0x00, 0x00, 0x00, 0x00, 0x00, resp) != ESP_OK) {
        return out;
    }

    uint16_t raw = ((uint16_t)resp[2] << 8) | resp[3];
    uint8_t  decimal_places = resp[4];
    float divisor = 1.0f;
    for (int i = 0; i < decimal_places; i++) divisor *= 10.0f;
    out.ppm = (float)raw / divisor;
    out.valid = true;
    return out;
}

// ---------------------------------------------------------------------------
// MQ-6 — propane / LPG analog sensor on ADC1
// Voltage divider 10k+15k between SIG and ADC pin scales 0-5V → 0-3V at ADC.
// ---------------------------------------------------------------------------

esp_err_t sensors_init_mq6(int adc_gpio)
{
    // Map GPIO to ADC1 channel (ESP32-S3: GPIO1=CH0 ... GPIO10=CH9)
    if (adc_gpio < 1 || adc_gpio > 10) {
        ESP_LOGE(TAG, "MQ-6: GPIO %d not on ADC1", adc_gpio);
        return ESP_ERR_INVALID_ARG;
    }
    s_mq6_channel = (adc_channel_t)(adc_gpio - 1);

    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = ADC_UNIT_1,
    };
    esp_err_t ret = adc_oneshot_new_unit(&unit_cfg, &s_adc_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ADC unit init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,   // ~0-3.1V; required to capture our 0-3V divided signal
        .bitwidth = ADC_BITWIDTH_12, // explicit 12-bit (also the ESP32-S3 default)
    };
    ret = adc_oneshot_config_channel(s_adc_handle, s_mq6_channel, &chan_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ADC channel config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Calibration (curve fitting if available)
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = ADC_UNIT_1,
        .chan = s_mq6_channel,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_adc_cali) == ESP_OK) {
        ESP_LOGI(TAG, "ADC calibration: curve fitting (per-chip eFuse data)");
    } else {
        ESP_LOGW(TAG, "ADC calibration unavailable — using linear fallback (~6%% error)");
        s_adc_cali = NULL;
    }

    // Try to load stored R0
    nvs_handle_t h;
    if (nvs_open(NVS_NS_MQ6, NVS_READONLY, &h) == ESP_OK) {
        uint32_t stored = 0;
        if (nvs_get_u32(h, NVS_KEY_R0, &stored) == ESP_OK) {
            // Stored as fixed-point: kΩ × 1000
            s_mq6_r0 = (float)stored / 1000.0f;
            s_mq6_calibrated = true;
            ESP_LOGI(TAG, "MQ-6 R0 loaded from NVS: %.2f kΩ", s_mq6_r0);
        }
        nvs_close(h);
    }

    if (!s_mq6_calibrated) {
        ESP_LOGW(TAG, "MQ-6 R0 not yet calibrated — Rs/R0 ratio will report 1.0");
        ESP_LOGW(TAG, "Run mq6_calibrate_r0() in clean air after burn-in (24h+ on first power-up)");
    }

    s_mq6_initialized = true;
    ESP_LOGI(TAG, "MQ-6 ADC ready on GPIO%d (channel %d)", adc_gpio, s_mq6_channel);
    return ESP_OK;
}

// Insertion sort — n=64 takes ~10 µs on the S3, simpler than qsort and
// avoids pulling in stdlib for this one purpose.
static void mq6_sort_ascending(int *a, int n)
{
    for (int i = 1; i < n; i++) {
        int key = a[i];
        int j = i - 1;
        while (j >= 0 && a[j] > key) {
            a[j + 1] = a[j];
            j--;
        }
        a[j + 1] = key;
    }
}

static bool mq6_sample_voltage(float *v_at_pin_out, float *v_mq6_out)
{
    if (!s_mq6_initialized) return false;

    // Discard initial reads — the ADC sample-and-hold cap charges on the first
    // conversion after idle, so the first read or two can be off.
    int dummy;
    for (int i = 0; i < MQ6_ADC_WARMUP_READS; i++) {
        if (adc_oneshot_read(s_adc_handle, s_mq6_channel, &dummy) != ESP_OK) {
            return false;
        }
    }

    // Collect oversampled raw counts.
    int samples[MQ6_ADC_SAMPLES];
    for (int i = 0; i < MQ6_ADC_SAMPLES; i++) {
        if (adc_oneshot_read(s_adc_handle, s_mq6_channel, &samples[i]) != ESP_OK) {
            return false;
        }
    }

    // Trimmed mean: sort ascending, drop top-N and bottom-N, average the rest.
    // Robust to occasional outliers (WiFi TX, ESD, supply transients) that
    // could otherwise trigger a false safety alarm via a single bad reading.
    mq6_sort_ascending(samples, MQ6_ADC_SAMPLES);
    int kept = MQ6_ADC_SAMPLES - 2 * MQ6_ADC_TRIM;
    int total = 0;
    for (int i = MQ6_ADC_TRIM; i < MQ6_ADC_SAMPLES - MQ6_ADC_TRIM; i++) {
        total += samples[i];
    }
    int avg_raw = total / kept;

    // Apply per-chip ADC calibration if available (curve fitting from eFuse).
    int mv = 0;
    if (s_adc_cali) {
        if (adc_cali_raw_to_voltage(s_adc_cali, avg_raw, &mv) != ESP_OK) {
            return false;
        }
    } else {
        // Linear fallback for chips without eFuse calibration data.
        // Per-chip variation in actual range means this is only ~6% accurate.
        const int adc_max = (1 << 12) - 1;
        mv = (avg_raw * 3100) / adc_max;
    }

    // Sanity check — calibrated voltage outside the achievable range means
    // something went wrong (broken cable, ADC fault, EMI burst).  Reject the
    // sample rather than feed nonsense into the Rs/R0 calculation.
    if (mv < 0 || mv > MQ6_ADC_V_MAX_MV) {
        ESP_LOGW(TAG, "MQ-6 ADC voltage out of range: %d mV", mv);
        return false;
    }

    float v_pin = (float)mv / 1000.0f;
    float v_mq6 = v_pin / MQ6_VOLTAGE_DIVIDER_RATIO;  // recover original 0-5V swing

    if (v_at_pin_out) *v_at_pin_out = v_pin;
    if (v_mq6_out)    *v_mq6_out    = v_mq6;
    return true;
}

static float mq6_compute_rs(float v_mq6)
{
    // Avoid divide-by-zero; clamp v_mq6 to small positive value
    if (v_mq6 < 0.01f) v_mq6 = 0.01f;
    if (v_mq6 > MQ6_VCC) v_mq6 = MQ6_VCC;
    // Rs = RL * (Vcc - Vout) / Vout
    return MQ6_RL_KOHMS * (MQ6_VCC - v_mq6) / v_mq6;
}

mq6_data_t mq6_read(void)
{
    mq6_data_t out = { .valid = false, .rs_over_r0 = 1.0f, .rs_over_r0_x1000 = 1000, .calibrated = s_mq6_calibrated };
    float v_pin, v_mq6;
    if (!mq6_sample_voltage(&v_pin, &v_mq6)) return out;

    out.voltage = v_mq6;
    float rs = mq6_compute_rs(v_mq6);

    if (s_mq6_calibrated && s_mq6_r0 > 0.0f) {
        out.rs_over_r0 = rs / s_mq6_r0;
    } else {
        out.rs_over_r0 = 1.0f;
    }

    uint32_t scaled = (uint32_t)(out.rs_over_r0 * 1000.0f);
    if (scaled > 65535) scaled = 65535;
    out.rs_over_r0_x1000 = (uint16_t)scaled;
    out.valid = true;
    return out;
}

esp_err_t mq6_calibrate_r0(void)
{
    if (!s_mq6_initialized) return ESP_ERR_INVALID_STATE;

    float v_pin, v_mq6;
    if (!mq6_sample_voltage(&v_pin, &v_mq6)) return ESP_FAIL;

    float rs = mq6_compute_rs(v_mq6);
    if (rs <= 0.0f) return ESP_FAIL;

    s_mq6_r0 = rs;
    s_mq6_calibrated = true;

    nvs_handle_t h;
    if (nvs_open(NVS_NS_MQ6, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for MQ-6 R0 write");
        return ESP_FAIL;
    }
    uint32_t scaled = (uint32_t)(s_mq6_r0 * 1000.0f);
    nvs_set_u32(h, NVS_KEY_R0, scaled);
    nvs_commit(h);
    nvs_close(h);

    ESP_LOGI(TAG, "MQ-6 R0 calibrated: %.2f kΩ (V=%.3f V)", s_mq6_r0, v_mq6);
    return ESP_OK;
}
