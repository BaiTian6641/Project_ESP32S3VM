# H-NET-02: ESP32-S3 GPIO, IO MUX, GPIO matrix and RTC IO MUX models

Prototype lane replacing the official-base GPIO stub (`GPIO_STRAP`-only,
`hw/gpio/esp32_gpio.c`) and the `esp32s3.iomux` `create_unimplemented_device`
placeholder with real register/matrix/IRQ models, re-derived from the
ESP32-S3 TRM chapter 6 (IO MUX and GPIO Matrix) and cross-checked against the
ESP-IDF 6.1 SoC headers (`components/soc/esp32s3/register/soc/gpio_reg.h`,
`gpio_struct.h`, `io_mux_reg.h`, `rtc_io_reg.h`, `rtc_io_struct.h`).  The
recovered fork files were used as structural reference only.

Status: **prototype-unqualified** — register/matrix/IRQ semantics validated
by qtest and by a real IDF-driver firmware fixture; no hardware comparison.
License: GPL-2.0-or-later (matching the QEMU tree).

## Files

| Prototype file | Destination in the QEMU tree |
|---|---|
| `copies/esp32s3_iomux.h` | `include/hw/gpio/esp32s3_iomux.h` |
| `copies/esp32s3_iomux.c` | `hw/gpio/esp32s3_iomux.c` |
| `copies/esp32s3_rtc_io.h` | `include/hw/misc/esp32s3_rtc_io.h` |
| `copies/esp32s3_rtc_io.c` | `hw/misc/esp32s3_rtc_io.c` |
| `copies/esp32s3-gpio-test.c` | `tests/qtest/esp32s3-gpio-test.c` |

`integration.patch` carries every modification to tracked files:

- `hw/gpio/esp32_gpio.c` + `include/hw/gpio/esp32_gpio.h`: virtual
  `gpio_read`/`gpio_write` class dispatch; the default register file still
  implements only `GPIO_STRAP`, so the ESP32 (classic) and ESP32-C3 machines
  are unchanged.
- `hw/gpio/esp32s3_gpio.c` + `include/hw/gpio/esp32s3_gpio.h`: full rewrite
  into the real S3 model (see below).
- `hw/gpio/meson.build`, `hw/misc/meson.build`: build the new files under
  `CONFIG_XTENSA_ESP32S3`.
- `tests/qtest/meson.build`: `qtests_xtensa` with `esp32s3-gpio-test`.
- `hw/xtensa/esp32s3.c`: confined wiring — includes, SoC state members,
  child initialization, the GPIO/IO_MUX/RTC_IO realize+MMIO+IRQ block and
  removal of the `esp32s3.iomux` unimplemented placeholder.  No edits to
  `clk_update` or `soc_reset` (CoreClockWorker owns those regions).

## GPIO model (`esp32s3.gpio`, MMIO 0x60004000)

Register layout per TRM ch.6 / IDF `gpio_reg.h`:

- `GPIO_BT_SELECT` 0x000 (stored), `GPIO_OUT` 0x004, `OUT_W1TS` 0x008,
  `OUT_W1TC` 0x00C, `OUT1` banks 0x010-0x018 (22 bit),
  `GPIO_SDIO_SELECT` 0x01C, `GPIO_ENABLE` 0x020-0x02C (W1TS/W1TC),
  `GPIO_STRAP` 0x038 (RO), `GPIO_IN`/`IN1` 0x03C/0x040 (RO samples),
  `GPIO_STATUS` 0x044-0x050 (W1TS/W1TC), per-CPU masked views 0x05C-0x070
  (`PCPU_INT`, `PCPU_NMI_INT`, `CPUSDIO_INT`, `PCPU_INT1`,
  `PCPU_NMI_INT1`, `CPUSDIO_INT1`), `GPIO_PINn` 0x074 + 4n (n = 0..48),
  `STATUS_NEXT` 0x14C/0x150 (level conditions), 256 input selectors
  `GPIO_FUNCn_IN_SEL_CFG` 0x154 + 4n, 49 output selectors
  `GPIO_FUNCm_OUT_SEL_CFG` 0x554 + 4n, `CLOCK_GATE` 0x62C, `DATE` 0x6FC
  (reset 0x02101191).
- `GPIO_PINn`: `SYNC2_BYPASS` [1:0], `PAD_DRIVER` [2] (0 = push-pull,
  1 = open-drain), `SYNC1_BYPASS` [4:3], `INT_TYPE` [9:7]
  (0 dis, 1 rising, 2 falling, 3 any edge, 4 low level, 5 high level),
  `WAKEUP_ENABLE` [10], `CONFIG` [12:11] (stored), `INT_ENA` [17:13]
  (bit0 PROCPU level -> matrix source 16, bit1 APPCPU level -> source 18,
  bit2 PROCPU NMI -> source 17, bit3 APPCPU NMI -> source 19, bit4 SDIO).
