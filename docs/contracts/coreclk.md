# coreclk contract — clock tree, peripheral gates and reset domains (H-CORE-01)

Status: prototype-unqualified (qtest + boot-smoke qualified on the lane
binary; shared-parent behavior preserved for other machines). Base
`40edccac415693c5130f91c01d84176ae6008566`, series
`qemu-extensions/prototypes/coreclk/0001..0007`, apply with
`git am --keep-cr`.

## Clock tree

* Source of truth: the SYSTEM clock device (`esp32s3.soc.clk`, MMIO
  0x600C0000). All frequencies are recomputed from `SYSTEM_SYSCLK_CONF` /
  `SYSTEM_CPU_PER_CONF` and propagated on qdev Clock outputs.
* Outputs: `cpu-clk`, `apb-clk`, `xtal-clk`, `rtc-slow-clk`, `rtc-fast-clk`.
  Public recompute entry points:
  `esp32s3_clk_recompute_system(ESP32S3ClockState *)` and
  `esp32s3_clk_set_rtc_clocks(ESP32S3ClockState *, rtc_slow_hz, rtc_fast_hz)`
  in `include/hw/xtensa/esp32s3_clk.h`.
* Consumers wired by the SoC (0005): CPU cores (`clk-in` = CPU_CLK),
  TIMG0/1 (`apb-clk`/`xtal-clk`, per-timer `USE_XTAL`), SYSTIMER
  (`xtal-clk`, CNT_CLK = 2/5 × XTAL), RTC_CNTL (`xtal-clk`, XTAL/2
  RTC_FAST). Future consumers (ADC lane: `apb-clk`, `rtc-fast-clk`) connect
  in their own lane's `esp32s3.c` patch; the output names are stable.
* Reset frequencies (TRM v1.8 Register 17.20 truth): SOC_CLK_SEL=0 (XTAL),
  PRE_DIV_CNT=1 → CPU = APB = XTAL/2 = 20 MHz; XTAL 40 MHz; RTC_SLOW 136
  kHz (RC_SLOW); RTC_FAST 20 MHz (XTAL/2). The ROM reconfigures the PLL.
* `SYSTEM_SYSCLK_CONF` writes: `SOC_CLK_SEL`/`PRE_DIV_CNT`/`CLK_DIV_EN`
  store, `CLK_XTAL_FREQ` is read-only (TRM Register 17.20).
* Gate semantics (`SYSTEM_PERIP_CLK_EN0/1`, IDF6.1 field defaults,
  EN0=0x6181A06F, EN1=0x600): TIMG timers = TIMERS[0] AND TIMERGROUP[13] /
  TIMERGROUP1[15]; TIMG MWDT = WDG[3] AND group. A gated group makes zero
  progress (counters freeze, alarms disarm and resume with the remaining
  ticks, RTC calibration does not complete).
* `SYSTEM_PERIP_RST_EN0/1` 0→1 edges emit a cold reset to the matching
  modeled device (strobe approximation of the level-held reset; enumerated
  in `Esp32s3PeriphRstReq`).

## Reset domains

* `esp32s3_soc_reset` maps SW/chip resets onto domains: PERIPH = SYSTEM +
  intmatrix + UART×3 + TIMG×2 + SYSTIMER + GDMA + SPI1 + SDMMC + TWAI +
  AES/SHA/RSA/HMAC/DS + XTS + RNG + GPIO + PMS; RTC = RTC_CNTL; PROCPU /
  APPCPU = core only. eFuse (OTP), USB-Serial/JTAG console and the RGB
  framebuffer are intentionally outside the digital domain reset.
* Devices with clearing hold phases consult
  `esp32s3_reset_covers_periph/_rtc` (`include/hw/xtensa/esp32s3_reset_domain.h`)
  through the `soc-reset` link, so the blanket QEMU device reset only clears
  state when the pending domain covers it. Without the link (classic ESP32
  machines) behavior is unchanged.
* The pending domain is latched until the next reset request or a PERIP_RST
  strobe: a single reset request can dispatch through multiple
  `qemu_system_reset` passes and devices must keep seeing the requested
  domain.

## Integration notes

* 0007 (I2C_MST bridge at 0x6000E000) is **SUPERSEDED-BY-REGI2C** — drop it
  when the REGI2C lane owns that window; it is load-bearing until then
  (recalib_bbpll polls 0x6000E040 bit24 exactly once per ordinary boot).
* The REGI2C/ADC lanes must anchor their `esp32s3.c` edits away from the
  three coreclk regions: top of `esp32s3_soc_realize` (first-stage clock
  wiring), the block before `/* Timer Groups realization */`, and the
  second-stage block after the `rgb` `object_initialize_child` line.

## Evidence

`build-runtime-state/coreclk-2026-10-07/` — qtest transcript (9/9), boot
smoke logs (3/3 on the final binary, sha256
74cee5437206e977feb8e461b1b5d9a5aaf3ba1bcb0acc9e4bc6116333ba93a6), patch
sha256s, command transcripts, pristine-clone `git am --keep-cr` apply log
and standalone build result.
