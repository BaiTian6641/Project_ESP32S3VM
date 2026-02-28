# ESP32-S3 QEMU Detailed Comparison

Last updated: 2026-02-27
Compared artifact: local tree `Project_ESP32S3VM/qemu`

## Method
This comparison was built from:
1. ESP32-S3 TRM chapter/module list (v1.2)
2. Espressif QEMU documentation support tables and notes
3. Local source inspection of machine wiring and available device models

Main implementation anchor:
- `qemu/hw/xtensa/esp32s3.c`

Supporting inventory:
- Device model files under `qemu/hw/**/esp32s3_*.c`
- Device type definitions in `qemu/include/hw/**/esp32s3*.h`

## Status Legend
- **Supported**: peripheral model exists and is wired/realized in ESP32-S3 machine
- **Stubbed**: explicitly mapped as unimplemented placeholder
- **Not found**: no dedicated ESP32-S3 model implementation found in this tree
- **Virtual helper**: non-real-hardware helper used for emulation convenience

## 2026-02-27 Audit Correction (Supersedes stale entries below)

This section is the authoritative status from direct code inspection of:
- `qemu/hw/xtensa/esp32s3.c` (machine wiring/realize)
- `qemu/hw/**/esp32s3_*.c` (device model presence and model comments)

### Corrected peripheral status snapshot

| Module | Audited status | Notes |
|---|---|---|
| I2C0/I2C1 | Supported | `hw/i2c/esp32s3_i2c.c` implements command engine + FIFO + IRQ |
| LEDC | Supported (MVP) | `hw/misc/esp32s3_ledc.c`; low-speed channel/timer register behavior |
| PCNT | Supported (MVP) | `hw/misc/esp32s3_pcnt.c`; register/IRQ semantics, no external pulse driving |
| RTC_IO | Supported (stub) | `hw/misc/esp32s3_rtc_io.c` register store model |
| I2S0/I2S1 | Supported (MVP) | `hw/misc/esp32s3_i2s.c`; TX immediate-completion behavior |
| SPI2/SPI3 (GP-SPI) | Supported (MVP) | `hw/misc/esp32s3_gpspi.c`; register model + immediate completion |
| MCPWM0/MCPWM1 | Supported (MVP) | `hw/misc/esp32s3_mcpwm.c`; register/IRQ compatibility model |
| LCD_CAM | Supported (stub/MVP) | `hw/misc/esp32s3_lcd_cam.c`; transfer completion interrupt stub |
| USB OTG | Supported (stub/MVP) | `hw/misc/esp32s3_usb_otg.c`; DWC2 ID/HWCFG register values modeled |
| BLE Controller | Supported (MVP) | `hw/misc/esp32s3_bt.c`; VHCI/HCI + ACL/L2CAP/ATT + virtual peer |
| Wi-Fi | Supported (partial fidelity) | `hw/net/esp32s3_wifi.c` + `esp32s3_wifi_backend.c`; TX bridge + host NIC + RX injection path |

### Remaining major gaps (confirmed)

- ULP coprocessor blocks: Stub only (8 KiB slow memory R/W, no code execution)
- WCL (World Controller): Not found
- Full ADC/SENS and Touch functional behavior: Stub (APB_SARADC + SENS register models with deterministic readings)
- Coexistence timing/fidelity (Wi-Fi + BLE): Implemented (time-division arbiter with 3 modes)

### Important nuance for Wi-Fi RX path

- `esp32s3_wifi.c` still contains an MVP `slc0_dma_rx_run()` path for empty RX completion during pure DMA start/poll flows.
- Actual host→guest frame injection is implemented through `esp32s3_wifi_backend.c` (`wifi_nic_receive()` + `inject_frame_to_slc_rx()`).
- Therefore, Wi-Fi data plane is implemented, but fidelity remains mixed between MVP and backend-driven paths.

---

## A) CPU and Core Extensions

| TRM Area | Local QEMU Status | Notes |
|---|---|---|
| Xtensa LX7 core + dual core | Supported | CPU type `esp32s3` instantiated for each core in machine init |
| PIE (Processor Instruction Extensions) / vector-like TIE ops | Supported | Implemented in `target/xtensa/translate_tie_esp32s3.c`; translator registered from `core-esp32s3.c` |
| Generic Xtensa vector ecosystem (HiFi/ConnX/Vectra families) | Disabled in config | Macros in `core-esp32s3/core-isa.h` show these options off |

---

## B) System / Control / Security / Memory