- Pads 22..25 are holes on the ESP32-S3 chip (45 physical GPIOs; indexed
  register slots remain 0..48).  There are **no input-only pads** on S3 —
  the ESP32-classic GPIO34-39 rule does not apply; 26..32 are flash/module
  reservations by package, not register behavior.

## GPIO matrix

- Output: `FUNC_OUT_SEL` 256 = simple GPIO (`GPIO_OUT`/`GPIO_ENABLE` drive
  the pad); 0..255 select peripheral output signals — no model sources any
  yet, so the pad driver stays released; 257..511 reserved = no drive.
  `FUNC_OUT_INV_SEL`/`FUNC_OEN_SEL`/`FUNC_OEN_INV_SEL` per TRM; `OEN_SEL=1`
  takes OE from the (absent) signal, i.e. releases the pad.
- Input: 256 selectors store per TRM (`FUNC_IN_SEL` 6 bit pad/constant
  selection, `FUNC_IN_INV_SEL`, `SIG_IN_SEL`); the constants 0x38 (high)
  and 0x3C (low) are the reset default.  `esp32s3_gpio_matrix_input(s, n)`
  is the exported lookup for the first matrix-consuming peripheral (UART
  reroute, RMT, ...).  Behavioral verification of in-routing therefore
  lands with that consumer; this lane validates the register semantics.

## Electrical handoff (digital, pre-analog-gate)

- 49 named input lines `gpio-in` (external/solver drivers) and 49 named
  observation lines `gpio-out` (resolved pad node level, the digital view;
  future net layers must drive `gpio-in`, never feed `gpio-out` back —
  the raw snapshot below is the solver-side truth instead).
- RAW drive-configuration snapshot for the net solver (register truth,
  separate from the resolved line):
  `esp32s3_gpio_get_drive_snapshot()` -> `ESP32S3GpioDriveSnapshot`
  (mcu_sel/matrix_gpio, out_oe, out_sel/out_inv/oen_from_signal/oen_inv,
  out_level, open_drain, drive_strength, ie, pull_up/pull_down, slp_sel,
  hold=false, analog_owned=false), and
  `esp32s3_rtc_io_get_drive_snapshot()` -> `ESP32S3RtcIoDriveSnapshot`
  (mux_sel, out_bit/out_oe, open_drain, int_type, fun_ie, rue/rde, drv,
  slp_sel, xpd, hold=false).
- Resolution: own output driver wins; otherwise an external valid driver;
  otherwise IO_MUX `FUN_PU`/`FUN_PD` (reported as `PULLED_*`); otherwise
  the net is **unresolved (floating)**.  A pull is never an unconditional
  level against a conflicting driver.  Internal drive + valid external
  driver at a different level is reported as `CONTENTION`; until the
  analog gate resolves it, the explicit digital sampling policy reads the
  internal drive.
- **Floating is not valid low**: unresolved nets sample 0 in `GPIO_IN`
  under the documented permissive profile, but the unresolved state is
  separately observable: `esp32s3_gpio_pad_state()` returns
  `FLOATING/DRIVEN/EXTERNAL/PULLED_HIGH/PULLED_LOW/CONTENTION`,
  `esp32s3_gpio_net_valid()` the resolution flag.
  `esp32s3_gpio_drive_net(s, pad, valid, level, volts)` is the
  voltage/validity entry point for the future DC solver (volts are
  recorded and inert until the analog gate).
- Drive strength (`FUN_DRV`) is stored but electrically inert until the
  analog gate.  Push-pull-vs-push-pull contention diagnostics beyond the
  `CONTENTION` state are an analog-gate item.
- MODE BOUNDARY (explicit): the standalone permissive digital mode
  (strict-unknown=false, no electrical layer attached) is an UNQUALIFIED
  boundary for standalone use only — it is never a qualification path and
  never applies while an electrical graph is active.  In an active graph,
  enabled floating inputs are strict UNKNOWN (pause before consumption);
  on-die pulls (FUN_PU/FUN_PD, RTC RUE/RDE) resolve nets with actual
  hardware behavior; unpopulated pads (22-25) and disabled receivers
  (FUN_IE=0) read 0 per documented hardware read semantics — no false
  uncertainty for physically absent or disabled roles.
- `detach_net` is a topology-teardown API only — never a positive-test
  bypass: it returns a pad to standalone resolution when the solver
  removes it from the owned set.

