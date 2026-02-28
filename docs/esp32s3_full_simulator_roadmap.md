# ESP32-S3 Full Simulator Roadmap (Including Wi-Fi and Bluetooth)

Last updated: 2026-02-27  
Project root: `Project_ESP32S3VM`  
Target tree: `qemu/`  
Reference: `esp-idf/` (ESP-IDF v5.x tree for HAL/SOC/driver reference)

## 1) Goal and Definition of "Fully Functional"

For this project, **fully functional ESP32-S3 simulator** means:

1. Boots ESP-IDF images reliably (bootloader + app + partitions).
2. Supports major digital peripherals used by production firmware.
3. Preserves enough register/interrupt behavior for unmodified drivers.
4. Supports meaningful networking workflows for:
   - Wi-Fi station mode (minimum)
   - BLE host/controller workflows (minimum)
5. Is deterministic enough for CI and regression testing.

### Practical completion criteria
- `idf.py build` artifacts run in QEMU without target-specific hacks.
- Representative ESP-IDF examples pass smoke tests.
- Networking examples (TCP/UDP over Wi-Fi path and BLE GATT sample path) execute end-to-end in emulation.

---

## 2) Current Baseline (S7 code integrated)

### 2.1 Implemented Peripherals (38 model files)

| Category | Peripherals | Evidence |
|----------|------------|----------|
| CPU/Core | Xtensa LX7 dual-core, PIE TIE translator | `target/xtensa/core-esp32s3.c`, `translate_tie_esp32s3.c` |
| System | Interrupt Matrix, RTC_CNTL, Clock (minimal), PMS, Cache/MMU | `hw/xtensa/esp32s3_intc.c`, `esp32s3_clk.c`, `hw/misc/esp32s3_{rtc_cntl,pms,cache}.c` |
| Crypto | AES, SHA, RSA, HMAC, DS, XTS_AES, RNG | `hw/misc/esp32s3_{aes,sha,rsa,hmac,ds,xts_aes,rng}.c` |
| Timers | TIMG×2, SYSTIMER, RMT (MVP) | `hw/timer/esp32s3_{timg,systimer,rmt}.c` |
| GPIO/IO | GPIO Matrix (256-input + 49-output routing), IO MUX | `hw/gpio/esp32s3_{gpio,iomux}.c` |
| Comms | UART×3, TWAI, SPI (flash path), USB Serial/JTAG, SDMMC | `hw/char/esp32s3_uart.c`, `hw/net/can/esp32s3_twai.c`, `hw/ssi/esp32s3_spi.c` |
| DMA | GDMA (5-channel AHB) | `hw/dma/esp32s3_gdma.c` |
| Storage | eFuse, SPI Flash/PSRAM via cache | `hw/nvram/esp32s3_efuse.c` |
| Wi-Fi | SLC DMA engine, 7 sub-block MMIO, interrupt model | `hw/net/esp32s3_wifi.c` |
| Virtual | OpenCores Ethernet, RGB framebuffer | Non-TRM virtual helpers |

### 2.2 System Infrastructure State

**SYSCON** (`0x60026000`) — ✅ Full register model (`esp32s3_syscon.c`, Sprint S3):
- Proper QOM SysBusDevice replacing prior RAM hack
- All registers: `WIFI_CLK_EN` (default `0xFFFCE030`), `WIFI_RST_EN`, `CLK_CONF`, `CLK_OUT_EN`, `WIFI_BB_CFG`, `FRONT_END_MEM_PD`, ACE regions, `DATE` (with ECO3 MSB for chip revision detection)
- QEMU origin signature at `0x3F8`

**SYSTEM** (`0x600C0000`) — ✅ Expanded model in `esp32s3_clk.c` (Sprint S3):
- Handles 25+ registers: `CORE_1_CONTROL_0/1`, `CPU_PER_CONF`, `SYSCLK_CONF`, `CPU_PERI_CLK_EN/RST_EN`, `MEM_PD_MASK`, `PERIP_CLK_EN0/1`, `PERIP_RST_EN0/1`, `BT_LPCK_DIV_INT/FRAC`, `RSA_PD_CTRL`, `EDMA_CTRL`, `CACHE_CONTROL`, `RTC_FASTMEM_CONFIG/CRC`, `REDUNDANT_ECO_CTRL`, `CLOCK_GATE`, `SYSTEM_DATE`
- Correct boot defaults: `PERIP_CLK_EN0=0xF9C1A7C0`, `PERIP_CLK_EN1=0x184`

