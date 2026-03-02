# Arduino Test Sketches (ESP32-S3)

This folder contains Arduino sketches used to validate the GUI simulator's
peripheral simulation system. Each sketch exercises a specific peripheral
simulator, and a combined sketch runs all tests in one pass.

---

## Sketches Overview

| Sketch | Peripheral | Bus | Library |
|--------|-----------|-----|---------|
| `esp32s3_bridge_tester` | Bridge protocol | Serial | — |
| `ssd1306_display_test` | SSD1306 128×64 OLED | I2C | Adafruit SSD1306, Adafruit GFX |
| `sht21_sensor_test` | SHT21 Temp/Humidity | I2C | Wire (raw) |
| `spi_flash_test` | W25Q128JV NOR Flash | SPI | SPI (raw) |
| `uart_loopback_test` | UART Loopback | UART1 | HardwareSerial |
| `combined_peripheral_test` | All of the above | I2C+SPI+UART | All of the above |

---

## 1. `esp32s3_bridge_tester`

Path: `esp32s3_bridge_tester/esp32s3_bridge_tester.ino`

Periodically emits `[PERIPH][I2C/SPI/UART]` bridge request lines and
validates response lines. Prints stats counters (`sent`, `rsp ok`, `rsp err`).

Config assumptions match `peripherals.example.json`:
- I2C: `i2c0`, address `0x40` (SHT21)
- SPI: `spi2`, CS `0` (SPI flash)
- UART: `uart1`, unit `1` (loopback)

---

## 2. `ssd1306_display_test`

Path: `ssd1306_display_test/ssd1306_display_test.ino`
**Requires**: Adafruit SSD1306 (≥ 2.5.0), Adafruit GFX (≥ 1.11.0)

14 test phases exercising the full SSD1306 simulator:
1. Display init (full I2C command sequence)
2. Clear screen (horizontal addressing mode GDDRAM write)
3. Single-pixel pattern
4. Text rendering at sizes 1, 2, 3
5. Line drawing
6. Rectangles (outline + filled)
7. Circles (outline + filled)
8. Triangles (outline + filled)
9. XBM bitmap rendering (16×16 smiley)
10. Scroll right + left + stop (`0x26`/`0x27`/`0x2E`/`0x2F`)
11. Invert display (`0xA6`/`0xA7`)
12. Contrast sweep (`0x81`)
13. Dim display
14. Full-screen mixed-content dashboard

Wiring: SDA=GPIO8, SCL=GPIO9, I2C address `0x3C`

---

## 3. `sht21_sensor_test`

Path: `sht21_sensor_test/sht21_sensor_test.ino`
**Requires**: Wire (built-in)

15+ tests using raw I2C Wire calls:
- I2C ACK presence
- Soft reset (`0xFE`)
- User register default value + CRC-8
- Temperature hold-master (`0xE3`) with CRC
- Humidity hold-master (`0xE5`) with CRC
- All 4 resolution settings (RH12/T14, RH8/T12, RH10/T13, RH11/T11)
- Reserved bit [3:5] preservation
- Heater enable/disable
- Heater preserved after soft reset
- 5 consecutive readings consistency check
- Explicit CRC-8 validation (polynomial `0x131`)

Wiring: SDA=GPIO8, SCL=GPIO9, I2C address `0x40`

---

## 4. `spi_flash_test`

Path: `spi_flash_test/spi_flash_test.ino`
**Requires**: SPI (built-in)

15 tests covering the full W25Q128JV command set:
- JEDEC ID (`0x9F`) → `EF:40:18`
- Manufacturer/Device ID (`0x90`)
- Unique ID (`0x4B`)
- Write Enable/Disable (`0x06`/`0x04`) + WEL flag
- Status Registers SR1/SR2/SR3 read + SR2 QE bit write
- Sector Erase (`0x20`) + verify all `0xFF`
- Page Program (`0x02`) + readback
- 256-byte page boundary wrap
- AND-only write semantics
- Fast Read (`0x0B`)
- 32KB Block Erase (`0x52`)
- WIP busy polling
- Power Down / Release (`0xB9`/`0xAB`)
- Software Reset (`0x66`+`0x99`)
- Chip Erase (`0xC7`)

Wiring: MOSI=GPIO11, MISO=GPIO13, SCLK=GPIO12, CS=GPIO10 (SPI2/HSPI at 20 MHz)

---

## 5. `uart_loopback_test`

Path: `uart_loopback_test/uart_loopback_test.ino`
**Requires**: HardwareSerial (built-in)

12 tests covering the UART loopback simulator:
- Basic 5-byte loopback
- Single byte echo
- Sequential pattern (0–255)
- Large payload (512 bytes, FIFO overflow detection)
- Baud rate changes: 9600, 115200, 921600
- Parity modes: 8E1, 8O1
- 2 stop bits (8N2)
- Zero byte / break condition
- Rapid burst TX (10 × 16 bytes)
- ASCII string with CR/LF

Wiring: TX=GPIO17, RX=GPIO18 (UART1)

---

## 6. `combined_peripheral_test`

Path: `combined_peripheral_test/combined_peripheral_test.ino`
**Requires**: All libraries listed above

Runs all four peripheral test suites in a single sketch:
1. SSD1306 OLED (8 tests)
2. SHT21 sensor (7 tests)
3. SPI flash (6 tests)
4. UART loopback (3 tests)

Outputs a pass/fail summary on Serial and displays the final result on
the SSD1306 OLED.

---

## Building

### Prerequisites
- [arduino-cli](https://arduino.github.io/arduino-cli/latest/installation/)
- ESP32 board package (auto-installed by build script)

### Build all sketches

```bash
bash tests/arduino/build_all_sketches.sh
```

### Build a single sketch

```bash
# Bridge tester only (no external libraries)
bash tests/arduino/build_esp32s3_bridge_tester.sh

# Individual peripheral test
FQBN=esp32:esp32:esp32s3 arduino-cli compile \
  --fqbn esp32:esp32:esp32s3 \
  --libraries "Adafruit SSD1306,Adafruit GFX Library" \
  tests/arduino/ssd1306_display_test
```

### Default board target
`esp32:esp32:esp32s3`

Override: `FQBN=esp32:esp32:esp32s3 bash tests/arduino/build_all_sketches.sh`

---

## Running in the Simulator

1. Start the GUI simulator (`gui-esp32s3-simulator`).
2. Load peripheral config from `peripherals/peripherals.example.json`.
3. Start the QEMU instance.
4. Flash/upload the sketch `.bin` to the QEMU firmware image.
5. Watch the Serial tab and Peripheral panels for test output.

---

## CI

Arduino sketch compile workflow:
- `.github/workflows/arduino-bridge-sketch.yml`