## IO MUX (`esp32s3.iomux`, MMIO 0x60009000)

`IO_MUX_PIN_CTRL` 0x000, `IO_MUX_GPIOn` 0x004 + 4n (n = 0..48),
`IO_MUX_DATE` 0x0FC (reset 0x02006050).  Pad fields per TRM/IDF:
`SLP_OE` [0], `SLP_SEL` [1], `SLP_PD` [2], `SLP_PU` [3], `SLP_IE` [4],
`SLP_DRV` [6:5], `FUN_PD` [7], `FUN_PU` [8], `FUN_IE` [9], `FUN_DRV` [11:10],
`MCU_SEL` [14:12], `FILTER_EN` [15].  Reset value 0x800 (`FUN_DRV = 2`)
uniformly; pad-specific boot defaults of the flash pads (GPIO26-32) are
not modelled (the ROM/bootloader re-programs them).  `MCU_SEL != 1` hands
the pad to a direct IO_MUX function (none of those paths is modelled, so
the GPIO matrix drive releases — matrix disconnect is register-visible);
the GPIO matrix function on the S3 is `PIN_FUNC_GPIO = 1` (IDF
`io_mux_reg.h:140`; the classic ESP32 uses 2, and function values are
per-pad — no array assumptions).
Sleep-mode, filter and drive-strength bits are stored without electrical
effect.

## RTC IO MUX (`esp32s3.rtc_io`, MMIO 0x60008400)

22 RTC pads (RTC_GPIO0..21 = GPIO0..21) with the TRM register file:
`RTC_GPIO_OUT/ENABLE/STATUS` with W1TS/W1TC and `RTC_GPIO_IN` — all in
**bits [31:10]** (pad i at bit 10+i, per IDF `rtc_io_reg.h`); `RTC_GPIO_PINn`
0x28 + 4n (`PAD_DRIVER` [2], `INT_TYPE` [9:7], `WAKEUP_ENABLE` [10]);
`RTC_DEBUG_SEL` 0x080; per-pad configuration at 0x084..0xD8 with the TRM
pad mapping (pad 0 -> `TOUCH_PAD14` @0xBC, pads 1..14 -> `TOUCH_PAD0..13`
@0x84..0xB8, 15 -> `XTAL_32P` @0xC0, 16 -> `XTAL_32N` @0xC4, 17 -> `PAD_DAC1`
@0xC8, 18 -> `PAD_DAC2` @0xCC, 19..21 -> `RTC_PAD19..21` @0xD0..0xD8);
common fields `FUN_IE` [13], `SLP_OE` [14], `SLP_IE` [15], `SLP_SEL` [16],
`FUN_SEL` [18:17], `MUX_SEL` [19], `RUE` [27], `RDE` [28], `DRV` [30:29]
(reset: DRV = 2, RDE = 1); `EXT_WAKEUP0` 0xDC, `XTL_EXT_CTR` 0xE0,
`SAR_I2C_IO` 0xE4, `TOUCH_CTRL` 0xE8, `DATE` 0x1FC.
Named lines `rtcio-in`/`rtcio-out` (22 each) with the same validity
diagnostics as the digital GPIO (`esp32s3_rtc_io_pad_state()` and
`esp32s3_rtc_io_drive_net()`); RUE/RDE resolve released nets.

## Machine wiring (confined to the H-NET-02 allowance)

`hw/xtensa/esp32s3.c`: the three devices are realized and mapped
(GPIO 0x60004000, IO_MUX 0x60009000 replacing the unimplemented device,
RTC_IO 0x60008400); the GPIO link `iomux` is set; four IRQ lanes connect to
the interrupt matrix inputs `ETS_GPIO_INTR_SOURCE` (16, sysbus irq 0),
`ETS_GPIO_NMI_SOURCE` (17, `nmi-int`), `ETS_GPIO_INTR_SOURCE2` (18,
`app-int`) and `ETS_GPIO_NMI_SOURCE2` (19, `app-nmi`).  Firmware GPIO
matrix configuration is observed hardware state (real IDF driver writes);
the models never rewrite guest programming.

## Tests

- `tests/qtest/esp32s3-gpio-test` (xtensa-softmmu): reset defaults, W1TS/
  W1TC on OUT/ENABLE/STATUS incl. 22-bit "1" banks, RO input/negative
  registers, reserved-hole and out-of-range-pad writes, external input via
  qtest `set_irq_in` gated by IO_MUX `FUN_IE`, simple-GPIO electrical
  loopback with open-drain release, pull resolution and external-over-pull
  precedence, matrix output routing (peripheral signal = no drive,
  reserved = no drive, inversion, OE source/inversion), matrix input
  selector semantics, `GPIO_PINn` rising/falling/any-edge and high/low
  level interrupts with per-CPU/NMI masked readbacks and level re-assert,
  `GPIO_STATUS_NEXT`, IO_MUX `MCU_SEL` matrix disconnect and write masks,
  RTC_IO [31:10] alignment, edge interrupt, IN gating and pad defaults.
