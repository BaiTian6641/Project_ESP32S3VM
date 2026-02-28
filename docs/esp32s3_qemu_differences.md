# ESP32-S3 QEMU Differences Record

Last updated: 2026-02-28
Project: `Project_ESP32S3VM/qemu`

## Scope
This document records differences between:
1. ESP32-S3 Technical Reference Manual (TRM v1.2)
2. Espressif QEMU support documentation
3. Current implementation in this local QEMU tree

Primary external references:
- https://github.com/espressif/esp-toolchain-docs/raw/refs/heads/main/qemu/README.md
- https://github.com/espressif/esp-toolchain-docs/raw/refs/heads/main/qemu/esp32s3/README.md

Primary local implementation reference:
- `qemu/hw/xtensa/esp32s3.c`

---

## Quick Summary

### Implemented (major blocks)
The local tree instantiates and realizes the following ESP32-S3 peripherals:
- UART
- Interrupt Matrix
- RTC CNTL
- SPI1 (flash path)
- Cache/MMU and external memory mapping
- eFuse
- RNG
- GDMA
- AES, SHA, RSA, HMAC, Digital Signature, XTS_AES
- Timer Groups (TIMG0/TIMG1)
- System Timer (SYSTIMER)
- TWAI/CAN
- USB Serial/JTAG
- **IO MUX** (functional register model for all 49 GPIO pads)
- **GPIO Matrix** (full signal routing: 256 input + 49 output signals, W1TS/W1TC ops)
- **RMT** (MVP model with TX immediate-completion and register-level RX stub)
- **Wi-Fi subsystem** (SLC DMA with descriptor ownership, interrupt model, init-critical regs)
- **SYSCON** (full register model at `0x60026000`)
- **SYSTEM clock/reset bank** (expanded register-level model at `0x600C0000`)
- **ASSIST_DEBUG** (dual-core register model at `0x600CE000`)
- **REGI2C** (analog calibration stub at `0x6000E000`)
- **I2C** (×2 instances at `0x60013000` / `0x60027000`, command-based master controller)
- **LEDC** (8 channels, 4 timers at `0x60019000`, duty shadow + interrupt model)
- **PCNT** (4-unit pulse counter at `0x60017000`, counter reset + interrupt model)
- **RTC_IO** (22-pad IO MUX stub at `0x60008400`, W1TS/W1TC atomic ops)
- **BLE Controller** (register model + QEMU HCI transport + virtual peer + GATT at `0x60011000`)
- **I2S** (×2 instances at `0x6000F000` / `0x6002D000`, R/W store + TX immediate completion)
- **GP-SPI** (SPI2 at `0x60024000`, SPI3 at `0x60025000`, R/W store + immediate transaction)
- **MCPWM** (×2 instances at `0x6001E000` / `0x6002C000`, R/W store + interrupt model)
- **LCD_CAM** (R/W store at `0x60041000`, LCD immediate completion)
- **USB OTG** (DWC2 register stub at `0x60080000`, correct GHWCFG values)
- **APB SAR ADC** (register-level stub at `0x60040000`, INT model, mid-scale readings)
- **SENS** (analog sensor stub at `0x60008800`, SAR/Touch/TSENS/COCPU interrupt model)
- **ULP coprocessor** (8 KiB RTC slow memory at `0x60021000`, R/W stub — no code execution)
- **Coexistence arbiter** (virtual QEMU peripheral — time-division Wi-Fi/BLE scheduling)
- OpenCores Ethernet (virtual helper peripheral)
- RGB framebuffer (virtual helper peripheral)

Evidence:
- Instantiation list: `qemu/hw/xtensa/esp32s3.c` (around lines 638-659)
- USB Serial/JTAG: `qemu/hw/xtensa/esp32s3.c` (around lines 671-675)
- TIMG/SYSTIMER/GDMA/crypto realizations: `qemu/hw/xtensa/esp32s3.c` (around lines 760-870)

### Missing (Not Implemented)
- No ESP32-S3 device models found in this tree for:
  - World Controller (WCL)
  - On-chip analog/sensor processing block (functional ADC conversion — stub only)

Status terminology used in this file:
- **Supported**: model exists and is wired in machine init
- **Partial**: model exists but only subset/MVP behavior is implemented
- **Not Implemented**: no dedicated model exists in this tree

### Recently added (Sprint S1)
- **IO MUX**: Full register model replacing unimplemented stub
  - Files: `qemu/hw/gpio/esp32s3_iomux.c`, `qemu/include/hw/gpio/esp32s3_iomux.h`
  - 49 GPIO pad config registers + PIN_CTRL + DATE
  - Trace events for read/write/reset