**ASSIST_DEBUG** (`0x600CE000`) — ✅ Full register model (`esp32s3_assist_debug.c`, Sprint S3):
- Dual-core support (Core 0 at `0x000`, Core 1 at `0x090`)
- SP_MIN/SP_MAX stack guard, interrupt enable/raw/release/clear, recording registers
- IRQ wired to `ETS_ASSIST_DEBUG_INTR_SOURCE`

**REGI2C** (`0x6000E000`) — ✅ Analog bus stub (`esp32s3_regi2c.c`, Sprint S3):
- Absorbs PHY calibration writes for BBPLL, DIG_REG, BIAS, SAR
- `BBPLL_CAL_DONE` bit always set on read to prevent infinite polling

**Catch-all I/O region**: `0x60000000`-`0x600D1000` (`esp32s3.iomem`) silently absorbs reads (returns 0) and drops writes for all unmapped addresses.

### 2.3 Known Gaps

- **Still missing major blocks**: ULP coprocessor, WCL, ADC/SENS functional model, Touch functional model
- **MVP-level peripherals still need fidelity work**: I2S, MCPWM, LCD_CAM, USB OTG, RTC_IO, parts of Wi-Fi/BLE behavior are implemented as register-focused compatibility models
- **System-model caveats**: register-level behavior is implemented for SYSCON/SYSTEM/ASSIST_DEBUG/REGI2C; strict hardware side-effects (true clock gating, stack monitor traps, full analog behavior) remain partial
- **Coexistence and CI determinism**: Coexistence scheduler and deterministic RNG mode are integrated; CI soak automation is in progress

---

## 3) Architecture Strategy

### Layer A — Peripheral-faithful control path
Emulate register blocks, interrupts, clocks, reset behavior, DMA descriptors, and mailbox interfaces expected by ESP-IDF. Goal: let stock driver stacks initialize and run.

### Layer B — Host-backed packet/data path
Bridge emulated MAC/controller data to host networking/BLE stacks. Keep interfaces deterministic and testable.

### Layer C — Optional enhanced fidelity
Add stricter timing windows, coexistence arbitration details, and additional error injection modes.

### ESP-IDF HAL/LL Pattern
All ESP-IDF peripheral drivers follow:
1. **HAL context** holds a pointer to hardware register struct (direct-mapped peripheral address)
2. **LL functions** do direct `REG_SET_FIELD()` / `REG_GET_FIELD()` — what QEMU models must satisfy
3. Reads return immediately with valid values; writes take effect immediately
4. DMA/interrupt handling above HAL layer — GDMA channels service data movement
5. Reference register structs in `esp-idf/components/soc/esp32s3/register/soc/`

---

## 4) ESP-IDF Boot Path Register Dependencies

### Pre-OS Phase (from `esp_system/port/cpu_start.c`)
1. CPU init → vector table
2. Cache init → `EXTMEM` registers — **QEMU: Supported**
3. RTC init → `RTC_CNTL` — **QEMU: Supported** (clears WiFi/BT force PD, configures timers)
4. MSPI/PSRAM init → SPI flash timing — **QEMU: Supported**
5. Clock init → writes `SYSTEM_PERIP_CLK_EN0/1_REG`, `SYSTEM_PERIP_RST_EN0/1_REG` — **QEMU: Supported** (register-level model in `esp32s3_clk.c`)
   - Also writes `SYSTEM_WIFI_CLK_EN_REG` / `SYSCON_WIFI_CLK_EN_REG` — **QEMU: Supported** via `esp32s3_syscon.c`
6. Interrupt matrix clear — **QEMU: Supported**

### OS Init Phase (from `system_init_fn.txt`)
7. eFuse check — **QEMU: Supported**
8. SysTimer init — **QEMU: Supported**
9. Brownout detection → RTC_CNTL — **QEMU: Supported**
10. UART/USB VFS init — **QEMU: Supported**
11. Flash init — **QEMU: Supported**
12. **ASSIST_DEBUG** stack guard → `0x600CE000` — **QEMU: Supported** (`esp32s3_assist_debug.c`, register-level model)
13. RNG clock enable — **QEMU: Supported**

### Wi-Fi Init Path (from `wifi_init.c` + `esp_adapter.c`)
- `periph_module_enable(PERIPH_WIFI_MODULE)` → `SYSCON_WIFI_CLK_EN_REG`
- `periph_module_reset(PERIPH_WIFI_MODULE)` → `SYSCON_WIFI_RST_EN_REG`
- `esp_phy_enable()` → writes BB/NRX/FE/FE2 registers
- PHY calibration → REGI2C analog writes (**QEMU: Supported** via `esp32s3_regi2c.c` stub; `BBPLL_CAL_DONE` reported set)

