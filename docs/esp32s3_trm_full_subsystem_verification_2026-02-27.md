# ESP32-S3 QEMU Full Subsystem Verification Against TRM

Last verified: 2026-02-28  
Workspace: Project_ESP32S3VM  
Primary sources: `qemu/hw/**/esp32s3*.c`, `qemu/hw/xtensa/esp32s3.c`, `docs/Esp32-s3_technical_reference_manual_en.md`, ESP-IDF register headers

## Scope and method

This verification checks **all ESP32-S3 TRM subsystems** against the current QEMU implementation by:

1. Confirming device instantiation/mapping in `qemu/hw/xtensa/esp32s3.c`
2. Auditing each `esp32s3*.c` peripheral model for register/interrupt behavior level
3. Marking each TRM subsystem as:
   - **Implemented**: dedicated model exists and is wired
   - **Implemented (MVP/Stub)**: model exists but behavior is compatibility-level
   - **Missing**: no dedicated model in tree

---

## Executive summary

- **38 ESP32-S3 model files** are present under `qemu/hw/**`
- Most digital subsystems are implemented and wired in machine init
- Major high-fidelity areas: core boot path, interrupt matrix, RTC_CNTL, SYSTEM/SYSCON, GDMA, SPI1 flash path, GPIO matrix, I2C, crypto family, Wi-Fi/BLE control/data planes
- Main fidelity gaps: USB OTG (register stub), RTC_IO (stub), RMT RX realism, MCPWM behavior depth, analog functional behavior depth
- Missing subsystem models from TRM coverage: WCL, clock-glitch detector

---

## TRM chapter-level verification (all subsystems)

| TRM | Subsystem | Verification result | Evidence |
|---|---|---|---|
| 1 | PIE / Xtensa extensions | Implemented | `target/xtensa/core-esp32s3.c`, `translate_tie_esp32s3.c` |
| 2 | ULP coprocessor | Implemented (stub) | `hw/misc/esp32s3_ulp.c` |
| 3 | GDMA | Implemented | `hw/dma/esp32s3_gdma.c`, wired in machine |
| 4 | System and memory | Implemented (partial fidelity) | `esp32s3_cache.c`, machine memory map |
| 5 | eFuse | Implemented | `hw/nvram/esp32s3_efuse.c` |
| 6 | IO MUX + GPIO matrix | Implemented | `esp32s3_iomux.c`, `esp32s3_gpio.c` |
| 7 | Reset and clock | Implemented | `esp32s3_clk.c`, `esp32s3_syscon.c`, `esp32s3_rtc_cntl.c` |
| 8 | Boot control | Implemented (partial) | strap/reset paths in GPIO + RTC_CNTL + machine boot flow |
| 9 | Interrupt matrix | Implemented | `hw/xtensa/esp32s3_intc.c` |
| 10 | RTC_CNTL low-power control | Implemented | `hw/misc/esp32s3_rtc_cntl.c` |
| 11 | System timer | Implemented | `hw/timer/esp32s3_systimer.c` |
| 12 | Timer group | Implemented | `hw/timer/esp32s3_timg.c` |
| 13 | Watchdog timers | Implemented (partial) | via TIMG/RTC paths, not full analog side-effects |
| 14 | XTAL32K watchdog | Missing/partial | no dedicated XTWDT model |
| 15 | PMS | Implemented (stub-level) | `hw/misc/esp32s3_pms.c` generic register store |
| 16 | WCL | Missing | no `esp32s3*wcl*.c` model |
| 17 | SYSTEM registers | Implemented | `hw/xtensa/esp32s3_clk.c` |
| 18 | AES | Implemented | `hw/misc/esp32s3_aes.c` (inherits `TYPE_ESP_AES`) |
| 19 | HMAC | Implemented | `hw/misc/esp32s3_hmac.c` |
| 20 | RSA | Implemented | `hw/misc/esp32s3_rsa.c` |
| 21 | SHA | Implemented | `hw/misc/esp32s3_sha.c` |
| 22 | DS | Implemented | `hw/misc/esp32s3_ds.c` |
| 23 | XTS_AES | Implemented | `hw/misc/esp32s3_xts_aes.c` |
| 24 | RNG | Implemented | `hw/misc/esp32s3_rng.c` |
| 25 | Clock glitch detection | Missing | no dedicated ESP32-S3 model |
| 26 | UART | Implemented | `hw/char/esp32s3_uart.c` |
| 27 | SPI | Implemented (split) | SPI1: `hw/ssi/esp32s3_spi.c`; GP-SPI2/3: `hw/misc/esp32s3_gpspi.c` |
| 28 | I2C | Implemented | `hw/i2c/esp32s3_i2c.c` |
| 29 | I2S | Implemented (MVP) | `hw/misc/esp32s3_i2s.c` |
| 30 | PCNT | Implemented | `hw/misc/esp32s3_pcnt.c` |
| 31 | USB OTG | Implemented (register stub) | `hw/misc/esp32s3_usb_otg.c` |
| 32 | USB Serial/JTAG | Implemented | JTAG/USB serial device wiring in machine |
| 33 | TWAI | Implemented | `hw/net/can/esp32s3_twai.c` |
| 34 | SD/MMC host | Implemented | SDMMC device wiring and init in machine |
| 35 | LEDC | Implemented | `hw/misc/esp32s3_ledc.c` |
| 36 | MCPWM | Implemented (MVP) | `hw/misc/esp32s3_mcpwm.c` |
| 37 | RMT | Implemented (MVP) | `hw/timer/esp32s3_rmt.c` |
| 38 | LCD_CAM | Implemented (MVP+) | `hw/misc/esp32s3_lcd_cam.c` |
| 39 | On-chip sensors / analog signal processing | Implemented (stub) | `hw/misc/esp32s3_apb_saradc.c`, `hw/misc/esp32s3_sens.c` |