- **RMT**: MVP replacing unimplemented stub
  - Files: `qemu/hw/timer/esp32s3_rmt.c`, `qemu/include/hw/timer/esp32s3_rmt.h`
  - 4 TX + 4 RX channels, full register set, channel RAM at 0x0400
  - TX immediate-completion for driver compatibility
  - IRQ wired to ETS_RMT_INTR_SOURCE
  - Trace events for TX start/end, IRQ updates
- **Wi-Fi skeleton**: New device with 7 sub-block register regions
  - Files: `qemu/hw/net/esp32s3_wifi.c`, `qemu/include/hw/net/esp32s3_wifi.h`
  - BB, NRX, FE, FE2, SLC, SLCHOST, WDEV mapped to correct addresses
  - All 4 Wi-Fi IRQs wired to interrupt matrix (MAC, MAC_NMI, PWR, BB)
  - OpenCores Ethernet removed from default init (source 0 conflict resolved in S4)

### Recently added (Sprint S2)
- **GPIO Matrix**: Full rewrite with signal routing
  - Files: `qemu/hw/gpio/esp32s3_gpio.c`, `qemu/include/hw/gpio/esp32s3_gpio.h`
  - Parent class virtual dispatch added to `esp32_gpio.h`/`esp32_gpio.c`
  - 49 GPIO pins with OUT/OUT1, ENABLE/ENABLE1, IN/IN1, STATUS/STATUS1
  - Atomic W1TS/W1TC operations for OUT, ENABLE, STATUS registers
  - 256 input signal routing entries (FUNC_IN_SEL_CFG)
  - 49 output signal routing entries (FUNC_OUT_SEL_CFG)
  - Per-pin configuration with interrupt type support
  - IRQ wired to ETS_GPIO_INTR_SOURCE=16
  - Trace events: esp32s3_gpio_read/write/irq_update/reset
- **Wi-Fi init-critical registers**: SYSCON + PHY power-down defaults
  - SYSCON register model now provides `SYSCON_WIFI_CLK_EN_REG` (default `0xFFFCE030`) and `SYSCON_WIFI_RST_EN_REG`
  - BB/NRX/FE/FE2 power-down control registers set to power-up defaults
  - Satisfies ESP-IDF PHY init assertions
- **Wi-Fi SLC interrupt model**: Full RAW/ST/ENA/CLR semantics
  - SLC sub-block uses dedicated ops (not generic store)
  - SLC0 interrupt drives Wi-Fi MAC IRQ output in device model
  - All 4 Wi-Fi IRQs connected to interrupt matrix (S4)
  - SLC1 interrupt state tracked (BT, not yet wired)
- **Wi-Fi SLC DMA engine**: Descriptor ownership transitions
  - lldesc_t descriptor format: 12-byte linked-list descriptors
  - TX engine: walks chain, flips owner HW→SW, fires TX_DONE/TX_SUC_EOF/TX_DSCR_EMPTY
  - RX engine: walks chain, flips owner HW→SW, fires RX_DONE/RX_EOF
  - SLC link register handling (START/STOP/RESTART/PARK)
  - SLC_CONF0 reset bits clear DMA engine state
  - Trace events: slc_dma_tx/rx, slc_dma_start/stop, slc_irq

### Recently added (Sprint S3)
- **SYSCON register model** (replaces previous RAM hack)
  - Files: `qemu/hw/misc/esp32s3_syscon.c`, `qemu/include/hw/misc/esp32s3_syscon.h`
  - Full `0x400`-byte register space with defaults and R/W behavior
  - Includes `WIFI_CLK_EN`, `WIFI_RST_EN`, `CLK_CONF`, `FRONT_END_MEM_PD`, ACE regions, `DATE`
- **SYSTEM register expansion**
  - File: `qemu/hw/xtensa/esp32s3_clk.c`
  - Adds `PERIP_CLK_EN0/1`, `PERIP_RST_EN0/1`, `CPU_PERI_CLK_EN/RST_EN`, `MEM_PD_MASK`, and related control registers
- **ASSIST_DEBUG model**
  - Files: `qemu/hw/misc/esp32s3_assist_debug.c`, `qemu/include/hw/misc/esp32s3_assist_debug.h`
  - Dual-core register blocks, SP_MIN/SP_MAX stack guard regs, interrupt regs