### BLE Init Path (from `bt/controller/esp32c3/bt.c`)
- `esp_bt_power_domain_on()` → `RTC_CNTL_DIG_PWC_REG` — **QEMU: Supported** (RTC_CNTL model absorbs writes)
- `periph_module_enable(PERIPH_BT_MODULE)` → BT bits in `SYSCON_WIFI_CLK_EN_REG` — **QEMU: Supported** (SYSCON stores bits)
- `btdm_controller_init()` → closed-source blob at `0x60011000` — **QEMU: Partial** (R/W register store absorbs blob accesses)
- 7 interrupt sources: BT_MAC(4), BT_BB(5), BT_BB_NMI(6), RWBT(7), RWBLE(8), RWBT_NMI(9), RWBLE_NMI(10) — **QEMU: Supported** (all wired to interrupt matrix)
- QEMU HCI transport (at BT_BASE+0xF00): bypasses blob for clean HCI command/event pipe
- Virtual BLE peer: deterministic advertising reports + connection for testing

---

## 5) SoC Capabilities (from `soc_caps.h`)

| Capability | Value | QEMU Status |
|-----------|-------|-------------|
| `SOC_CPU_CORES_NUM` | 2 | Supported |
| `SOC_GPIO_PIN_COUNT` | 49 | Supported |
| `SOC_I2C_NUM` | 2 | Supported (S5) |
| `SOC_I2S_NUM` | 2 | Supported (MVP) |
| `SOC_UART_NUM` | 3 | Supported |
| `SOC_LEDC_CHANNEL_NUM` | 8 | Supported (S5) |
| `SOC_SPI_PERIPH_NUM` | 3 | Partial (SPI1) |
| `SOC_GDMA_PAIRS_PER_GROUP` | 5 | Supported |
| `SOC_TIMER_GROUPS` | 2 | Supported |
| `SOC_RMT_CHANNELS_PER_GROUP` | 8 | Supported (MVP) |
| `SOC_MCPWM_GROUPS` | 2 | Supported (MVP) |
| `SOC_PCNT_UNITS_PER_GROUP` | 4 | Supported (S5) |
| `SOC_TWAI_CONTROLLER_NUM` | 1 | Supported |
| `SOC_WIFI_SUPPORTED` | 1 | Partial (STA/AP/AP_STA data plane) |
| `SOC_BLE_SUPPORTED` | 1 | Partial (S6: register model + QEMU HCI + virtual peer) |
| `SOC_PHY_COMBO_MODULE` | 1 | Partial (shared BB/FE with Wi-Fi) |

---

## 6) Complete Peripheral Register Map

From ESP-IDF `reg_base.h` cross-referenced with QEMU:

