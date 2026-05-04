# Sensor Wiring Guide

## Sensor Overview

Borealis monitors the four most common dangers in an enclosed living space (toy hauler, RV bedroom, sealed cabin) using four DFRobot Gravity sensor modules. All four are wired to a single 20-pin IDC ribbon cable that plugs into the Waveshare ESP32-S3-RS485-CAN's user header.

| Module | Measures | DFRobot SKU | Interface | Address |
|--------|----------|-------------|-----------|---------|
| SCD41 | Real CO2 (NDIR) + temperature + humidity | SEN0536 | I²C | 0x62 |
| SGP40 | VOC index (1-500) | SEN0394 | I²C | 0x59 |
| SEN0466 | Carbon monoxide (factory-calibrated electrochemical) | SEN0466 | I²C | 0x74 |
| MQ-6 | Propane / LPG / butane | SEN0131 | Analog | — |

Three of the four are I²C, sharing one bus. The MQ-6 is analog and uses one ADC pin.

## Wiring Diagram

See [`borealis-ribbon-cable.svg`](borealis-ribbon-cable.svg) (or [PNG](borealis-ribbon-cable.png)) for the complete pin layout.

### Pin Assignments

| IDC Pin | GPIO | Signal | Connects to |
|---------|------|--------|-------------|
| 1 | — | 3.3V | SCD41, SGP40, SEN0466 VCC (red wires) |
| 2 | — | 5V | MQ-6 VCC only |
| 3 | — | GND | All sensor GNDs |
| 4 | — | GND | All sensor GNDs |
| 9 | IO3 | MQ-6 analog | MQ-6 SIG (via 10k+15k divider) |
| 13 | IO5 | I²C SDA | SCD41 / SGP40 / SEN0466 SDA |
| 15 | IO6 | I²C SCL | SCD41 / SGP40 / SEN0466 SCL |

All other pins are unused or reserved for the board's USB.

### Power

- **3.3V (Pin 1)** powers the three I²C sensors. They have onboard regulators that accept 3.3V or 5V; using 3.3V keeps the LDO load lower and reduces noise.
- **5V (Pin 2)** powers the MQ-6 *only*. The MQ-6 heater requires 5V to reach the correct sensing temperature; running it at 3.3V gives meaningless readings.
- **GND (Pins 3, 4)** are tied together internally. Use either one — it's the same net.

### MQ-6 Voltage Divider (CRITICAL)

The MQ-6 module's SIG pin outputs 0-5V proportional to gas concentration. Connecting it directly to a 3.3V GPIO will damage the ESP32-S3 ADC.

```
MQ-6 SIG ──[ R1 = 10kΩ ]──┬──► IO3 (ADC)
                          │
                       [ R2 = 15kΩ ]
                          │
                         GND
```

