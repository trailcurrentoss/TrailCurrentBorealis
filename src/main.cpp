#include <Arduino.h>
#include <Wire.h>
#include <DHT.h>
#include <Adafruit_SGP30.h>
#include <TwaiTaskBased.h>
#include <OtaUpdate.h>
#include <debug.h>
#include "wifiConfig.h"

#define DHT_PIN 2
#define DHT_TYPE DHT22
#define TEMP_OFFSET_C -2.78f
#define I2C_SDA 5
#define I2C_SCL 6
#define CAN_TX GPIO_NUM_9
#define CAN_RX GPIO_NUM_10
#define RGB_LED_PIN 21

void setLed(uint8_t r, uint8_t g, uint8_t b) {
    // WS2812 on this board uses GRB order; neopixelWrite sends bytes as-is
    neopixelWrite(RGB_LED_PIN, g, r, b);
}

#define CAN_ID_OTA_TRIGGER  0x00
#define CAN_ID_WIFI_CONFIG  0x01
#define CAN_ID_SENSOR_DATA  0x1F

enum CO2Level : uint8_t {
    CO2_LOW,      // < 400 ppm  (fresh air / outdoor baseline)
    CO2_NORMAL,   // 400–999 ppm
    CO2_HIGH,     // 1000–1999 ppm
    CO2_ALARM     // >= 2000 ppm
};

const char* co2LevelStr(CO2Level level) {
    switch (level) {
        case CO2_LOW:    return "Low";
        case CO2_NORMAL: return "Normal";
        case CO2_HIGH:   return "High";
        case CO2_ALARM:  return "ALARM";
        default:         return "Unknown";
    }
}

CO2Level getCO2Level(uint16_t eCO2) {
    if (eCO2 < 400)       return CO2_LOW;
    else if (eCO2 < 1000)  return CO2_NORMAL;
    else if (eCO2 < 2000)  return CO2_HIGH;
    else                    return CO2_ALARM;
}

// Global credential buffers - writable at runtime
char runtimeSsid[33] = {0};
char runtimePassword[64] = {0};

// OTA update handler (3-minute timeout)
OtaUpdate otaUpdate(180000, runtimeSsid, runtimePassword);

DHT dht(DHT_PIN, DHT_TYPE);
Adafruit_SGP30 sgp;

unsigned long lastReadTime = 0;
const unsigned long READ_INTERVAL = 2000;

// Cached at startup when MAC is readable (WiFi.macAddress() returns zeros later)
String cachedHostName;

void handleOtaTrigger(const uint8_t *data) {
    char updateForHostName[14];

    sprintf(updateForHostName, "esp32-%02X%02X%02X",
            data[0], data[1], data[2]);

    debugf("[OTA] Target: %s, This device: %s\n",
           updateForHostName, cachedHostName.c_str());

    if (cachedHostName.equals(updateForHostName)) {
        debugln("[OTA] Hostname matched - entering OTA mode");
        setLed(0, 0, 40);  // Blue: OTA mode
        otaUpdate.waitForOta();
        setLed(0, 40, 0);  // Green: back to normal
        debugln("[OTA] OTA mode exited - resuming normal operation");
    }
}

void onCanRx(const twai_message_t &msg) {
    if (msg.rtr) return;

    if (msg.identifier == CAN_ID_OTA_TRIGGER && msg.data_length_code >= 3) {
        debugln("[OTA] OTA trigger received");
        handleOtaTrigger(msg.data);
    }
    else if (msg.identifier == CAN_ID_WIFI_CONFIG && msg.data_length_code >= 1) {
        wifiConfig::handleCanMessage(msg.data, msg.data_length_code);
    }
    else {
        debugf("CAN RX: ID=0x%03X DLC=%d\n", msg.identifier, msg.data_length_code);
    }
}

void onCanTx(bool success) {
    if (!success) {
        debugln("[CAN] TX FAILED");
    }
}

void sendCanMessage(float tempC, float humidity, uint16_t tvoc, uint16_t eco2) {
    uint16_t humidityScaled = static_cast<uint16_t>(humidity * 100.0f);
    int tempCInt = static_cast<int>(tempC + 0.5f);
    int tempFInt = static_cast<int>(tempC * 9.0f / 5.0f + 32.0f + 0.5f);

    twai_message_t msg = {};
    msg.identifier = CAN_ID_SENSOR_DATA;
    msg.data_length_code = 8;
    // Bytes 0-3: same format as predecessor
    msg.data[0] = tempCInt;
    msg.data[1] = tempFInt;
    msg.data[2] = (humidityScaled >> 8) & 0xFF;
    msg.data[3] = humidityScaled & 0xFF;
    // Bytes 4-5: TVOC (big-endian)
    msg.data[4] = (tvoc >> 8) & 0xFF;
    msg.data[5] = tvoc & 0xFF;
    // Bytes 6-7: eCO2 (big-endian)
    msg.data[6] = (eco2 >> 8) & 0xFF;
    msg.data[7] = eco2 & 0xFF;

    TwaiTaskBased::send(msg);
}

