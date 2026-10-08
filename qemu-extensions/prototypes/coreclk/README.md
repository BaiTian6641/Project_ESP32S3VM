# coreclk — H-CORE-01 clock/reset ports and domain propagation

Lane branch `codex/coreclk` in `/home/polar/.cache/esp32s3vm/qemu-coreclk-40edccac4156`,
base `40edccac415693c5130f91c01d84176ae6008566`, GPL-2.0-or-later.
Frozen clock foundations are 0001..0014; 0007 is the superseded standalone
bridge and is excluded from the combined runtime. The additive CPU1 lifecycle
increment is 0015. Apply with `git am --keep-cr` (the repo stores CRLF; plain
`git am` strips the CR and 0001 fails on `esp32s3_clk_defs.h`).

## What the lane implements

* **Clock tree (0001)** — `esp32s3_clk` (SYSTEM, 0x600C0000) is the register
  source of truth and exposes qdev Clock outputs `cpu-clk`, `apb-clk`,
  `xtal-clk`, `rtc-slow-clk`, `rtc-fast-clk`. Every derived frequency is
  cited in code:
  * CPU: PLL480/{6,3,2} or PLL320/{4,2} by `CPUPERIOD_SEL`, or
    `XTAL/(PRE_DIV_CNT+1)`, or `RC_FAST/(PRE_DIV_CNT+1)` (17.5 MHz)
    — TRM v1.8 ch.7 Tables 7.2-1/7.2-2.
  * APB: 80 MHz from PLL, else = CPU_CLK — TRM Table 7.2-4.
  * XTAL: stored `CLK_XTAL_FREQ` = 40 MHz, read-only on writes — TRM
    Register 17.20.
  * RTC_SLOW: RC_SLOW 136 kHz / XTAL32K 32768 / RC_FAST_DIV (17.5M/256);
    RTC_FAST: XTAL/2 or RC_FAST — TRM ch.7 §7.2.4.4, ch.10, IDF6.1
    `clk_tree_defs.h`.
* **Reset SOC_CLK_SEL deviation fixed** — reset now follows TRM v1.8
  Register 17.20: `SOC_CLK_SEL=0` (XTAL), `PRE_DIV_CNT=1` → CPU/APB start at
  XTAL/2 = 20 MHz and the ROM reconfigures the PLL. Revalidated: boot smoke
  3/3 on the IDF 6.1 boot-smoke firmware with the XTAL start.
* **Gate/reset registers (0001)** — `PERIP_CLK_EN0/1`, `PERIP_RST_EN0/1`
  stored R/W; EN reset values follow the IDF6.1 field defaults
  (EN0=`0x6181A06F`, EN1=`0x600`, RST=0). Gate aggregations
  (TIMG timers = `TIMERS[0] AND TIMERGROUP[13]/[15]`, MWDT = `WDG[3] AND
  group`) are driven on gate outputs; `PERIP_RST` 0→1 edges emit per-device
  cold-reset strobes to the SoC.
* **Consumers (0003/0005)** — both CPU cores take `cpu-clk` on their
  existing `clk-in` input (Xtensa CCOUNT/CCOMPARE timing reads that clock);
  TIMG0/1 take `apb-clk`/`xtal-clk` (`TIMG_TxCONFIG.USE_XTAL` selects, TRM
  ch.12) plus the gate lines; SYSTIMER takes `xtal-clk` (CNT_CLK = 2/5 ×
  XTAL = 16 MHz, TRM ch.11 — deliberately invariant to CPU/APB dividers);
  RTC_CNTL takes `xtal-clk` (drives the XTAL/2 RTC_FAST rate).
* **Reset domains (0003/0004/0005)** — devices with clearing hold phases
  gate on `esp32s3_reset_covers_periph/_rtc` (new header) via a `soc-reset`
  link. `esp32s3_soc_reset` PERIPH domain cold-resets SYSTEM, intmatrix,
  UART×3, TIMG×2, SYSTIMER, GDMA, SPI1, SDMMC, TWAI, AES, SHA, RSA, HMAC,
  DS, XTS-AES, RNG, GPIO, PMS; RTC domain cold-resets RTC_CNTL. eFuse (OTP),
  USB-Serial/JTAG console and the RGB framebuffer are deliberately excluded
  (commented in code). The pending domain is latched because one reset
  request dispatches through more than one `qemu_system_reset` pass.
* **0007 TEMPORARY I2C_MST bridge** — read-only `0x6000E000` window
  answering `I2C_MST` `0x6000E040` bit24 (`I2C_BBPLL_CAL_DONE`)=1.
  **SUPERSEDED-BY-REGI2C**: drop at integration when the REGI2C lane model
  owns the window (the two collide). Evidence that it is load-bearing here:
  with the real clock semantics an ordinary boot reaches
  `rtc_clk_bbpll_configure` (recalib path) and polls that bit exactly once
  per run (warn-probe count `poll_hits=1`); without the window the boot
  spins in `rtc_clk_bbpll_configure` forever.