| Peripheral / Module | TRM Presence | Local QEMU Status | Evidence |
|---|---|---|---|
| System and Memory | Yes | Partial | Memory map + cache/MMU wiring in `hw/xtensa/esp32s3.c` and `hw/misc/esp32s3_cache.c` |
| eFuse | Yes | Supported | `hw/nvram/esp32s3_efuse.c`, wired in machine |
| Interrupt Matrix | Yes | Supported | `hw/xtensa/esp32s3_intc.c`, wired in machine |
| RTC_CNTL (LP mgmt) | Yes | Supported | `hw/misc/esp32s3_rtc_cntl.c`, wired in machine |
| Reset/Clock/System registers | Yes | Supported (expanded register-level model) | `hw/xtensa/esp32s3_clk.c`, `hw/misc/esp32s3_syscon.c`, `hw/misc/esp32s3_assist_debug.c`, `hw/misc/esp32s3_regi2c.c` plus machine wiring |
| PMS (Permission Control) | Yes | Supported | `hw/misc/esp32s3_pms.c` |
| WCL (World Controller) | Yes | Not found | Register base exists in headers, no device model found |
| AES | Yes | Supported | `hw/misc/esp32s3_aes.c` |
| SHA | Yes | Supported | `hw/misc/esp32s3_sha.c` |
| RSA | Yes | Supported | `hw/misc/esp32s3_rsa.c` |
| HMAC | Yes | Supported | `hw/misc/esp32s3_hmac.c` |
| DS (Digital Signature) | Yes | Supported | `hw/misc/esp32s3_ds.c` |
| XTS_AES | Yes | Supported | `hw/misc/esp32s3_xts_aes.c` |
| RNG | Yes | Supported | `hw/misc/esp32s3_rng.c` |

---

## C) Timers / DMA / Buses

| Peripheral / Module | TRM Presence | Local QEMU Status | Evidence |
|---|---|---|---|
| GDMA | Yes | Supported | `hw/dma/esp32s3_gdma.c`, wired with IRQs |
| SYSTIMER | Yes | Supported | `hw/timer/esp32s3_systimer.c` |
| TIMG (Timer Groups) | Yes | Supported | `hw/timer/esp32s3_timg.c` |
| WDT (inside TIMG path) | Yes | Partial | WDT IRQ lines connected from TIMG |
| RTC WDT / XTWDT details | Yes | Partial | esp32s3 README notes RTC watchdog not emulated yet |

---

## D) Communication / IO Peripherals

| Peripheral / Module | TRM Presence | Local QEMU Status | Evidence |
|---|---|---|---|
| UART | Yes | Supported | `hw/char/esp32s3_uart.c` |
| TWAI/CAN | Yes | Supported | `hw/net/can/esp32s3_twai.c` |
| SPI (flash-focused path) | Yes | Partial | `hw/ssi/esp32s3_spi.c` (SPI1 flash/psram path) |
| SD/MMC Host | Yes | Supported in this branch | `TYPE_DWC_SDMMC` initialized + `esp32s3_machine_init_sd(...)` |
| USB Serial/JTAG | Yes | Supported | JTAG/USB-serial block wired in machine |
| USB OTG | Yes | Not found | No ESP32-S3 USB OTG device model located |
| GPIO basic controller | Yes | Supported | `hw/gpio/esp32s3_gpio.c` |
| IO MUX + GPIO Matrix | Yes | **Both Supported** | IO MUX: `hw/gpio/esp32s3_iomux.c`; GPIO Matrix: `hw/gpio/esp32s3_gpio.c` (256 input + 49 output signal routing) |
| I2C | Yes | Not found | No ESP32-S3 I2C model file in local tree |
| I2S | Yes | Not found | No ESP32-S3 I2S model file in local tree |
| RMT | Yes | Partial | `hw/timer/esp32s3_rmt.c`, TX immediate-completion, register-level RX |
| LEDC | Yes | Not found | Register defs exist but no ESP32-S3 LEDC model file |
| MCPWM | Yes | Not found | No ESP32-S3 MCPWM model file |
| PCNT | Yes | Not found | Register defs exist but no ESP32-S3 PCNT model file |
| LCD_CAM | Yes | Not found | Register defs exist but no ESP32-S3 LCD_CAM model file |

---

## E) Analog / LP Co-processors

| Peripheral / Module | TRM Presence | Local QEMU Status | Evidence |
|---|---|---|---|
| ULP (FSM / RISC-V) | Yes | Stub | 8 KiB RTC slow memory R/W at `0x60021000`; no code execution |
| RTC I2C | Yes | Not found | Address constants exist, no modeled device found |
| On-chip sensors / analog signal processing | Yes | Stub | APB_SARADC at `0x60040000` + SENS at `0x60008800`; deterministic ADC/Touch/TSENS values |

