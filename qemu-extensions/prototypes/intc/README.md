# intc — ESP32-S3 interrupt matrix OR/remap/disable semantics (H-CORE-02 / CORE-03)

Status: **prototype-unqualified**. Lane: intc (interrupt matrix).
Date: 2026-10-07. Owner: intc worker (IntMatrixWorker).

## What this is

Three patches against the **stacked base** = pristine official QEMU
`40edccac415693c5130f91c01d84176ae6008566` + the coreclk lane series
(`qemu-extensions/prototypes/coreclk/0001..0007`, stacked base commit
`761719510105d8b91b0b18b06fad43ea7dea71d8`). Apply with `git am` in order
after the coreclk series; verified to apply cleanly with resulting tree sha
`743796536753c5990d43e6cf469e5117cc0761ae`. No firmware hooks, no machine
(`hw/xtensa/esp32s3.c`) changes (GPIO/RTC_CORE/SPI1/PMS/cache sources stay
unwired — later integration task).

| Patch | Core paths | Behavior |
| --- | --- | --- |
| `0001-hw-xtensa-esp32s3_intc-model-source-OR-remap-disable.patch` | `include/hw/xtensa/esp32s3_intc.h`, `hw/xtensa/esp32s3_intc.c` | Per-source input-level tracking; each CPU external interrupt output = OR of asserted sources whose MAP entry selects it (TRM §9.3.3.2); routes re-derived on every input edge and MAP write on both CPU files. MAP values naming CPU internal interrupts (6, 7, 11, 15, 16, 29 — TRM Table 9.3-2) route nowhere (detach, §9.3.3.3; IDF `INT_MUX_DISABLED_INTNO` = 6); writes masked to the 5-bit MAP field. Per-CPU-file registers per TRM/IDF: `INTR_STATUS_0..3` (RO, 0x18C–0x198, bounded to the 99 documented sources — word 3 carries only the last 3), `CLOCK_GATE` (0x19C, bit0 CLK_EN, reset 1; gated file ignores writes and freezes outputs/status, latches current levels on re-enable), `DATE` (0x7FC, reset 0x02012300). MAP reset value 0x10 (IDF `interrupt_core{0,1}_reg.h` defaults); reserved slots 99–510 RAZ/WI **and inert** (asserted stray inputs neither corrupt the snapshot nor spuriously route to external interrupt 0). Real matrix→CPU interrupt lines exposed as named GPIO `cpu-int-out` (52 lines: CPU0 k=0..25, CPU1 26+k), still wired to the CPUs. Keeps the coreclk soc-reset gating: CPU-only resets preserve map+lines, PERIPH resets restore documented defaults. |
| `0002-tests-qtest-esp32s3-intmatrix-test-drive-matrix-OR-r.patch` | `tests/qtest/esp32s3-intmatrix-test.c`, `tests/qtest/meson.build` | QTest (`-M esp32s3`, qtest accel, no firmware): sources driven through the matrix GPIO inputs; CPU lines observed via `irq_intercept_out` on `cpu-int-out`. 9 cases: reset defaults, single-source routing + per-file INTR_STATUS, shared-source wired-OR (negative: clearing one of two keeps the line), remap of an asserted source, internal-int values + 0x1f write masking, dual-CPU files, status boundary, machine reset, clock gate. |
| `0003-tests-qtest-wire-intmatrix-test-and-align-coreclk-re.patch` | `tests/qtest/meson.build`, `tests/qtest/esp32s3-coreclk-test.c` | Stacked-integration fixes: runs BOTH ESP32-S3 qtest suites from `qtests_xtensa`; aligns the coreclk test's post-system-reset intmatrix expectation with the documented MAP reset value (0x10). No device change. |

Semantics ground truth: local TRM chapter 9 (§9.3.2, §9.3.3.1–9.3.3.3, 9.4/9.5)
and pinned IDF 6.1 headers
`build-idf-6.1/components/soc/esp32s3/register/soc/interrupt_core{0,1}_reg.h`
(5-bit MAP fields, reset default `5'd16`, `INTR_STATUS` RO reset 0, `CLK_EN`
bit0 reset 1, `INTERRUPT_DATE` reset `28'h2012300`) plus
`esp_hw_support/intr_alloc.c:1043` (`INT_MUX_DISABLED_INTNO` = 6). Register
file layout (512 MAP slots per CPU file, CPU0 +0x000 / CPU1 +0x800 at
0x600C2000) preserved.

## Native firmware evidence (`tests/firmware/interrupt_matrix/`)

Pinned IDF 6.1 (`fff9895c82d744c7237be8847347bdd1b07c6643`) fixture that
allocates two normally-shared interrupts via
`esp_intr_alloc_intrstatus(..., ESP_INTR_FLAG_SHARED, statusreg, own-bit, ...)`
and runs sequential pinned passes on CPU0 then CPU1:

* shared-allocation proof: both handles report ONE CPU interrupt number per
  core (`esp_intr_get_intno`: CPU0 → 8, CPU1 → 2);