- `tests/firmware/gpio_net` (real IDF 6.1 gpio driver): `gpio_config`
  output toggle with `gpio_get_level` loopback, plus `gpio_isr_register`
  rising-edge interrupt on GPIO5 whose ISR provably fires when the model's
  `gpio-in` line is driven from an external qtest client
  (build-runtime-state/gpio-2026-10-07/scripts/).  This exercises the full
  path GPIO -> interrupt matrix -> CPU ISR without any firmware hook.
- BootSmokeTest 3/3 on the final binary (shared GUI boot-smoke runner).

## Explicit boundaries (not modelled here)

- No analog voltage on lines: solver voltages are recorded and inert until
  the analog gate; contention, drive strength and floating thresholds need
  that gate (NET-03/ANALOG-01).
- No RTC deep-sleep hold / SLP_* electrical behavior, no touch/analog sense
  path, no RTC power domains.
- RTC GPIO interrupt aggregation into RTC_CNTL
  (`RTC_CNTL_INT_ST_RTC_GPIO` -> `ETS_RTC_CORE_INTR_SOURCE`) is not wired;
  it belongs to the rtc_cntl device owner.
- GPIO matrix input selectors are stored and exported but have no
  behavioral consumer until a matrix-input peripheral (UART reroute, RMT)
  lands; in-routing verification is deferred to that lane.
- Direct IO_MUX peripheral functions (`MCU_SEL != 1`, e.g. U0TXD = 0 on
  GPIO43) disconnect the GPIO matrix but do not themselves drive pads
  (no such peripheral model yet).
- Pads 22..25 are unpopulated; the S3 has no input-only pads — verified
  against pinned IDF 6.1 `soc_caps.h`: `SOC_GPIO_VALID_GPIO_MASK`
  excludes only bits 22-25 and `SOC_GPIO_VALID_OUTPUT_GPIO_MASK` equals
  the valid mask (all 45 physical pads 0-21/26-48 are bidirectional;
  SOC_GPIO_PIN_COUNT = 49). The ESP32-classic input-only GPIO34-39 rule
  does not apply.
- No SDM (sigma-delta) output and no CPU-dedicated GPIO bundles (PIE /
  EE.* instructions) — separate H-NET-02 follow-up items.

## Evidence

`build-runtime-state/gpio-2026-10-07/` — commands, hashes, qtest output,
firmware fixture UART trace, BootSmokeTest log.

## Correction log (review-driven)

- Floating is not valid low: validity/diag API + raw drive snapshots
  (parent contract); strict-unknown property pauses consumption.
- S3 PIN_FUNC_GPIO = 1 (not 0, not classic-2): matrix_active check + QTest
  fixed; verified against build-idf-6.1 io_mux_reg.h (func lists per pad).
- Open-drain releases on output 1 (was inverted in the first cut).
- PCPU_INT1/PCPU_NMI_INT1 are the high-bank PROCPU views (classic naming);
  INT_ENA bit1/3 lanes assert matrix sources 18/19 with no status register.
- RTC_IO data fields sit at bits [31:10] (mask 0xFFFFFC00); RTC pad-0
  config register is TOUCH_PAD14 @0xBC per the TRM/IDF pad mapping.
- FUNC_OEN_SEL S3 polarity (I2CCoreReview, pinned gpio_ll evidence):
  0 = OE from the routed peripheral signal, 1 = OE from GPIO_ENABLE; the
  simple GPIO signal (256) hardwires OE to GPIO_ENABLE so OEN_SEL does
  not apply there. Snapshot `oen_from_signal` is the semantic boolean.
- Strict pre-consumption (NativeNet/parent): strict-unknown property +
  esp32s3_gpio_consume_unknown + unknown handlers; solver-owned pads
  reject legacy boolean line injection; default strict behavior pauses
  from the main context (BH), no auto-resume.
- Reset-domain gating (26dbe + core 0010..13): soc-reset links on
  GPIO/IOMUX (PERIPH domain) and RTC_IO (RTC domain) via
  esp32s3_reset_covers_periph/rtc; CPU-only resets preserve state.
- No GPIO matrix source store (parent decision): NativeNet
  electrical.set_matrix_drive is the single desired-output authority;
  GPIO publishes raw selector snapshots, strict esp32s3_gpio_matrix_sample
  and consume_unknown only.