---

## F) Alignment with Espressif QEMU README

### Consistent with README limitations
The local tree still shows missing/stubbed state for several peripherals listed as unsupported in docs, including:
- I2C, I2S, USB OTG, LEDC, ULP

### Divergence from README table
- SD/MMC: the generic table marks ESP32-S3 as unsupported, but this local tree has SDMMC integration paths.
- **IO MUX**: Functional register model (49 GPIO pads, PIN_CTRL, trace events).
- **GPIO Matrix**: Full signal routing model (256 input + 49 output signals, W1TS/W1TC, per-pin config).
- **RMT**: MVP model with immediate TX completion (4 TX + 4 RX channels, channel RAM).
- **Wi-Fi**: Full SLC DMA engine with descriptor ownership transitions, interrupt RAW/ST/ENA/CLR semantics, init-critical register defaults (SYSCON, PHY power-down), 7 sub-block register regions. **Sprint S4**: QEMU NIC integration (slirp/TAP), 802.11↔Ethernet frame bridge, virtual AP (beacon/auth/assoc), STA/AP/AP_STA mode support, MAC IRQ fully routed, WDEV register intercepts for MAC address config.
- **Wi-Fi MAC IRQ routing**: All 4 Wi-Fi IRQs (MAC, MAC_NMI, PWR, BB) now connected to the interrupt matrix. OpenCores Ethernet removed from default init to resolve source 0 conflict (Sprint S4).

Possible explanation:
- Local branch revision may include newer work than the referenced support matrix snapshot.

---

## G) Maintenance Checklist
Use this checklist to keep this comparison current when updating QEMU:

1. Re-scan `hw/xtensa/esp32s3.c` for new `object_initialize_child(...)` peripherals.
2. Re-scan `hw/**` for new `esp32s3_*.c` model files.
3. Check whether previously stubbed peripherals (`rmt`, `iomux`) are replaced by real models.
4. Re-check Espressif docs support matrix for updated statuses.
5. Update this file and `esp32s3_qemu_differences.md` together.

---

## H) File Pointers Used During This Comparison

- Machine assembly and realized blocks:
  - `qemu/hw/xtensa/esp32s3.c`
- Implemented ESP32-S3 models discovered:
  - `qemu/hw/char/esp32s3_uart.c`
  - `qemu/hw/gpio/esp32s3_gpio.c`
  - `qemu/hw/dma/esp32s3_gdma.c`
  - `qemu/hw/timer/esp32s3_timg.c`
  - `qemu/hw/timer/esp32s3_systimer.c`
  - `qemu/hw/misc/esp32s3_{aes,sha,rsa,hmac,ds,xts_aes,rng,rtc_cntl,pms,cache,syscon,assist_debug,regi2c}.c`
  - `qemu/hw/net/can/esp32s3_twai.c`
  - `qemu/hw/nvram/esp32s3_efuse.c`
  - `qemu/hw/ssi/esp32s3_spi.c`
- SIMD/PIE context:
  - `qemu/target/xtensa/core-esp32s3.c`
  - `qemu/target/xtensa/translate_tie_esp32s3.c`
  - `qemu/target/xtensa/core-esp32s3/core-isa.h`

---

## I) TRM Chapter-Level Coverage (Detailed)

This section maps the TRM chapter list (1..39) to the current local QEMU implementation status.