| Address | Peripheral | Size | QEMU Status | Sprint |
|---------|-----------|------|-------------|--------|
| `0x60000000` | UART0 | 0x1000 | **Supported** | — |
| `0x60002000` | SPI1 (Flash) | 0x1000 | **Supported** | — |
| `0x60003000` | SPI0 (Cache) | 0x1000 | **Supported** | — |
| `0x60004000` | GPIO | 0x1000 | **Supported** | — |
| `0x60005000` | FE2 | 0x1000 | Wi-Fi sub-block | — |
| `0x60006000` | FE | 0x1000 | Wi-Fi sub-block | — |
| `0x60007000` | eFuse | 0x1000 | **Supported** | — |
| `0x60008000` | RTC_CNTL | 0x400 | **Supported** | — |
| `0x60008400` | RTC_IO | 0x400 | **Supported** (stub, S5) | S5 ✅ |
| `0x60008800` | SENS (ADC/Touch) | 0x800 | **Supported** (stub, S8) | S8 ✅ |
| `0x60009000` | IO_MUX | 0x1000 | **Supported** | — |
| `0x60010000` | UART1 | 0x1000 | **Supported** | — |
| `0x60011000` | **BT Controller** | 0x1000 | **Supported** (S6: R/W store + QEMU HCI transport) | **S6 ✅** |
| `0x60013000` | **I2C0** | 0x200 | **Supported** (S5) | **S5 ✅** |
| `0x60014000` | UHCI0 | 0x1000 | Catch-all | S8 |
| `0x60015000` | SLC Host | 0x1000 | Wi-Fi sub-block | — |
| `0x60016000` | RMT | 0x1000 | **Supported (MVP)** | — |
| `0x60017000` | **PCNT** | 0x100 | **Supported** (S5) | **S5 ✅** |
| `0x60018000` | SLC | 0x1000 | Wi-Fi sub-block | — |
| `0x60019000` | **LEDC** | 0x100 | **Supported** (S5) | **S5 ✅** |
| `0x6001CC00` | NRX | 0x400 | Wi-Fi sub-block | — |
| `0x6001D000` | BB | 0x1000 | Wi-Fi sub-block | — |
| `0x6001E000` | **MCPWM0** | 0x128 | **Supported** (MVP) | S7 ✅ |
| `0x6001F000` | TIMG0 | 0x1000 | **Supported** | — |
| `0x60020000` | TIMG1 | 0x1000 | **Supported** | — |
| `0x60023000` | SysTimer | 0x1000 | **Supported** | — |
| `0x60024000` | SPI2 | 0x1000 | **Supported** (MVP) | S7 ✅ |
| `0x60025000` | SPI3 | 0x1000 | **Supported** (MVP) | S7 ✅ |
| `0x60026000` | **SYSCON** | 0x400 | **Supported** (register model) | **S3 ✅** |
| `0x60027000` | **I2C1** | 0x200 | **Supported** (S5) | **S5 ✅** |
| `0x60028000` | SDMMC | 0x1000 | **Supported** | — |
| `0x6002B000` | TWAI | 0x1000 | **Supported** | — |
| `0x6002E000` | UART2 | 0x1000 | **Supported** | — |
| `0x60035000` | WDEV (RNG) | 0x100 | **Supported** | — |
| `0x60038000` | USB_SERIAL_JTAG | 0x1000 | **Supported** | — |
| `0x6003A000` | AES | 0x1000 | **Supported** | — |
| `0x6003B000` | SHA | 0x1000 | **Supported** | — |
| `0x6003C000` | RSA | 0x1000 | **Supported** | — |
| `0x6003D000` | DS | 0x1000 | **Supported** | — |
| `0x6003E000` | HMAC | 0x1000 | **Supported** | — |
| `0x6003F000` | GDMA | 0x1000 | **Supported** | — |
| `0x60040000` | APB_SARADC | 0x1000 | **Supported** (stub, S8) | S8 ✅ |
| `0x60041000` | LCD_CAM | 0x1000 | **Supported** (MVP) | S7 ✅ |
| `0x600C0000` | **SYSTEM** | 0x1000 | **Supported** (expanded register model) | **S3 ✅** |
| `0x600C1000` | PMS/SENSITIVE | 0x1000 | **Supported** | — |
| `0x600C2000` | Interrupt Matrix | 0x1000 | **Supported** | — |
| `0x600C4000` | EXTMEM/Cache | 0x1000 | **Supported** | — |
| `0x600CC000` | XTS-AES | 0x1000 | **Supported** | — |
| `0x600CE000` | **ASSIST_DEBUG** | 0x200 | **Supported** (register model) | **S3 ✅** |
| `0x6000E000` | **REGI2C** | 0x100 | **Supported** (analog stub) | **S3 ✅** |
| `0x600D0000` | USB OTG (DWC2) | 0xE08 | **Supported** (stub/MVP) | S7 ✅ |
| `0x60021000` | RTC_SLOWMEM / ULP memory | 0x2000 | **Supported** (ULP stub, S8) | S8 ✅ |
| `0x600D2000` | COEX (QEMU virtual) | 0x20 | **Supported** (coex model, S8) | S8 ✅ |

---

## 7) Wi-Fi Implementation Plan

### 7.1 Current state (Sprint S4 complete)
- 7 sub-block MMIO regions mapped (BB, NRX, FE, FE2, SLC, SLCHOST, WDEV)
- SLC DMA engine with descriptor ownership transitions + frame extraction
- SLC interrupt RAW/ST/ENA/CLR semantics
- Full 27-bit SLC interrupt bitmap (+TX_START/RX_START + descriptor error paths)
- Init-critical register defaults (PHY power-down bits)
- SYSCON register model in place (`0x60026000`)
- REGI2C analog stub in place (`0x6000E000`) for PHY calibration
- **All 4 Wi-Fi IRQs wired** to interrupt matrix (MAC, MAC_NMI, PWR, BB)
- **QEMU NIC integration** — host networking via slirp/TAP/socket backends
- **802.11 ↔ Ethernet frame bridge** — bidirectional conversion through SLC DMA
- **Virtual AP** for STA mode — beacon, probe-resp, auth, assoc state machine
- **STA/AP/AP_STA mode support** — mode-aware frame routing and association
- **WDEV register intercepts** — MAC address configuration from blob writes
- OpenCores Ethernet replaced by native Wi-Fi NIC (IRQ source 0 conflict resolved)

