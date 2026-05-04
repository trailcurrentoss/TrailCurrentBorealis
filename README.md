# TrailCurrent Borealis

![TrailCurrent Borealis](DOCS/images/borealis_main.png)

Environmental and safety sensor node for toy haulers, RVs, and similar enclosed living spaces. Measures temperature, humidity, real CO2, VOC index, carbon monoxide, and propane / LPG levels, then broadcasts readings over a CAN bus. Supports over-the-air (OTA) firmware updates, WiFi credential provisioning via CAN, and network discovery for integration with TrailCurrent Headwaters.

## Hardware

- **MCU board:** [Waveshare ESP32-S3-RS485-CAN](https://www.waveshare.com/esp32-s3-rs485-can.htm)
  - ESP32-S3R8 (8MB PSRAM, external 16MB flash)
  - Built-in isolated CAN transceiver (TJA1051) on GPIO15/16
  - Built-in isolated RS485 transceiver (unused by Borealis)
  - External PCF85063AT RTC
  - 7-36V DC input or 5V USB-C
- **CO2 / temp / humidity:** DFRobot SEN0536 (Sensirion SCD41, photoacoustic NDIR)
- **VOC trending:** DFRobot SEN0394 (Sensirion SGP40, VOC Index 1-500)
- **Carbon monoxide:** DFRobot SEN0466 (factory-calibrated electrochemical, 0-1000 ppm)
- **Propane / LPG:** DFRobot SEN0131 (MQ-6 with adjustable potentiometer)

### Pin Assignments

| GPIO | Function |
|------|----------|
| 5 | I2C SDA (SCD41, SGP40, SEN0466) |
| 6 | I2C SCL (SCD41, SGP40, SEN0466) |
| 3 | MQ-6 analog input (ADC1_CH2, via 10k/15k divider) |
| 15 | CAN TX (internal transceiver) |
| 16 | CAN RX (internal transceiver) |

See [`DOCS/sensor-wiring.md`](DOCS/sensor-wiring.md) for full wiring details and the [ribbon-cable diagram](DOCS/borealis-ribbon-cable.svg). When the parts arrive, work through [`DOCS/arrival-todos.md`](DOCS/arrival-todos.md) for the assembly, calibration, and validation checklist.

### Circuit Notes

- KiCad schematic and PCB files are in the `EDA/` directory
- The MQ-6 sensor's analog output swings 0-5V; a 10kΩ + 15kΩ resistor divider scales it to 0-3V at the ADC pin
- Add a 100nF cap from the ADC pin to GND for noise filtering near the heater

## Building

This project uses [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/get-started/index.html) (v5.x).

```bash
# Configure (first time only)
idf.py set-target esp32s3

# Build
idf.py build

# Flash via USB
idf.py flash

# Monitor serial output
idf.py monitor

# Build, flash, and monitor in one step
idf.py build flash monitor
```

### OTA Upload

```bash
curl -X POST http://<hostname>.local/ota --data-binary @build/borealis.bin
```

## Architecture

| File | Purpose |
|------|---------|
| `main/main.c` | Sensor reading loop, TWAI task, CAN frame transmission |
| `main/sensors.c/h` | I²C drivers for SCD41, SGP40, SEN0466; ADC reading and Rs/R0 calculation for MQ-6 |
| `main/wifi_config.c/h` | NVS-backed WiFi credentials and STA connect/disconnect |
| `main/ota.c/h` | HTTP POST `/ota` server, runs in dedicated task |
| `main/discovery.c/h` | mDNS-based network discovery for Headwaters registration |

### CAN Bus Task

The TWAI (CAN) driver runs in a dedicated FreeRTOS task with alert-based message handling. It uses a dual-state transmission model:

- **TX_ACTIVE** (1000 ms period): Normal operation when peers are detected on the bus
- **TX_PROBING** (2000 ms period): Slow probe when no peers are ACKing, reduces bus noise

The task automatically transitions between states based on TX success/failure and incoming messages. Bus-off recovery is handled via TWAI alerts. This pattern is shared with all other TrailCurrent ESP-IDF modules — see `TrailCurrentReservoir`, `TrailCurrentSwitchback`, etc.

## CAN Bus Protocol

All communication uses a 500 kbps CAN bus. The device transmits sensor data and receives OTA / WiFi / discovery commands.

### Environmental Data (TX)

**CAN ID:** `0x1F` | **DLC:** 8 | **Period:** 1000 ms

| Byte | Content |
|------|---------|
| 0 | Temperature (°C, signed int8, rounded) |
| 1 | Temperature (°F, signed int8, rounded) |
| 2-3 | Humidity (big-endian uint16, value × 100, e.g. 4523 = 45.23%) |
| 4-5 | Real CO2 in ppm (big-endian uint16, NDIR measurement from SCD41) |
| 6-7 | VOC Index (big-endian uint16, range 1-500, baseline ≈ 100) |

### Safety Data (TX)

**CAN ID:** `0x20` | **DLC:** 8 | **Period:** 1000 ms

| Byte | Content |
|------|---------|
| 0-1 | CO concentration in ppm (big-endian uint16) |
| 2-3 | LPG / propane Rs/R0 ratio × 1000 (big-endian uint16; lower = more gas) |
| 4 | Alarm flags (bitmask) |
| 5-7 | Reserved |

Alarm flag bits:
- `0x01` CO warning (≥ 70 ppm)
- `0x02` CO alarm (≥ 200 ppm)
- `0x04` LPG warning (Rs/R0 < 0.5)
- `0x08` LPG alarm (Rs/R0 < 0.3)
- `0x10` CO2 warning (≥ 1500 ppm)
- `0x20` CO2 alarm (≥ 2500 ppm)
- `0x40` VOC alarm (Index ≥ 400)

### OTA Trigger (RX)

**CAN ID:** `0x00` | **DLC:** 3+

| Byte | Content |
|------|---------|
| 0-2 | Target MAC suffix (e.g., `F2 7E 6C` for hostname `esp32-F27E6C`) |

When the MAC matches, the device connects to its configured WiFi network, starts an HTTP server at `/ota`, and waits for a firmware upload for 3 minutes before returning to normal operation.

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

### Discovery Trigger (RX)

**CAN ID:** `0x02` | **DLC:** any (broadcast)

When received, the device connects to WiFi and advertises itself via mDNS (`_trailcurrent._tcp`) with TXT records:

| Key | Value |
|-----|-------|
| `type` | `borealis` |
| `canid` | `0x1F` |
| `fw` | firmware version |

Headwaters confirms registration by calling `GET /discovery/confirm`. The discovery window is 3 minutes.

### Firmware Version Report (TX)

**CAN ID:** `0x04` | **DLC:** 6

Sent once on boot after CAN initialization, then again whenever a peer is first detected. Reports the running firmware version so Headwaters can track what each device is running.

| Byte | Content |
|------|---------|
| 0-2 | Last 3 bytes of device WiFi MAC (matches hostname `esp32-XXYYZZ`) |
| 3 | Version major |
| 4 | Version minor |
| 5 | Version patch |

## Air Quality and Safety Thresholds

### Real CO2 (SCD41, NDIR)

| ppm | Level |
|-----|-------|
| < 800 | Fresh / well-ventilated |
| 800-1499 | Normal indoor |
| 1500-2499 | Stuffy / drowsiness possible |
| ≥ 2500 | Alarm |

### VOC Index (SGP40)

| Index | Meaning |
|-------|---------|
| 1-99 | Better than recent baseline |
| 100 | Typical indoor air (24-hour running average) |
| 101-249 | Worse than baseline |
| 250-399 | Significant pollution event |
| 400-500 | Severe pollution event |

### Carbon Monoxide (SEN0466)

| ppm | Level |
|-----|-------|
| < 35 | Normal |
| 35-69 | Elevated |
| 70-199 | Warning |
| ≥ 200 | Alarm |

### Propane / LPG (MQ-6, Rs/R0 ratio)

| Rs/R0 | Level |
|-------|-------|
| > 0.7 | Clean air |
| 0.5-0.7 | Trace |
| 0.3-0.5 | Warning |
| < 0.3 | Alarm |

The MQ-6 outputs a resistance ratio rather than ppm; calibrating to absolute ppm requires tank-test exposure. For leak detection the ratio is what matters: a sharp drop from baseline indicates propane present.

## OTA Updates

1. Provision WiFi credentials via CAN (one-time setup, stored in NVS)
2. Send an OTA trigger message on CAN ID `0x00` with the target device's MAC suffix
3. The device connects to WiFi
4. Upload firmware via HTTP: `curl -X POST http://<hostname>.local/ota --data-binary @build/borealis.bin`
5. On success, the device reboots with new firmware
6. On timeout (3 minutes), the device disconnects WiFi and resumes normal operation

The device hostname is printed to serial at boot (format: `esp32-XXYYZZ`).

## License

[MIT](LICENSE)