- **REGI2C analog stub**
  - Files: `qemu/hw/misc/esp32s3_regi2c.c`, `qemu/include/hw/misc/esp32s3_regi2c.h`
  - Absorbs PHY calibration accesses and reports `BBPLL_CAL_DONE`
- **Wi-Fi SLC interrupt completeness improvements**
  - Added full SLC0 interrupt bit definitions and TX/RX START interrupt signaling on DMA link start

### Recently added (Sprint S4)
- **Wi-Fi data-plane backend** (STA/AP/AP_STA)
  - Files: `qemu/hw/net/esp32s3_wifi_backend.c`, `qemu/include/hw/net/esp32s3_wifi_backend.h`
  - QEMU NIC integration via `qemu_new_nic()` — supports slirp/TAP/socket backends
  - Bidirectional 802.11 ↔ Ethernet frame conversion
    - TX: SLC DMA → parse 802.11 data frame → extract DA/SA/payload → Ethernet → QEMU NIC
    - RX: QEMU NIC → Ethernet → construct 802.11 header + RX metadata → inject SLC RX DMA
  - Three operating modes: STA (station), AP (access point), AP_STA (concurrent)
  - Mode-aware frame routing (ToDS/FromDS flag selection based on interface)
  - Fallback raw Ethernet pass-through for non-standard blob frame formats
- **Virtual AP for STA mode**
  - Beacon timer (100 TU ≈ 102.4 ms) driven by QEMU virtual clock
  - Probe response with full IEs (SSID, supported rates, DS params, extended rates)
  - Authentication response (Open System, sequence 2, status success)
  - Association response with AID, capability info, rate IEs
  - Association state machine: IDLE → AUTH → ASSOC
  - Deauthentication/disassociation handling
  - Fixed BSSID `02:00:00:00:00:01`, configurable SSID (default "QEMU_WIFI")
  - Deterministic RSSI values (-30 dBm data, -40 dBm beacons)
- **WDEV register intercepts**
  - MAC address LO/HI registers intercepted for backend MAC configuration
  - Other registers remain generic R/W store
- **MAC IRQ routing completed**
  - All 4 Wi-Fi IRQs now connected: MAC→source 0, MAC_NMI→source 1, PWR, BB
  - OpenCores Ethernet removed from default machine init (resolves source 0 conflict)
  - OpenCores function preserved but commented out for optional re-enablement
- **SLC DMA TX frame extraction**
  - TX DMA engine now reads frame buffer contents and routes to backend
  - Frame length extracted from descriptor word0 length field
  - Backend parses 802.11 frame type: management handled locally, data bridged to NIC
- **NIC device properties**
  - Wi-Fi device exposes `DEFINE_NIC_PROPERTIES` for command-line NIC configuration
  - Usage: `qemu-system-xtensa -machine esp32s3 -nic user,model=esp32s3.wifi`

Evidence:
- IO MUX model: `qemu/hw/gpio/esp32s3_iomux.c` mapped at `DR_REG_IO_MUX_BASE`
- RMT model: `qemu/hw/timer/esp32s3_rmt.c` mapped at `DR_REG_RMT_BASE`
- Wi-Fi device: `qemu/hw/net/esp32s3_wifi.c` with sub-blocks at respective addresses
- Wi-Fi backend: `qemu/hw/net/esp32s3_wifi_backend.c` with QEMU NIC and frame bridge
- System infrastructure: `qemu/hw/misc/esp32s3_{syscon,assist_debug,regi2c}.c` and `qemu/hw/xtensa/esp32s3_clk.c`
- I2C controllers: `qemu/hw/i2c/esp32s3_i2c.c` wired at I2C0=`0x60013000`, I2C1=`0x60027000`
- LEDC controller: `qemu/hw/misc/esp32s3_ledc.c` wired at `0x60019000`
- PCNT controller: `qemu/hw/misc/esp32s3_pcnt.c` wired at `0x60017000`
- RTC IO stub: `qemu/hw/misc/esp32s3_rtc_io.c` wired at `0x60008400`
- BLE controller: `qemu/hw/misc/esp32s3_bt.c` wired at `0x60011000` (R/W store + QEMU HCI transport + virtual peer + GATT, 7 IRQs)
- I2S controllers: `qemu/hw/misc/esp32s3_i2s.c` wired at I2S0=`0x6000F000`, I2S1=`0x6002D000`
- GP-SPI controllers: `qemu/hw/misc/esp32s3_gpspi.c` wired at SPI2=`0x60024000`, SPI3=`0x60025000`
- MCPWM controllers: `qemu/hw/misc/esp32s3_mcpwm.c` wired at MCPWM0=`0x6001E000`, MCPWM1=`0x6002C000`
- LCD_CAM stub: `qemu/hw/misc/esp32s3_lcd_cam.c` wired at `0x60041000`
- USB OTG stub: `qemu/hw/misc/esp32s3_usb_otg.c` wired at `0x60080000`
- APB SARADC stub: `qemu/hw/misc/esp32s3_apb_saradc.c` wired at `0x60040000` (IRQ→ETS_APB_ADC_INTR_SOURCE=65)
- SENS stub: `qemu/hw/misc/esp32s3_sens.c` wired at `0x60008800`
- ULP slow memory: `qemu/hw/misc/esp32s3_ulp.c` wired at `0x60021000` (DR_REG_RTC_SLOWMEM_BASE)
- Coexistence: `qemu/hw/misc/esp32s3_coex.c` (virtual — realized but not memory-mapped)
- Deterministic RNG: `qemu/hw/misc/esp32s3_rng.c` (xorshift64 PRNG mode added)
- Missing model files under `qemu/hw/**` for the peripherals above.