### 7.2 Work package status

**WP-W1** (S3): SYSCON register model — ✅ done  
**WP-W2** (S3): PHY calibration stub (REGI2C) — ✅ done  
**WP-W3** (S4): Wi-Fi backend abstraction — ✅ done (`esp32s3_wifi_backend.c/.h`)  
**WP-W4** (S4): MAC frame bridge — ✅ done (SLC TX→Ethernet, Ethernet→SLC RX)  
**WP-W5** (S4): MAC IRQ routing — ✅ done (all 4 sources, OpenCores conflict resolved)  
**WP-W6** (S4): Management frame handling — ✅ done (beacon/probe/auth/assoc)  
**WP-W7** (S4): Deterministic virtual backend — ✅ done (fixed BSSID, virtual clock beacons)

### 7.3 Validation set
- `examples/wifi/getting_started/station`
- `examples/protocols/sockets/tcp_client`
- `examples/protocols/sockets/tcp_server`

---

## 8) Bluetooth Implementation Plan

### 8.1 Current state: BLE controller MVP implemented (S6/S7)

- BT controller register model is present at `0x60011000` with 7 IRQ outputs wired to the interrupt matrix
- VHCI/HCI transport and ACL/L2CAP/ATT handling are implemented in `esp32s3_bt.c`
- Virtual BLE peer is implemented with deterministic advertising and connection flow
- GATT database and basic ATT handlers are present (MVP scope)
- Remaining work is mainly fidelity/polish (SMP completeness, broader profile behavior, coexistence timing)

### 8.2 BT clock/reset path
On ESP32-S3, BT clocking is via `SYSCON_WIFI_CLK_EN_REG` (shared with WiFi):
- BT baseband enable: bit 11
- BT link controller: bits 16-17
- BT PHY: shared bits 0-3 with WiFi

BT reset via `SYSCON_WIFI_RST_EN_REG`:
- `BTBB_RST` (bit 3), `BTMAC_RST` (bit 4), `RW_BTMAC_RST` (bit 9), `RW_BTLP_RST` (bit 10), `RW_BTMAC_REG_RST` (bit 11), `RW_BTLP_REG_RST` (bit 12), `BTBB_REG_RST` (bit 13)

### 8.3 Work packages

**WP-B1** (S6): BT controller skeleton at `0x60011000`, 7 IRQ sources  
**WP-B2** (S6): Blob init support — absorb `btdm_controller_init()` register writes  
**WP-B3** (S6): VHCI transport — command/event/ACL data queues  
**WP-B4** (S6): Virtual BLE peer — deterministic testing without host BT stack  
**WP-B5** (S7): BLE features — advertising, scanning, connection, GATT

### 8.4 Validation set
- `examples/bluetooth/nimble/bleprph`
- `examples/bluetooth/nimble/blecent`

---

## 9) Sprint Plan (S3-S8)

### Sprint S3 (2 weeks) — System Infrastructure ✅ COMPLETE
**Goal**: Fix system register models so `esp_perip_clk_init()` and peripheral clock gating work.

| ID | P | Size | Status | Description |
|----|---|------|--------|-------------|
| S3-T1 | P0 | M | ✅ Done | **SYSCON register model** — `esp32s3_syscon.c` at `0x60026000`. 0x400-byte space with R/W. Critical: `WIFI_CLK_EN` (default `0xFFFCE030`), `WIFI_RST_EN`, `CLK_CONF`, `FRONT_END_MEM_PD`, `CLK_OUT_EN`, DATE. |
| S3-T2 | P0 | M | ✅ Done | **SYSTEM register expansion** — extended `esp32s3_clk.c` for `PERIP_CLK_EN0/1` (default `0xF9C1A7C0`/`0x184`), `PERIP_RST_EN0/1`, `CPU_PERI_CLK_EN`, `CPU_PERI_RST_EN`, `MEM_PD_MASK`, `CORE_1_CONTROL_0`, and 10+ more registers. |
| S3-T3 | P1 | S | ✅ Done | **ASSIST_DEBUG** — `esp32s3_assist_debug.c` at `0x600CE000`. Dual-core R/W for `SP_SPILL_MIN/MAX`, `SP_UNSTALL`, interrupt regs. IRQ → `ETS_ASSIST_DEBUG_INTR_SOURCE`. |
| S3-T4 | P1 | S | ✅ Done | **REGI2C analog stub** — `esp32s3_regi2c.c` at `0x6000E000`. Absorbs PHY calibration I2C writes. `BBPLL_CAL_DONE` always set. |
| S3-T5 | P1 | S | ✅ Done | **Wi-Fi SLC error/overflow** — complete M1-T5. Full 27-bit interrupt bit map, TX_START/RX_START on DMA link start. |