## Radio-lane compatibility

0001 keeps both radio hunks' anchors byte-stable: no `BT_LPCK` cases, reset
insertion point untouched, header fields added away from the radio hunk
context. Offsets 0x28/0x2C remain unimplemented in this lane's tree (the
radio patch supplies the storage). Series verified to apply in order onto a
pristine `40edccac` clone with `git am --keep-cr`.

## Historical verification (not inherited by 0015)

* `QTEST_QEMU_BINARY=./qemu-system-xtensa ./tests/qtest/esp32s3-coreclk-test`
  → 9/9 ok (list in `source-map.json`).
* Boot smoke (existing binary, no rebuild):
  `ESP32S3_QEMU_BIN=<lane>/build-coreclk/qemu-system-xtensa
  ESP32S3_BOOT_FIRMWARE=tests/firmware/boot_smoke/build-idf-6.1/boot-smoke.merged.bin
  QT_QPA_PLATFORM=offscreen ./gui-esp32s3-simulator/build-wsl/gui_esp32s3_boot_smoke_test`
  → 3 passed / 0 failed. Binary sha256
  `74cee5437206e977feb8e461b1b5d9a5aaf3ba1bcb0acc9e4bc6116333ba93a6`.
* CPU-core clock propagation evidence: raw QMP probe
  `qom-get /machine/soc/clock/cpu-clk` and `/machine/soc/cpu{0,1}/clk-in`
  `qtest-clock-period` = `53687091200` (80 MHz, `CLOCK_PERIOD_FROM_HZ`
  units), changing with `CPUPERIOD_SEL`. Caveat: the libqtest QMP channel
  returns a sign-truncated int32 for values > 2^31 (plain `-qmp unix:`
  channel returns the correct value) — use a raw QMP client for this probe.

## Known boundaries / unknowns

* SYSTIMER `SYSTIMER_CLK_EN` (EN0[29]) stored but not a counter gate (TRM
  ch.11: it gates the APB register access; CNT_CLK is XTAL-derived). APB
  access stalling is not modeled.
* GDMA `EN1[6]` gate not consumed for progress (instantaneous-transfer
  model; H-CORE-03); `DMA_RST` strobe cold-resets the device.
* WDT gate line implemented, not behaviorally qtested; `TIMGCLK.CLK_EN`
  register bit not consumed.
* `PERIP_RST` level-held semantics approximated as a rising-edge strobe
  (one cold reset per 0→1).
* TIMG hardware-divider encodings and exact rational tick accounting are
  supplied by 0008/0009; the earlier zero-divider defect is superseded.
* The libqtest QMP int32 truncation noted above.

Evidence: `build-runtime-state/coreclk-2026-10-07/`.

## CPU1 lifecycle (0015)

The implementation lives in the independent native checkout
`/home/polar/.cache/esp32s3vm/qemu-cpu-lifecycle-gate`, not the old clock lane's
mutable build directory. It reuses the surviving 0015 register/pin delta;
the previous owner reported that the complete uncommitted WIP and its
matching executable were lost. Neither the old WIP claims nor the historical
clock-suite/boot results qualify this increment.

`SYSTEM_CORE_1_CONTROL_0` implements only RUNSTALL [0], CLKGATE_EN [1], and
RESETING [2], with stored readback and reset value `4` (TRM v1.8 Register 17.1;
locked IDF `system_reg.h` lines 15–40). `CONTROL_1.MESSAGE` resets to zero.
CPU1 is cold-held; initial levels are replayed after the GPIO consumers are
connected. Gate disable preserves the CPU state, and SYSTEM hold/RUNSTALL,
RTC CPU1 stall, and pending CPU1 reset work combine without transient release.

A RESETING rising edge queues only CPU1's reset through `async_run_on_cpu`,
outside the requesting MMIO translation block. Repeated asserted writes do
not reset again. SYSTEM registers, clock configuration, peripherals, RTC,
and external attachments are not reset by this path. Release enters the
actual ROM reset vector; the ROM subsequently consumes MESSAGE. There is no
direct boot-address jump, ROM-PC special case, widened MMIO page, ROMD
shortcut, or increased hostbus deadline.

The pinned Xtensa runstall API raises `CPU_INTERRUPT_HALT` on stall but only
kicks the CPU on release. A cold-held CPU has never consumed that request;
0015 cancels it only when the combined hold clears, before releasing the
core. Read-only lifecycle properties expose the actual hold sources,
completed CONTROL_0 RESETING-edge CPU1 resets, CPU halted state and pending HALT request for
the regression harness; no guest IRQ register is substituted for host CPU
state.

