# TrailCurrent Borealis

An ESP32-S3-based environmental sensor node that monitors temperature, humidity, TVOC, and eCO2, broadcasting readings over a CAN bus. Supports over-the-air (OTA) firmware updates and WiFi credential provisioning via CAN.

## Hardware

- **MCU:** [Waveshare ESP32-S3-Zero](https://www.waveshare.com/product/arduino/boards-kits/esp32-s3/esp32-s3-zero.htm) (CPU clocked at 80 MHz for reduced power consumption)
- **Temperature/Humidity:** SHT31-D sensor (I2C)
- **Air Quality:** SGP30 TVOC and eCO2 sensor (I2C)
- **CAN Transceiver:** SN65HVD230DR (3.3V, 500 kbps)
- **Status LED:** Onboard WS2812 RGB LED (GPIO 21)

### Pin Assignments

| Pin | GPIO | Function |
|-----|-------|----------|
| 8 | GPIO5 | I2C SCL (SGP30, SHT31-D) |
| 9 | GPIO6 | I2C SDA (SGP30, SHT31-D) |
| 12 | GPIO9 | CAN TX |
| 13 | GPIO10 | CAN RX |
| - | GPIO21 | Onboard RGB LED |

### Circuit Notes

- KiCad schematic and PCB files are in the `EDA/` directory

## Building

This project uses [PlatformIO](https://platformio.org/).

```bash
# Build
pio run

# Upload via USB
pio run --target upload

# Monitor serial output
pio device monitor
```

## CAN Bus Protocol

All communication uses a 500 kbps CAN bus. The device transmits sensor data and receives OTA/WiFi configuration commands.

### Sensor Data (TX)

**CAN ID:** `0x1F` | **DLC:** 8

| Byte | Content |
|------|---------|
| 0 | Temperature (C, rounded integer) |
| 1 | Temperature (F, rounded integer) |
| 2-3 | Humidity (big-endian, value x 100) |
| 4-5 | TVOC in ppb (big-endian) |
| 6-7 | eCO2 in ppm (big-endian) |

Sensor data is transmitted every 2 seconds when both SHT31-D and SGP30 readings are valid. The SGP30 receives humidity compensation from the SHT31-D for improved accuracy.

### OTA Trigger (RX)

**CAN ID:** `0x00` | **DLC:** 3+

Send the last 3 bytes of the target device's MAC address to trigger OTA mode on that specific device.

| Byte | Content |
|------|---------|
| 0-2 | Target MAC bytes (e.g., `F2 7E 6C` for hostname `esp32-F27E6C`) |

When triggered, the device connects to its configured WiFi network and listens for ArduinoOTA uploads for 3 minutes before returning to normal operation.

### WiFi Configuration (RX)

**CAN ID:** `0x01` | **DLC:** varies

WiFi credentials are provisioned over CAN using a chunked protocol. Credentials are stored in NVS (non-volatile storage) and persist across reboots.

| Byte 0 | Message Type |
|--------|-------------|
| `0x01` | Start: contains SSID length, password length, chunk counts |
| `0x02` | SSID chunk: 6-byte payload with chunk index |
| `0x03` | Password chunk: 6-byte payload with chunk index |
| `0x04` | End: contains XOR checksum for validation |

The protocol includes a 5-second timeout — if chunks stop arriving, the state resets.

## Status LED

The onboard RGB LED indicates the device state:

| Color | State |
|-------|-------|
| Green | Normal operation |
| Blue | OTA update mode (waiting for firmware upload) |

## OTA Updates

1. Provision WiFi credentials via CAN (one-time setup, stored in NVS)
2. Send an OTA trigger message on CAN ID `0x00` with the target device's MAC suffix
3. The LED turns blue and the device connects to WiFi
4. Upload firmware using ArduinoOTA within the 3-minute window
5. On success, the device reboots with new firmware and the LED returns to green
6. On timeout, the device disconnects WiFi and resumes normal operation

The device hostname is printed to serial at boot (format: `esp32-XXYYZZ`).

## Air Quality Levels

### TVOC (Indoor Air Quality)

| Range (ppb) | Rating |
|-------------|--------|
| < 65 | Excellent |
| 65-219 | Good |
| 220-659 | Moderate |
| 660-2199 | Poor |
| >= 2200 | Unhealthy |

### eCO2

| Range (ppm) | Level |
|-------------|-------|
| < 400 | Low (fresh air) |
| 400-999 | Normal |
| 1000-1999 | High |
| >= 2000 | Alarm |

## Dependencies

| Library | Source |
|---------|--------|
| [ESP32ArduinoDebugLibrary](https://github.com/trailcurrentoss/ESP32ArduinoDebugLibrary) | TrailCurrent |
| [TwaiTaskBasedLibrary](https://github.com/trailcurrentoss/TwaiTaskBasedLibraryWROOM32) | TrailCurrent |
| [OtaUpdateLibrary](https://github.com/trailcurrentoss/OtaUpdateLibraryWROOM32) | TrailCurrent |
| [Adafruit SHT31 Library](https://github.com/adafruit/Adafruit_SHT31) | Adafruit |
| [Adafruit SGP30 Sensor](https://github.com/adafruit/Adafruit_SGP30) | Adafruit |

## License

[MIT](LICENSE)