### Sprint S4 (3 weeks) — Wi-Fi Data Plane ✅ COMPLETE
**Goal**: End-to-end Wi-Fi STA/AP/AP_STA: init → connect → DHCP → TCP.

| ID | P | Size | Status | Description |
|----|---|------|--------|-------------|
| S4-T1 | P0 | L | ✅ Done | **Wi-Fi backend** — `esp32s3_wifi_backend.c/.h`. QEMU NIC integration (slirp/TAP/socket). Ethernet ↔ 802.11 frame conversion. STA/AP/AP_STA mode support. MAC address management. |
| S4-T2 | P0 | L | ✅ Done | **MAC frame bridge** — SLC TX→extract 802.11→convert to Ethernet→QEMU NIC. Host RX→convert Ethernet to 802.11+RX metadata→inject SLC RX DMA. Descriptor ownership flips + RX_DONE/RX_EOF interrupts. Fallback raw Ethernet path. |
| S4-T3 | P0 | M | ✅ Done | **MAC IRQ routing** — all 4 Wi-Fi IRQs connected: MAC→`ETS_WIFI_MAC_INTR_SOURCE`, NMI→`ETS_WIFI_MAC_NMI_SOURCE`, PWR, BB. OpenCores Ethernet removed from default init (IRQ source 0 conflict resolved). |
| S4-T4 | P0 | L | ✅ Done | **Management frames** — Virtual AP for STA mode: beacon (timer-driven), probe-response, auth (Open System), assoc with AID/rates. WDEV register intercept for MAC address. Association state machine (IDLE→AUTH→ASSOC). Deauth/disassoc handling. |
| S4-T5 | P1 | M | ✅ Done | **Deterministic virtual backend** — fixed BSSID (`02:00:00:00:00:01`), configurable SSID (`QEMU_WIFI`), virtual clock beacons (100 TU), seeded sequence numbers, fixed RSSI values (-30/-40 dBm). |

### Sprint S5 (2 weeks) — Peripheral Batch 1 (I2C, LEDC, PCNT)
**Goal**: Three most commonly used missing peripherals.

| ID | P | Size | Description |
|----|---|------|-------------|
| S5-T1 | P1 | L | **I2C** (×2) — `esp32s3_i2c.c`. 0x200 space, 8 cmd regs, TX/RX FIFO (32), 18 IRQ bits. MVP: master immediate completion. IRQ: `ETS_I2C_EXT0/1_INTR_SOURCE`. Ref: `i2c_struct.h` |
| S5-T2 | P1 | M | **LEDC** — `esp32s3_ledc.c`. 0x100 space, 8 channels, 4 timers, 20 IRQ bits. MVP: duty_rd = duty write. IRQ: `ETS_LEDC_INTR_SOURCE`. Ref: `ledc_struct.h` |
| S5-T3 | P2 | S | **PCNT** — `esp32s3_pcnt.c`. 0x100 space, 4 units, 4 IRQ bits, global ctrl. IRQ: `ETS_PCNT_INTR_SOURCE`. Ref: `pcnt_struct.h` |
| S5-T4 | P2 | S | **RTC_IO stub** (`0x60008400`) — R/W store for RTC GPIO pads. Used by sleep/analog mux. |

### Sprint S6 (3 weeks) — BLE Controller MVP
**Goal**: `esp_bt_controller_init` + `enable` succeed, advertising starts.

| ID | P | Size | Description |
|----|---|------|-------------|
| S6-T1 | P0 | L | **BT controller model** — `esp32s3_bt.c` at `0x60011000` (8K). R/W store + init defaults. 7 IRQ outputs. |
| S6-T2 | P0 | L | **Blob init support** — trace + absorb `btdm_controller_init()` register writes. Shared BB/FE response. |
| S6-T3 | P0 | M | **VHCI transport** — command/event/ACL queues between controller and NimBLE/Bluedroid host. |
| S6-T4 | P1 | M | **Virtual BLE peer** — internal adv responder + connection acceptor for CI. |

### Sprint S7 (3 weeks) — BLE Features + Peripheral Batch 2 ✅ COMPLETE
**Goal**: BLE GATT end-to-end. USB OTG, I2S, SPI2/3, MCPWM stubs.