### Recently added (Sprint S8)
- **APB SAR ADC controller**
  - Files: `qemu/hw/misc/esp32s3_apb_saradc.c`, `qemu/include/hw/misc/esp32s3_apb_saradc.h`
  - 28 register offsets (CTRL, CTRL2, FILTER, FSM_WAIT, PAT_TAB, ARB, DMA, CLK, DATE)
  - INT_RAW/ST/ENA/CLR interrupt semantics, IRQ wired to ETS_APB_ADC_INTR_SOURCE(65)
  - SAR_START self-clearing with immediate SAR1_DONE + SAR2_DONE
  - Deterministic mid-scale 12-bit ADC readings (0x800)
- **SENS (analog sensor) controller**
  - Files: `qemu/hw/misc/esp32s3_sens.c`, `qemu/include/hw/misc/esp32s3_sens.h`
  - SAR measurement control with MEAS_DONE auto-set on read
  - Temperature sensor readout (~25°C, code 128) with READY bit
  - Touch sensor: all 15 pads report done, no-touch baseline
  - COCPU interrupt model (INT_RAW/ENA/ST/CLR) with W1TS/W1TC for ENA
- **ULP coprocessor stub**
  - Files: `qemu/hw/misc/esp32s3_ulp.c`, `qemu/include/hw/misc/esp32s3_ulp.h`
  - 8 KiB RTC slow memory R/W (byte-level access)
  - ULP does not execute code — shared memory only
  - Control registers in RTC_CNTL, interrupt/state in SENS block
- **Coexistence arbitration model**
  - Files: `qemu/hw/misc/esp32s3_coex.c`, `qemu/include/hw/misc/esp32s3_coex.h`
  - Time-division scheduling: BALANCED (50/50), WIFI_THROUGHPUT (75/25), BLE_LATENCY (25/75)
  - QEMU timer-based slot switching (default 10ms slots)
  - MMIO register interface: STATUS/MODE/SLOT_US/WIFI_GRANT/BLE_GRANT/CTRL/MAGIC
  - MVP: concurrent operation without blocking; grant counters for assertions
- **Deterministic RNG mode**
  - Modified: `qemu/hw/misc/esp32s3_rng.c`, `qemu/include/hw/misc/esp32s3_rng.h`
  - xorshift64 PRNG enabled via QEMU global properties
  - Usage: `-global misc.esp32s3.rng.deterministic=true -global misc.esp32s3.rng.seed=12345`

---

## Notable Documentation Mismatch
The generic support matrix in Espressif docs marks ESP32-S3 SD/MMC as unsupported, but this local tree has SD/MMC integration points:
- SDMMC child is initialized in `qemu/hw/xtensa/esp32s3.c`.
- SD card attachment flow exists in `esp32s3_machine_init_sd(...)` in the same file.

Interpretation:
- This local branch appears ahead of the referenced matrix for SD/MMC support.

---

## SIMD / PIE Context (already checked)
For completeness: ESP32-S3-specific PIE/TIE vector-like instructions are present in this tree (Xtensa TIE translator for ESP32-S3), even though generic Xtensa vector ecosystems are disabled in core feature macros.

Primary files:
- `qemu/target/xtensa/core-esp32s3.c`
- `qemu/target/xtensa/translate_tie_esp32s3.c`
- `qemu/target/xtensa/core-esp32s3/core-isa.h`