* both ISRs run, each clearing only its own peripheral status bit and
  recording `esp_cpu_get_core_id()` (validated == allocation core);
* pattern "basic": both sources pending together, both ISRs serviced;
* pattern "cross": ISR A raises source B before clearing A — B is only
  serviced by a second dispatch driven by the wired-OR line level (the
  shared-chain walk cannot reach it: B registers earlier in the chain);
  this is a genuine firmware OR-persistence discriminator;
* bounded failure (timeout → FAIL marker → `esp_restart()`) and reset/reboot
  repeat (`RTC_NOINIT` boot counter, `INTMATRIX_ROUNDS=2`):
  `INTMATRIX OVERALL PASS boots=2`.

**Source-pair deviation (evidence-backed):** FROM_CPU2/3, as requested, are
the IDF 6.1 `esp_ipc_isr` channels — `esp_ipc_isr_port.c` routes
`FROM_CPU_2/3` to `ETS_IPC_ISR_INUM` at startup, and `esp_ipc_isr_handler.S`
clears exactly those registers. Routing fixture ISRs to them desynchronized
the cross-core IPC protocol: Core 1 `panic'ed (InstrFetchProhibited)`,
PC=0 inside `esp_ipc_isr_handler` (captured:
`build-runtime-state/intc-2026-10-07/firmware-fixture-fromcpu-crash.log`).
FROM_CPU0/1 are equally taken (crosscore yield handler clears the register
first in the chain). The fixture therefore uses TIMG0 timer0 / TIMG1 timer0
(sources 50/53) — software-schedulable, unclaimed by the IDF 6.1 default
config, per-source status bit and per-source clear.

## Deliberate model decisions

* MAP reset value is the IDF-documented **0x10**, not the old model-wide 6;
  0x10 is itself an internal CPU interrupt, so nothing is routed after reset
  until software programs the matrix (CPU INTENABLE=0 after reset in any
  case). The coreclk test expectation was aligned accordingly (patch 0003).
* While `CLOCK_GATE.CLK_EN=0`, MAP writes are ignored and outputs/INTR_STATUS
  hold; input levels are wire state and propagate on re-enable (level-correct
  for a level-signaled matrix). IDF never gates this clock.
  **Evidence classification**: the register itself is TRM/IDF-evidenced
  (TRM 9.4 Register 9.89/9.179 `CLOCK_GATE_REG` at 0x019C/0x099C, "used to
  control clock-gating of interrupt matrix", R/W reset 1; IDF header bit0
  `CLK_EN` default 1). The freeze-and-latch *behavior* is register-clock
  domain modeling (an APB-gated register file ignores writes and holds state,
  latching current inputs on re-enable) — **inferred**, not silicon-observed:
  this host-only lane has no silicon reference for gated-domain interrupt
  timing, and no hardware capture of CLK_EN=0 exists in the repo. The QTest
  (`clock-gate-freeze`) pins the modeled contract only.
* Reserved MAP slots (99–510) and non-32-bit-aligned accesses RAZ/WI; inputs
  ≥ 99 have no status bit and never route (0 is a valid external int number,
  so unbounded routing would be a real bug).
* CPU interrupts 15/16 (Timer1/2), 6/7 (Timer0/Software), 11 (Profiling),
  29 (Software) are not external on the LX7 config
  (`XCHAL_NUM_EXTINTERRUPTS` = 26), which is why the TRM lists them as valid
  "disable" codes.
* NMI (CPU int 14) is routable like any external interrupt; NMI masking via
  the World Controller is out of scope (no WCL model).

## Verification (evidence: `build-runtime-state/intc-2026-10-07/`)

All on the **stacked binary** (pristine + coreclk 0001..0007 + intc series):

* QTest 9/9 intmatrix + 9/9 coreclk (`qtest-tap.log`, `qtest-tap-coreclk.log`,
  per-test protocol captures `qtest-log-*.txt`). Protocol logs show named
  offsets (e.g. `writel 0x600c206c 0x4` =
  `INTERRUPT_CORE0_UART_INTR_MAP_REG ← 4`) and real transitions
  (`IRQ raise 4` / `IRQ raise 42` = CPU1 int 21; shared-OR case lowers only
  after both sources clear).
* Native fixture: `INTMATRIX OVERALL PASS boots=2` on the stacked binary —
  both cores, shared intno proof, per-source clearing, basic + cross patterns
  (`firmware-fixture-run.log`).
* BootSmokeTest 3/3 PASS (`boot-smoke.log`) — IDF's real
  `esp_intr_alloc`/`intr_matrix_set` routing path works unchanged against the
  reworked matrix (regression gate).
* Boundary (documented): OR/remap/clock-gate semantics are proven QTest-only
  at MMIO level plus the native fixture's cross pattern; no dedicated IDF
  fixture on FROM_CPU sources is possible (see deviation above).

Exact commands and hashes are in `build-runtime-state/intc-2026-10-07/evidence.md`.