Ratio = 15 / (10 + 15) = 0.6, giving 5.0V × 0.6 = 3.0V max at the ADC pin (well under the ESP32-S3 ADC's 3.1V limit at 12 dB attenuation). The firmware multiplies the measured voltage by 1/0.6 to recover the original 0-5V signal before computing Rs/R0.

Optional but recommended: a 100 nF ceramic cap from IO3 to GND for noise filtering near the heater.

### I²C Bus Topology

All three I²C sensors share GPIO5 (SDA) and GPIO6 (SCL). Each sensor's onboard regulator and pull-ups handle the bus electrically, and the addresses don't conflict:

| Address | Sensor |
|---------|--------|
| 0x59 | SGP40 |
| 0x62 | SCD41 |
| 0x74 | SEN0466 (default; configurable via DIP switch) |

If you use the Adafruit STEMMA-QT versions instead of DFRobot Gravity, you can chain them via the QT cables — same bus, no rewiring.

### Per-Sensor Wire Map (DFRobot Gravity 4-pin JST-PH)

DFRobot Gravity I²C cables use this color order (verify against your specific cable — vendors occasionally swap):

| DFRobot wire | Signal | IDC pin |
|--------------|--------|---------|
| Red | VCC (3.3V or 5V) | 1 (or 2 for MQ-6) |
| Black | GND | 3 or 4 |
| Yellow / Blue | SDA | 13 |
| White / Green | SCL | 15 |

For the MQ-6 (3-pin Gravity analog cable):

| DFRobot wire | Signal | IDC pin |
|--------------|--------|---------|
| Red | VCC (5V) | 2 |
| Black | GND | 3 or 4 |
| Blue / Yellow | SIG | 9 (via divider) |

## CAN Bus Wiring

The CAN bus uses the onboard TJA1051 transceiver (galvanically isolated from the MCU side):

| J5 terminal | Connection |
|-------------|------------|
| CANH | CAN bus high wire |
| CANL | CAN bus low wire |

A 120-ohm termination resistor is required at each end of the bus. The board has an onboard 120Ω termination jumper (H1) — close it on the two devices at each end of the bus, leave it open on intermediate nodes.

## Sensor Mounting

### Placement (toy hauler / sleeping area)

| Sensor | Best location | Rationale |
|--------|---------------|-----------|
| SCD41 (CO2) | Within 1-2 m of the sleeping zone, 1-1.5 m off the floor | Wants air representative of what people are breathing |
| SGP40 (VOC) | Same enclosure as SCD41 — needs RH/T compensation from it | Cross-compensation is more accurate when the two sensors share airflow |
| SEN0466 (CO) | Mid-height (1-1.5 m), away from cooking surfaces | Vehicle exhaust CO drifts through the whole space; mid-height is representative |
| MQ-6 (LPG) | **Low — near floor level** (0.1-0.3 m off the floor) | Propane is heavier than air and pools at floor level. Mounting it high will miss leaks |

### General Guidelines

1. **Ventilation** — don't seal sensors inside an airtight enclosure. They need ambient air to flow past them. A perforated case is fine.
2. **Avoid direct heat / sun** — temperature swings degrade accuracy and shorten sensor lifetime, especially the MQ-6 heater.
3. **Avoid spray contaminants** — silicones (gasket sealers, hand sanitizer), strong solvents, and chemical cleaners poison MOX gas sensors permanently. Keep the MQ-6 and SGP40 away from these.
4. **Cable length** — keep the ribbon under 1 m for clean I²C signaling at 100 kHz. Beyond ~1.5 m you may need to drop to 50 kHz in the firmware.

## First-Time Calibration

### MQ-6 Burn-in

The MQ-6's metal-oxide film needs heat-cycling to stabilize:

1. Power the unit on in clean air (e.g. open garage, outdoor air, well-ventilated room) and **leave it powered for 24-48 hours**. Don't trust readings during this window.
2. After burn-in, with the firmware running and serial connected, trigger `mq6_calibrate_r0()` to capture the clean-air baseline. (At present this is a function call you'd add manually or via a CAN message; a future build will automate it after the first 24h.)
3. Once calibrated, R0 is stored in NVS and persists across reboots. Re-calibrate yearly or after a long power-off period.

From a cold boot in subsequent sessions, the heater takes ~2-3 minutes to reach correct sensing temperature. Initial readings during this warmup will look like elevated gas — ignore the first 3 minutes after every power cycle.

### SCD41 Self-Calibration

The SCD41 has automatic self-calibration enabled by default. It assumes that during a 7-day sliding window, the lowest CO2 reading represents fresh air (~400 ppm) and corrects its zero point. For a unit installed in an always-occupied space (e.g. a permanent tow vehicle), turn this off via `scd41_set_automatic_self_calibration(false)` because the assumption breaks. For a toy hauler that's parked outdoors when not in use, leave it on.

### SGP40 Baseline

The firmware's stub VOC index estimator builds a running baseline over ~17 minutes of operation. For long-term accurate VOC trending, replace `sgp40_estimate_index()` with Sensirion's official `gas_index_algorithm` library (BSD-3 licensed, drop in via component manager). The official algorithm uses a 24-hour adaptive baseline and is what the SGP40 datasheet specifies.

## Troubleshooting

### "SCD41 attach failed" at boot

1. **Check the I²C bus** — measure SDA/SCL at the sensor with a scope. They should idle at 3.3V.
2. **Address conflict** — scan the bus with `i2cdetect`-equivalent code; verify 0x62 responds.
3. **Power** — confirm the SCD41 sees 3.3V on its VCC pin (multimeter at the breakout).
4. **Cable length / pull-ups** — the SCD41 has internal pull-ups, but adding external 4.7 kΩ pull-ups on SDA/SCL helps with longer cables or noisy environments.

### CO sensor reading 0 or NaN

1. **First 3 minutes after power-up** — electrochemical sensors stabilize over 1-3 minutes. Brand-new sensors need up to 1 hour.
2. **Check active mode** — verify the firmware sent the 0x78 0x03 command at boot. Look for `SEN0466 set to active acquisition mode` in the log.
3. **Cable / connector** — DFRobot Gravity I²C connectors are keyed; verify it's seated.
4. **Address conflict** — if you have another sensor at 0x74, change the SEN0466's DIP switch.

### MQ-6 always at full deflection (Rs/R0 < 0.3)

1. **You haven't calibrated R0 yet** — uncalibrated readings default to 1.0; if you're seeing very low ratios it means R0 was captured under non-clean-air conditions. Recalibrate in clean air.
2. **Voltage divider missing or wrong** — measure the voltage at IO3 with a multimeter. Should be < 3.0V. If it's at 5V, the divider isn't installed and the ADC is already damaged.
3. **Heater not at temperature** — first 3 minutes after cold boot will read high. Wait.
4. **Gas actually present** — if you smell propane, the sensor is doing its job. Ventilate.

### CAN Bus Not Transmitting

1. **Check J5 wiring** — CANH to CANH, CANL to CANL.
2. **Check termination** — 120Ω at each end of the bus only. The board's H1 jumper enables onboard termination.
3. **Check serial output** — look for `TWAI bus-off` or `error passive` messages. Probing mode (`entering slow probe`) is normal when no other CAN nodes are on the bus yet.
4. **Verify peer ACK** — TWAI normal mode requires at least one other CAN node to ACK frames. If Borealis is the only node on the bus, it'll cycle TX_ACTIVE → TX_PROBING.

## Modifying Pin Assignments

If you need to use different GPIOs (different sensor breakout, custom carrier PCB), edit the pin defines at the top of [`main/main.c`](../main/main.c):

```c
#define I2C_SDA_PIN          5
#define I2C_SCL_PIN          6
#define MQ6_ADC_GPIO         3   // must be on ADC1 (GPIO1-10)
```

CAN pins are hardwired to the onboard transceiver; do **not** change `CAN_TX_PIN` (15) or `CAN_RX_PIN` (16).

**Avoid these GPIOs:**
- GPIO 0: Boot strapping pin (also onboard button)
- GPIO 15: CAN TX (used by onboard transceiver)
- GPIO 16: CAN RX (used by onboard transceiver)
- GPIO 17 / 18: RS485 TX / RX (used by onboard transceiver)
- GPIO 19-20: USB D-/D+ (used for USB CDC console)
- GPIO 21: RS485 EN (used by onboard transceiver)
- GPIO 26-32: Internal QSPI flash and PSRAM (reserved on ESP32-S3R8)