---

## Verification of critical correctness fixes (already applied)

1. **I2S UPDATE bit positions fixed**  
   - `I2S_TX_UPDATE = BIT(8)` and `I2S_RX_UPDATE = BIT(8)` in `qemu/include/hw/misc/esp32s3_i2s.h`
2. **MCPWM interrupt offsets fixed**  
   - `UPDATE_CFG=0x10C`, `INT_ENA=0x110`, `INT_RAW=0x114`, `INT_ST=0x118`, `INT_CLR=0x11C` in `qemu/include/hw/misc/esp32s3_mcpwm.h`
3. Build status after fixes: `qemu-system-xtensa` links successfully (`ninja` pass)

---

## LL/HAL/GDMA readiness matrix (focused operational view)

Legend:
- **Ready**: common ESP-IDF LL/HAL register flow works, interrupt semantics are in place, and no known hard blocker for basic operation
- **Partial**: boots/configures and basic ops work, but behavior depth is still MVP/stub
- **Limited**: register-level compatibility only; functional data-path/device behavior is not complete

| Peripheral | LL/HAL readiness | GDMA readiness | Notes |
|---|---|---|---|
| UART0/1/2 | Ready | N/A | Stable register + IRQ path |
| GPIO + IO_MUX | Ready | N/A | Common IDF GPIO stack works |
| I2C0/1 | Ready | N/A | Register + command/IRQ path implemented |
| I2S0/1 | Partial | Partial | TX/RX start now kicks GDMA channel if assigned; still MVP data-path fidelity |
| SPI2/3 (GP-SPI) | Partial | Partial | USR transaction now probes GDMA IN/OUT channels and raises DMA done interrupts when active |
| SPI1/flash path | Ready | N/A | Flash/MMU boot path stable |
| GDMA core | Ready | Ready | Channel/peripheral selection + descriptor traversal in place |
| LCD_CAM | Partial | Ready | TX path drains GDMA descriptors and signals completion |
| AES/SHA | Ready | Ready | Peripheral engines integrated with GDMA channels |
| MCPWM0/1 | Partial | N/A | Corrected INT/UPDATE mapping; functional PWM behavior still limited |
| LEDC | Partial | N/A | Register compatibility with basic control paths |
| PCNT | Partial | N/A | Register/IRQ path, limited signal realism |
| RMT | Partial | N/A | TX path MVP; RX capture remains limited |
| USB OTG | Limited | Limited | DWC2 register stub, no full endpoint transfer engine |
| RTC_IO | Partial | N/A | Register-faithful stub, analog/deep-sleep behavior limited |
| APB_SARADC + SENS | Partial | Limited | Stubs for compatibility; analog conversion/touch behavior not fully modeled |
| TWAI | Ready | N/A | Controller model present and wired |
| Wi-Fi/BT/Coex | Partial | Partial | Functional emulation available, not cycle/PHY accurate |

## Subsystems that are present but still fidelity-limited

- `esp32s3_usb_otg.c`: DWC2 identity/config register stub; no full endpoint/transfer engine
- `esp32s3_rtc_io.c`: register-faithful stub only; no deep-sleep/analog behavior
- `esp32s3_rmt.c`: TX immediate completion MVP; RX capture timing not fully modeled
- `esp32s3_i2s.c`: register/IRQ model + GDMA kick path present; streaming fidelity remains MVP
- `esp32s3_gpspi.c`: immediate transaction completion model for SPI2/SPI3 with GDMA kick hooks
- `esp32s3_pms.c`: generic register-store compatibility model
- `esp32s3_apb_saradc.c`, `esp32s3_sens.c`, `esp32s3_ulp.c`: compatibility-level stubs

---

## Missing TRM subsystems in current tree

- WCL (World Controller)
- Clock glitch detector model

---

## Conclusion

The current QEMU codebase now provides **broad ESP32-S3 digital subsystem coverage** and boots/runs significant ESP-IDF workflows, but it is not yet a full transistor-faithful TRM implementation. Against TRM “all subsystem” verification, status is:

- **Implemented or implemented-at-MVP:** the majority of digital chapters
- **Missing:** ULP, WCL, ADC/touch/analog-focused chapters
- **Next priority for full TRM parity:** analog/ULP/WCL subsystem models and deeper behavior fidelity for USB OTG/RMT/I2S/GP-SPI/RTC_IO