| ID | P | Size | Description | Status |
|----|---|------|-------------|--------|
| S7-T1 | P0 | L | **BLE GATT** — ACL/L2CAP/ATT processing, GATT database (GAP+GATT+Custom), 7 ATT handlers, Disconnect/ConnUpdate. | ✅ Done |
| S7-T2 | P1 | M | **USB OTG** — DWC2 register stub at `0x60080000` (corrected). Returns correct GHWCFG1-4. IRQ: `ETS_USB_INTR_SOURCE`. | ✅ Done |
| S7-T3 | P2 | M | **I2S** (×2) — R/W store + TX immediate completion. I2S0=`0x6000F000`, I2S1=`0x6002D000`. | ✅ Done |
| S7-T4 | P2 | M | **SPI2/SPI3** — GP-SPI R/W + CMD_USR immediate completion. SPI2=`0x60024000`, SPI3=`0x60025000`. | ✅ Done |
| S7-T5 | P2 | M | **MCPWM** (×2) — R/W store + 30 interrupt bits. MCPWM0=`0x6001E000`, MCPWM1=`0x6002C000`. | ✅ Done |
| S7-T6 | P3 | M | **LCD_CAM stub** — R/W store + LCD_TRANS_DONE. At `0x60041000`. | ✅ Done |

### Sprint S8 (2 weeks) — Coexistence & Polish
**Goal**: Wi-Fi + BLE coex stable, CI pipeline green.

| ID | P | Size | Description | Status |
|----|---|------|-------------|--------|
| S8-T1 | P1 | L | **Coexistence** — shared radio time model for combo PHY. Modes: balanced, throughput, BLE-latency. | ✅ Done |
| S8-T2 | P1 | M | **Deterministic mode** — fixed-seed RNG (xorshift64), bounded jitter, repeatable queues. | ✅ Done |
| S8-T3 | P1 | M | **CI soak tests** — 30-60 min concurrent Wi-Fi + BLE. | 🟨 In Progress |
| S8-T4 | P2 | S | **ADC/Touch stub** — APB_SARADC + SENS R/W store. | ✅ Done |
| S8-T5 | P2 | S | **ULP stub** — minimal ULP interface for status checks. | ✅ Done |

---

## 10) Phased Delivery Summary

| Phase | Sprints | Duration | Key Deliverables |
|-------|---------|----------|-----------------|
| 0 — Foundation | S1-S2 ✅ | 4 weeks | IO MUX, GPIO Matrix, RMT, Wi-Fi control plane |
| 1 — System Infra | S3 ✅ | 2 weeks | SYSCON model, SYSTEM expansion, ASSIST_DEBUG, REGI2C stub |
| 2 — Wi-Fi Data | S4 ✅ | 3 weeks | Wi-Fi backend, MAC frame bridge, STA/AP/AP_STA, mgmt frames |
| 3 — Peripherals | S5 ✅ | 2 weeks | I2C, LEDC, PCNT, RTC_IO |
| 4 — BLE | S6 ✅ | 3 weeks | BT controller, blob support, HCI transport |
| 5 — Features | S7 ✅ | 3 weeks | BLE GATT, USB OTG, I2S, SPI2/3, MCPWM, LCD_CAM |
| 6 — Polish | S8 | 2 weeks | Coexistence, deterministic mode, CI soak |

**Total: ~15 weeks from S3 start** (single contributor estimate).

---

## 11) Test and CI Strategy

### 11.1 Test layers
1. **qtest unit tests** — register behavior and IRQ semantics per peripheral
2. **Integration tests** — device interactions, DMA flows, peripheral chaining
3. **System boot tests** — ESP-IDF bootloader + app boot to main
4. **ESP-IDF example tests** — specific example apps run to criterion
5. **Soak tests** — networking/BLE stability (30-60 min)

### 11.2 Deterministic mode
- Fixed-seed RNG, bounded scheduling jitter, repeatable packet queues, explicit timeout envelopes

### 11.3 ESP-IDF validation examples

| Category | Example | Sprint |
|----------|---------|--------|
| Boot | `examples/get-started/hello_world` | S3 |
| GPIO | `examples/peripherals/gpio/generic_gpio` | S3 |
| Wi-Fi | `examples/wifi/getting_started/station` | S4 |
| TCP/IP | `examples/protocols/sockets/tcp_client` | S4 |
| I2C | `examples/peripherals/i2c/i2c_tools` | S5 |
| LEDC | `examples/peripherals/ledc/ledc_fade` | S5 |
| BLE | `examples/bluetooth/nimble/bleprph` | S6-S7 |
| Crypto | `examples/protocols/https_request` | S4 |
| Coexist | Concurrent Wi-Fi+BLE custom test | S8 |