`tests/qtest/esp32s3-cpu1-lifecycle.py` exercises real TCG/ROM execution in
addition to the existing clock suite's lifecycle cases. Same-image evidence,
source/delta hashes and the immutable executable receipt belong to the
0015 verification record, separately from the historical evidence above.
GPIO is an unchanged verification overlay, not part of the 0015 patch.
The separate 0016 SYSTEM-storage history is unused and unqualified here;
it is not renamed, folded into 0013, or included in this cutover.

The initial `0a642e4a` image qualified execution freeze, not physical clock
gating. Its shared CPU clock continued advancing CPU1's CCOUNT/CCOMPARE while
CLKGATE_EN was clear. This is an observed regression, not an acceptable
final lifecycle boundary: the final increment adds a distinct gated CPU1
clock and clock-derived counter/timer accounting.

### Initial same-image evidence (before physical counter gating)

Initial source epoch: `0a642e4a093882f8b3c06b5c340d90801082b8b0`.
Immutable QEMU: SHA256
`3e8a4aaa0fecbd006a025be860a62da69d879a9b9bd6856166bbd407b0e26266`,
ELF build ID `352661ea80b7f5da951bd225fea0fc3ba6cbc302`.
Receipts: `/home/polar/.cache/esp32s3vm/cpu-lifecycle-artifacts/0a642e4a093882f8/`
`IMAGE_READY.json` and `NATIVE_QUALIFICATION.json`; source/delta/patch hashes
are in `source-map.json`.

Delegated verification on that image passed the clock suite **13/13**, GPIO
suite **12/12**, real-ROM cold hold and first-release checks, gate/RUNSTALL/
RTC hold combinations, and running CPU1-only reset/idempotency. First CPU1
execution was the actual ROM PC `0x40000400`, before MESSAGE consumption.
An isolated, byte-exact restoration of the pending-HALT bug failed with CPU1
halted at the ROM vector and zero CPU1 execution blocks; this is a controlled
before variant, not a recovered copy of the lost complete WIP.

The original fixture selected CPU0 through a transient HMP monitor. Its
failure logs remain preserved and disqualified; the corrected helpers select
CPU1 in the same command. Only those two test files changed after compilation;
the QEMU compiled inputs, executable SHA and ELF build ID did not change.

Same-image GPIO SDK proof used a fresh copy of the post-acknowledgement 4 MB
firmware: output loopback high/low both passed, an actual GPIO ISR ran, the
task continued at ticks 10/20, and STATUS_W1TC acknowledgement left status
clear. The existing feeder's 420-second READY and 8-second ISR limits were
not increased. Ordinary boot/reset/profile and original 90-second hostbus
results are separately owned by the independent combined verifier.

The before variant's preserved delta is CRLF: its actual raw SHA256 is
`cac1ba3055a71b5206b8faeb2f9436c1020ec232f435fc59931d6af9ecece40d`;
the original receipt's `cbdb14f7…` digest is explicitly LF-normalized, not
raw. `BEFORE_DELTA_RAW_IDENTITY.json` records both identities without
rewriting the original before archive/receipt.

### Physical CPU1 clock gate

