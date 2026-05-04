# Parts-Arrival TODO Checklist

Tasks to complete once the DFRobot sensors and Waveshare board arrive. The firmware is already written and builds clean — these are the hardware, calibration, and validation steps that can only be done with physical parts.

## Hardware to assemble

- [ ] **MQ-6 voltage divider**: solder 10kΩ + 15kΩ resistors between MQ-6 SIG and IO3, with 15kΩ to GND. See wiring diagram in [`sensor-wiring.md`](sensor-wiring.md).
- [ ] **MQ-6 ADC noise filter**: add 100nF ceramic cap from IO3 to GND.
- [ ] **Ribbon cable** (or carrier PCB): wire per [`borealis-ribbon-cable.svg`](borealis-ribbon-cable.svg).
- [ ] **Verify DFRobot Gravity cable wire colors** match the assumption in `sensor-wiring.md` (red=VCC, black=GND, blue/yellow=SDA/SIG, white/green=SCL). Update the doc if vendor swapped them.
- [ ] **CAN bus termination**: close H1 jumper on the Waveshare board if Borealis is at one end of the bus.
- [ ] **Mount MQ-6 low** (10-30 cm off the floor) — propane is heavier than air and pools.
- [ ] **Mount SCD41 at sleeping-zone height** (1-1.5 m off the floor).

## First power-up validation

- [ ] **VIN power**: feed 12V from RV house battery (or 5V via USB-C) and verify it boots.
- [ ] **Multimeter check on the breakout header**: Pin 1 = 3.3V, Pin 2 = 5V, IO3 < 3.0V (if it shows 5V, the divider isn't installed and the ADC is already damaged).
- [ ] **Serial log check**: confirm all four sensors attach. Look for these lines:
  - `SCD41 attached (0x62)`
  - `SGP40 attached (0x59)`
  - `SEN0466 CO sensor attached (0x74)`
  - `MQ-6 ADC ready on GPIO3 (channel 2)`
- [ ] **Hostname** is logged at boot: format `esp32-XXYYZZ`.
- [ ] **CAN frames** are visible on the bus (use Headwaters or a CAN sniffer). Expect 0x1F (environmental) and 0x20 (safety) every 1 s.

## MQ-6 burn-in and R0 calibration

- [ ] Leave the unit powered in clean air (open garage, outdoor, well-ventilated room) for **24-48 hours** continuously. Do not trust gas readings during this window.
- [ ] After burn-in, trigger `mq6_calibrate_r0()` — currently a code-side function call. Either:
  - Add a temporary CAN message handler that calls it, or
  - Add a button-press handler on GPIO0 (the boot button), or
  - Manually call it once via a debug serial command path.
- [ ] Confirm the log shows `MQ-6 R0 calibrated: X.XX kΩ` and that R0 persists across reboots (`MQ-6 R0 loaded from NVS`).
- [ ] Verify Rs/R0 reads ~1.0 in clean air after calibration. If it drifts < 1.0 quickly without gas present, R0 was captured under poor conditions — recalibrate.

## SCD41 sanity check

- [ ] Take the device outdoors briefly. CO2 should drop toward ~420 ppm (current outdoor baseline).
- [ ] Compare reported temperature against a reference thermometer; ±0.8°C is in spec but anything wider suggests a defective unit.
- [ ] **Decide on auto-self-calibration**:
  - Toy hauler that's parked outdoors when not in use → leave ASC on (default).
  - Always-occupied installation → call `scd41_set_automatic_self_calibration(false)` to turn off (function not yet in driver — add when needed).

## SEN0466 CO sensor warmup

- [ ] First power-up after factory shipping: leave running for **~1 hour** before trusting low-ppm readings. Electrochemical cells take time to stabilize.
- [ ] Confirm 0 ppm baseline in clean air.
- [ ] *Optional:* test with a controlled CO source (camp stove exhaust briefly directed at sensor) to confirm response. Be careful — actual CO is dangerous.

## SGP40 production VOC algorithm

The current [`sgp40_estimate_index()`](../main/sensors.c) is a placeholder with a 17-minute baseline. Replace it for production:

- [ ] Add Sensirion's [`embedded-gas-index-algorithm`](https://github.com/Sensirion/gas-index-algorithm) (BSD-3 licensed) to the project.
  - Either as a managed component (if Sensirion publishes one), or
  - Drop the C source files into `components/sensirion_gas_index/` and add a `CMakeLists.txt`.
- [ ] Replace the stub function body in `sensors.c` with calls to:
  - `GasIndexAlgorithm_init(&params, GasIndexAlgorithm_ALGORITHM_TYPE_VOC)`
  - `GasIndexAlgorithm_process(&params, sraw, &voc_index)`
- [ ] Confirm baseline settles to ~100 after 24 hours of operation.

## CAN integration with Headwaters

- [ ] Provision WiFi credentials via CAN ID 0x01 (see chunked protocol in README).
- [ ] Trigger discovery via CAN ID 0x02; verify Borealis advertises on mDNS as `_trailcurrent._tcp` with `type=borealis canid=0x1F`.
- [ ] Verify Headwaters confirms via `GET /discovery/confirm` (look for `Discovery confirmed by Headwaters` in the serial log).
- [ ] Test OTA upload: `curl -X POST http://esp32-XXYYZZ.local/ota --data-binary @build/borealis.bin`. After the device reboots, the firmware version frame on CAN ID 0x04 should reflect the new version.

## Threshold validation

The README documents these thresholds; verify they fire correctly in the field:

- [ ] **CO ≥ 70 ppm** → bit 0 set in safety frame byte 4.
- [ ] **CO ≥ 200 ppm** → bit 1 set.
- [ ] **MQ-6 Rs/R0 < 0.5** → bit 2 set (LPG warning).
- [ ] **MQ-6 Rs/R0 < 0.3** → bit 3 set (LPG alarm).
- [ ] **CO2 ≥ 1500 ppm** → bit 4 set.
- [ ] **CO2 ≥ 2500 ppm** → bit 5 set.
- [ ] **VOC index ≥ 400** → bit 6 set.

Easy way to test without real gas: temporarily inject test values via a debug CAN message or hardcode a high reading for one cycle, and confirm Headwaters' alarm path triggers.

## Power and thermal validation

- [ ] At **12V VIN**: measure current draw. Expect ~150-250 mA average (mostly the MQ-6 heater).
- [ ] After 1 hour of operation, **feel the buck regulator** (XL1509 area on the board). Should be warm, not painfully hot. > ~65°C calls for a heat sink or duty-cycling the MQ-6.
- [ ] *Optional:* repeat at 24V VIN if you'll be running on a 24V system. Expect more regulator heat.

## Documentation updates after validation

- [ ] Update `sensor-wiring.md` if any wire colors / connector orientations differ from what's documented.
- [ ] Update the threshold tables in `README.md` if field testing suggests different alarm levels.
- [ ] Photograph the assembled unit and add to `DOCS/images/` for the README header.
- [ ] Note any sensor quirks observed (e.g. SGP40 spike on cold boot, SCD41 needing pressure compensation at high altitude) in this checklist's "Known issues" section below.

## Known issues / observations

*(Fill in during commissioning)*

- _none yet_