---

## 12) New File Structure Plan

```
# Sprint S3 ✅
qemu/hw/misc/esp32s3_syscon.c             # SYSCON register model         ✅ created
qemu/include/hw/misc/esp32s3_syscon.h                                      ✅ created
qemu/hw/misc/esp32s3_assist_debug.c       # ASSIST_DEBUG                   ✅ created
qemu/include/hw/misc/esp32s3_assist_debug.h                                ✅ created
qemu/hw/misc/esp32s3_regi2c.c             # REGI2C analog bus stub         ✅ created
qemu/include/hw/misc/esp32s3_regi2c.h                                      ✅ created

# Sprint S4 ✅
qemu/hw/net/esp32s3_wifi_backend.c        # Wi-Fi host backend             ✅ created
qemu/include/hw/net/esp32s3_wifi_backend.h                                  ✅ created
# Also modified:
# qemu/hw/net/esp32s3_wifi.c             # + backend integration, WDEV intercepts, NIC props
# qemu/include/hw/net/esp32s3_wifi.h     # + backend state, WDEV offsets, SLC API export
# qemu/hw/xtensa/esp32s3.c              # + MAC IRQ wiring, OpenCores removal
# qemu/hw/net/meson.build               # + esp32s3_wifi_backend.c

# Sprint S5 ✅
qemu/hw/i2c/esp32s3_i2c.c                # I2C controller
qemu/include/hw/i2c/esp32s3_i2c.h
qemu/hw/misc/esp32s3_ledc.c              # LED PWM controller
qemu/include/hw/misc/esp32s3_ledc.h
qemu/hw/misc/esp32s3_pcnt.c              # Pulse counter
qemu/include/hw/misc/esp32s3_pcnt.h
qemu/hw/misc/esp32s3_rtc_io.c            # RTC IO MUX stub
qemu/include/hw/misc/esp32s3_rtc_io.h

# Sprint S6 ✅
qemu/hw/misc/esp32s3_bt.c                # BLE controller + VHCI + virtual peer
qemu/include/hw/misc/esp32s3_bt.h

# Sprint S7 ✅
qemu/hw/misc/esp32s3_i2s.c               # I2S ×2 controller              ✅ created
qemu/include/hw/misc/esp32s3_i2s.h                                          ✅ created
qemu/hw/misc/esp32s3_gpspi.c             # GP-SPI (SPI2/SPI3)              ✅ created
qemu/include/hw/misc/esp32s3_gpspi.h                                        ✅ created
qemu/hw/misc/esp32s3_mcpwm.c             # MCPWM ×2 controller             ✅ created
qemu/include/hw/misc/esp32s3_mcpwm.h                                        ✅ created
qemu/hw/misc/esp32s3_lcd_cam.c           # LCD_CAM stub                    ✅ created
qemu/include/hw/misc/esp32s3_lcd_cam.h                                      ✅ created
qemu/hw/misc/esp32s3_usb_otg.c           # USB OTG DWC2 stub               ✅ created
qemu/include/hw/misc/esp32s3_usb_otg.h                                      ✅ created
```

---

## 13) Risks and Mitigations

1. **Closed-source blob register expectations** — Wi-Fi/BT blobs write undocumented registers.
   - Mitigation: trace all MMIO writes during blob init, build absorb-and-acknowledge model.
2. **PHY calibration complexity** — `register_chipv7_phy()` does extensive analog writes.
   - Mitigation: REGI2C stub absorbs writes; success depends on non-fault return.
3. **DWC2 adaptation** — QEMU's DWC2 may need ESP32-S3 specific GHWCFG tuning.
   - Mitigation: start device-mode only; match `GHWCFG` from `usb_dwc_cfg.h`.
4. **Timing races** — interrupt delivery timing affects driver behavior.
   - Mitigation: deterministic mode + trace debugging + stress tests.
5. **Scope explosion** — many peripherals, each with unique register maps.
   - Mitigation: strict R/W-store MVP policy; full behavior only when drivers require it.

---

## 14) Doc Synchronization

Keep synchronized with:
- `docs/esp32s3_qemu_differences.md`
- `docs/esp32s3_qemu_detailed_comparison.md`
- `docs/esp32s3_execution_backlog.md`

ESP-IDF reference register definitions:
- `esp-idf/components/soc/esp32s3/register/soc/` — authoritative register structs
- `esp-idf/components/soc/esp32s3/include/soc/` — SoC caps, peripheral defs, interrupt mappings
- `esp-idf/components/hal/` — HAL layer expected register access patterns