[ESP32-S3 TRM v1.8](https://documentation.espressif.com/esp32-s3_technical_reference_manual_en.pdf)
§17.3.6 (page 826) states that only CPU0 starts at power-up, CPU1's clock is
disabled, CLKGATE_EN controls CPU1's clock, and RUNSTALL independently stalls
its execution. Register 17.1 (page 829) gives RESETING/CLKGATE_EN/RUNSTALL
defaults `1/0/0`. The primary-source receipt is
`build-runtime-state/cpu1-lifecycle-diagnostic/primary-physical-clock-gate.json`.

The real locked-SDK counter fixture seeds CCOUNT with nonzero `0x13572468`
and programs CCOMPARE1 before CPU0 gates CPU1. On the preserved initial
`3e8a4aaa` image, CPU0 continued advancing actual time/counter/instructions,
but CPU1's compare IRQ asserted while gated and its resumed guest RSR CCOUNT
charged **150,047,934** ticks across the gated interval. No lazily cached HMP
counter was accepted as proof. A real, enabled FROM_CPU3 interrupt could
not execute while gated and was delivered/acknowledged after release.
The authoritative before receipt is
`/home/polar/.cache/esp32s3vm/cpu-lifecycle-counter-artifacts/before-a4a7049e3d38d90b/FIXTURE_READY_BEFORE_RESULT.json`.

The distinct `cpu1-clk` output follows the actual CLKGATE_EN gate; CPU0
continues on the nominal `cpu-clk`. A zero **period** represents the
documented QEMU unclocked state, not a zero counter or fallback frequency.
Xtensa PRE/UPDATE callbacks preserve actual CCOUNT and compare intents under
a per-CPU mutex; clock changes do not acknowledge timer IRQs. Enabled-clock
RUNSTALL continues counter/timer progress. Native CPU `clk-in-period`
readback reports the actual input period in \(2^{-32}\,\mathrm{ns}\) units.


### Final physical-clock native evidence

Source epoch `fe47d5051ae5fb704febc581a5252489ef7b4a68`; immutable QEMU SHA256
`6069303f2d076041c51b4a3fe29a044d2cb28068f898aea1caf02f9ee3c8fde4`,
ELF build ID `871b4aac575a29263457125ce59dc56cfceae199`.
`/home/polar/.cache/esp32s3vm/cpu-lifecycle-gate-artifacts/fe47d5051ae5fb70/`
contains `IMAGE_READY.json`, `NATIVE_QUALIFICATION.json` and
`PRODUCER_QUIET.json`; the consumer copied and hash-acknowledged the actual
new image before any proofs. All 10,373 archived source files matched.

The identical real SDK firmware/harness used for before/after passed
**25/25** counter/IRQ checks on the new image:

* CPU1's nonzero CCOUNT `324491847` was identical across all three gated
  samples. Its real input period was zero while CPU0's period remained
  `26843545600` and CPU0 advanced `144017830` cycles, `888587` µs and
  `372140` loop iterations.
* Compare `372479907`, resumed count `324556630`, and ISR count `372561241`
  demonstrate that the remaining timer cycles ran only after clock release.
  No compare IRQ latched while gated.
* Enabled FROM_CPU3 IRQ bit 2 stayed pending without CPU1 execution while
  gated, then ran its actual ISR and guest acknowledgement after release.
* With the clock enabled, RUNSTALL froze execution but CCOUNT and the
  compare timer continued. CPU1 RESETING reset only CPU1; CPU0, peripheral
  state, RTC marker and the shared handshake survived.

The same new image also passed clock **14/14** (original 13 preserved),
GPIO **12/12**, real-ROM cold/direct/edge lifecycle checks, and the ordinary
GPIO SDK loopback/ISR/W1TC/post-ISR checks with unchanged feeder limits.
The physical-package application/byte-correspondence proof is
`build-runtime-state/cpu1-lifecycle-diagnostic/physical-package-verification.json`.
Independent source and raw before/after evidence reviews found no P2 issue.
The independent new-image three boot profiles, SIMD84 and original
90-second fast/slow/GDB proof are separately recorded, never inherited from
the initial `3e8a4aaa` image.

### Independent final acceptance

The independent verifier qualified its **own immutable copy** of the new
`6069303f` image, not a substituted old binary. Receipt:
`build-runtime-state/cpu1-physical-gate-independent-runtime/20261007T134541Z/NEW_IMAGE_ACCEPTANCE_DONE.json`,
SHA256 `3587b6a61214d24c86a118e024316a13eac786d6413d88ac531402c4b5c3fae6`.

* Real SDK counter/compare/FROM_CPU3 IRQ criteria: **25/25**.
* Initial hold/reset readback `4`, stored RMW `6/6/6/2`, actual first CPU1
  ROM execution `0x40000400` after release and before MESSAGE, and **zero**
  early BOOT_ADDR polls (previous observed baseline: 96,664,642).
* Three actual boot profiles with control/reset, same-image SIMD **84/84**,
  and original **90-second** fast/slow/GDB barriers: all passed, including
  exact deadline/+1 ms and all 11 GDB packets per case.

Final increment: `0015-hw-xtensa-esp32s3-CPU1-reset-gate-runstall-lifecycle.patch`,
SHA256 `e66fb5ae5d994994ce6c49ce3e0f2fad68647a08ee33fb5f88007aa686758932`.
Actual source archive SHA256
`05ebd222d96c7d69e0257e40a13fa36f2f3fb89a0018c38fe11d68557446166d`;
full source delta SHA256
`1955cbbc843dfd6d1dd35d1d752eb9b5848fe35add84ad337ef2972fc3d607bb`.

Temporary queued-clock/clock-changing scaffolds were removed from the
permanent core and the verifier's temporary observer plugin was cleaned.
Functional read-only lifecycle and input-period interfaces remain for the
permanent regression tests; no diagnostic substitutes a fake counter.
All original firmware corpus and failure receipts were preserved. Frozen
0001..0014 are unchanged, and SYSTEM storage 0016 remains separate,
unused/unqualified history.

These are simulator lifecycle/counter semantics and assigned same-image
acceptance results, **not real-hardware timing qualification or a blanket
119-package qualification**. No hardware operation or boot-speed
performance claim is used to extend that boundary.