void i2cScan() {
    debugln("I2C scan starting...");
    int found = 0;
    for (uint8_t addr = 1; addr < 127; addr++) {
        Wire.beginTransmission(addr);
        uint8_t err = Wire.endTransmission();
        if (err == 0) {
            debugf("  Found device at 0x%02X\n", addr);
            found++;
        }
    }
    debugf("I2C scan done, %d device(s) found\n", found);
}

void setup() {
    Serial.begin(115200);
    delay(1000);
    setLed(0, 40, 0);  // Green: running normally
    debugln("\n=== TrailCurrent Borealis ===");

    // WiFi config from NVS (provisioned via CAN bus)
    wifiConfig::init();
    wifiConfig::setRuntimeCredentialPtrs(runtimeSsid, runtimePassword);
    if (wifiConfig::loadCredentials(runtimeSsid, runtimePassword)) {
        debugln("[WiFi] Loaded credentials from NVS");
    } else {
        debugln("[WiFi] No credentials in NVS - OTA disabled until provisioned via CAN");
    }

    cachedHostName = otaUpdate.getHostName();
    debugf("[OTA] Device hostname: %s\n", cachedHostName.c_str());

    // Sensors
    dht.begin();

    Wire.begin(I2C_SDA, I2C_SCL);
    i2cScan();

    if (!sgp.begin(&Wire)) {
        debugln("ERROR: SGP30 sensor not found");
    } else {
        debugln("SGP30 sensor initialized");
    }

    // CAN bus
    TwaiTaskBased::onReceive(onCanRx);
    TwaiTaskBased::onTransmit(onCanTx);
    if (TwaiTaskBased::begin(CAN_TX, CAN_RX, 500000)) {
        debugln("[CAN] Bus initialized");
    } else {
        debugln("[CAN] ERROR: Bus init failed");
    }

    debugln("=== Setup Complete ===\n");
}

void loop() {
    wifiConfig::checkTimeout();

    unsigned long now = millis();
    if (now - lastReadTime < READ_INTERVAL) return;
    lastReadTime = now;

    float humidity = dht.readHumidity();
    float tempC = dht.readTemperature() + TEMP_OFFSET_C;
    float tempF = tempC * 9.0f / 5.0f + 32.0f;

    debugln("--- Sensor Readings ---");

    if (isnan(humidity) || isnan(tempC)) {
        debugln("DHT22: read failed");
    } else {
        debugf("DHT22: %.1fC / %.1fF, Humidity: %.1f%%\n", tempC, tempF, humidity);
    }

    uint16_t tvoc = 0;
    uint16_t eco2 = 0;

    if (sgp.IAQmeasure()) {
        tvoc = sgp.TVOC;
        eco2 = sgp.eCO2;
        const char* iaq;
        if (tvoc < 65)        iaq = "Excellent";
        else if (tvoc < 220)   iaq = "Good";
        else if (tvoc < 660)   iaq = "Moderate";
        else if (tvoc < 2200)  iaq = "Poor";
        else                    iaq = "Unhealthy";
        CO2Level co2Level = getCO2Level(eco2);
        debugf("SGP30: TVOC %d ppb, eCO2 %d ppm (%s), IAQ: %s\n",
               tvoc, eco2, co2LevelStr(co2Level), iaq);
    } else {
        debugln("SGP30: read failed");
    }

    if (sgp.IAQmeasureRaw()) {
        debugf("SGP30 Raw: H2 %d, Ethanol %d\n", sgp.rawH2, sgp.rawEthanol);
    }

    // Set humidity compensation on SGP30 if DHT22 reading is valid
    if (!isnan(humidity) && !isnan(tempC)) {
        float absHumidity = 216.7f * (humidity / 100.0f) * 6.112f *
            exp(17.62f * tempC / (243.12f + tempC)) / (273.15f + tempC);
        uint16_t ah_scaled = (uint16_t)(absHumidity * 256.0f);
        sgp.setHumidity(ah_scaled);

        sendCanMessage(tempC, humidity, tvoc, eco2);
    }

    debugln("");
}
