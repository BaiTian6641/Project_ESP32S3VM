# Arduino Test Sketches (ESP32-S3)

This folder contains Arduino sketches used to validate the GUI simulator peripheral bridge.

## Sketch: `esp32s3_bridge_tester`

Path:
- `tests/arduino/esp32s3_bridge_tester/esp32s3_bridge_tester.ino`

### What it does
- Periodically emits bridge request lines over `Serial`:
  - `[PERIPH][I2C] {...}`
  - `[PERIPH][SPI] {...}`
  - `[PERIPH][UART] {...}`
- Reads and validates bridge response lines:
  - `[PERIPH][I2C][RSP] {...}`
  - `[PERIPH][SPI][RSP] {...}`
  - `[PERIPH][UART][RSP] {...}`
- Prints periodic stats counters (`sent`, `rsp ok`, `rsp err`).

### Simulator config assumptions
The payload values match defaults in:
- `gui-esp32s3-simulator/peripherals/peripherals.example.json`

Specifically:
- I2C: `controller=i2c0`, `address=0x40` (SHT21)
- SPI: `controller=spi2`, `chip_select=0` (SPI flash sim)
- UART: `controller=uart1`, `unit=1` (UART loopback sim)

### How to run
1. Open sketch in Arduino IDE (or Arduino CLI) with ESP32 board package.
2. Select an ESP32-S3 board profile.
3. Build/upload to real hardware (or use as reference for QEMU/firmware output format).
4. In the GUI simulator, start peripherals from the `Peripherals` tab.
5. Watch the GUI Serial tab for `[PERIPH]` request/response flow and the sketch stats.

## Arduino CLI compile script

Build script:
- `tests/arduino/build_esp32s3_bridge_tester.sh`

Default board target:
- `esp32:esp32:esp32s3`

Run:
1. Install `arduino-cli`
2. From repo root:
  - `bash tests/arduino/build_esp32s3_bridge_tester.sh`

Optional override:
- `FQBN=esp32:esp32:esp32s3 bash tests/arduino/build_esp32s3_bridge_tester.sh`

## CI

Arduino sketch compile workflow:
- `.github/workflows/arduino-bridge-sketch.yml`