| TRM # | Chapter | QEMU Status | Notes / Evidence |
|---|---|---|---|
| 1 | Processor Instruction Extensions (PIE) | Supported | Xtensa ESP32-S3 TIE translator in `target/xtensa/translate_tie_esp32s3.c` |
| 2 | ULP Coprocessor (ULP-FSM, ULP-RISC-V) | Stub | 8 KiB RTC slow memory R/W (`hw/misc/esp32s3_ulp.c`); no code execution |
| 3 | GDMA Controller | Supported | `hw/dma/esp32s3_gdma.c`, IRQ wiring in machine |
| 4 | System and Memory | Partial | Core memory/cache/MMU paths present; not full TRM breadth |
| 5 | eFuse Controller | Supported | `hw/nvram/esp32s3_efuse.c` |
| 6 | IO MUX and GPIO Matrix | Supported | `hw/gpio/esp32s3_iomux.c` + `hw/gpio/esp32s3_gpio.c` |
| 7 | Reset and Clock | Supported (register-level) | `hw/xtensa/esp32s3_clk.c` + `hw/misc/esp32s3_syscon.c`; clock/reset banks and Wi-Fi/BT clk/rst regs handled at register level |
| 8 | Chip Boot Control | Partial | Strap handling in GPIO + boot-related machine behavior |
| 9 | Interrupt Matrix | Supported | `hw/xtensa/esp32s3_intc.c` |
| 10 | Low-power Management (RTC_CNTL) | Supported | `hw/misc/esp32s3_rtc_cntl.c` |
| 11 | System Timer (SYSTIMER) | Supported | `hw/timer/esp32s3_systimer.c` |
| 12 | Timer Group (TIMG) | Supported | `hw/timer/esp32s3_timg.c` |
| 13 | Watchdog Timers (WDT) | Partial | Covered via TIMG model paths |
| 14 | XTAL32K Watchdog Timers (XTWDT) | Partial | Not implemented as dedicated model |
| 15 | Permission Control (PMS) | Supported | `hw/misc/esp32s3_pms.c` |
| 16 | World Controller (WCL) | Not found | No model file found |
| 17 | System Registers (SYSTEM) | Supported (expanded register-level) | `hw/xtensa/esp32s3_clk.c` includes PERIP_CLK_EN0/1, PERIP_RST_EN0/1, CPU_PERI_*, MEM_PD_MASK, date and related control regs |
| 18 | AES Accelerator (AES) | Supported | `hw/misc/esp32s3_aes.c` |
| 19 | HMAC Accelerator (HMAC) | Supported | `hw/misc/esp32s3_hmac.c` |
| 20 | RSA Accelerator (RSA) | Supported | `hw/misc/esp32s3_rsa.c` |
| 21 | SHA Accelerator (SHA) | Supported | `hw/misc/esp32s3_sha.c` |
| 22 | Digital Signature (DS) | Supported | `hw/misc/esp32s3_ds.c` |
| 23 | External Memory Encryption and Decryption (XTS_AES) | Supported | `hw/misc/esp32s3_xts_aes.c` |
| 24 | Random Number Generator (RNG) | Supported | `hw/misc/esp32s3_rng.c` |
| 25 | Clock Glitch Detection | Not found | No dedicated ESP32-S3 model found |
| 26 | UART Controller (UART) | Supported | `hw/char/esp32s3_uart.c` |
| 27 | SPI Controller (SPI) | Partial | SPI1 flash/psram path implemented in `hw/ssi/esp32s3_spi.c` |
| 28 | I2C Controller (I2C) | Not found | No ESP32-S3 I2C model file found |
| 29 | I2S Controller (I2S) | Not found | No ESP32-S3 I2S model file found |
| 30 | Pulse Count Controller (PCNT) | Not found | No ESP32-S3 PCNT model file found |
| 31 | USB On-The-Go (USB) | Not found | No ESP32-S3 USB OTG model file found |
| 32 | USB Serial/JTAG Controller | Supported | Integrated via USB/JTAG device wiring in machine |
| 33 | Two-wire Automotive Interface (TWAI) | Supported | `hw/net/can/esp32s3_twai.c` |
| 34 | SD/MMC Host Controller (SDHOST) | Supported (branch-specific) | SDMMC integrated in machine and init flow |
| 35 | LED PWM Controller (LEDC) | Not found | No ESP32-S3 LEDC model file found |
| 36 | Motor Control PWM (MCPWM) | Not found | No ESP32-S3 MCPWM model file found |
| 37 | Remote Control Peripheral (RMT) | Partial (MVP) | `hw/timer/esp32s3_rmt.c` with MVP behavior |
| 38 | LCD and Camera Controller (LCD_CAM) | Not found | No ESP32-S3 LCD_CAM model file found |
| 39 | On-Chip Sensors and Analog Signal Processing | Stub | APB_SARADC (`hw/misc/esp32s3_apb_saradc.c`) + SENS (`hw/misc/esp32s3_sens.c`); deterministic readings |

### Summary of this chapter-level pass

- Fully/mostly covered chapters: 1, 3, 5, 6, 7, 9, 10, 11, 12, 15, 17, 18, 19, 20, 21, 22, 23, 24, 26, 32, 33, 34.
- Partial chapters: 4, 8, 13, 27, 37.
- Missing chapters: 2, 14, 16, 25, 28, 29, 30, 31, 35, 36, 38, 39.
- Outside the chapter list but relevant to project goals: Wi-Fi data plane implemented (STA/AP/AP_STA, S4); BLE controller register model + QEMU HCI transport + virtual peer implemented (S6); BLE GATT (ACL/L2CAP/ATT) + 5 peripheral stubs (USB OTG, I2S×2, SPI2/3, MCPWM×2, LCD_CAM) implemented (S7).

