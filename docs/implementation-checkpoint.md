# Implementation continuation checkpoint — 2026-10-07

Goal remains **active and incomplete**. The user explicitly requested takeover and
parallel implementation in this session. Preserve the reviewed full scope; do not
reduce completion to the foundation slices below. Historical heartbeat metadata
is retained in the frozen packet; it is not an inventory of this session's jobs.

## 2026-10-10 — real ROM UART0 memory download and208 native cases PASS

Fresh immutable UART candidate `7b8dad7015d9332a`, executable SHA256
`9bdf0b64804488a9c4cd457732310d1e20cf211c405f187bd4ce2e0f18a5dbd1`,
matches all **147 applied source hashes**. All nine previously qualified native
suites now total **208 PASS, zero skipped**; UART grows from64 to67 with three
actual powered-net clear/rearm regressions.

Native debugger capture proved the old ROM crash path: port4/FIFO22 at
clear-all40048e9f immediately regenerated RXFULL; enable40048eae raised CPU5
before the port0 store40048eb0. Receive progress or a changed committed threshold
now rearms RXFULL, not unchanged FIFO occupancy during mask/TX-only updates.
This event-rearm convention is explicitly **[INFERENCE]**, not independently
measured silicon clear/core-clock propagation. No invalid aperture alias,
guessed delay, ROM-specific branch or synthetic ACK was added.

Fresh real ROM traversal **PASS**: four normal SYNC requests, two complete
eight-reply groups, MEM_BEGIN,6144+1688 payload bytes and checked MEM_END(1,0).
Every actual four-byte ROM status succeeds, flash stays unchanged, the external
graph is empty and payload execution is disabled. Raw bytes are retained.
The validator permits only complete eight-reply groups bounded by actual
ordinary acquisition requests; it never removes retry replies. Four host-only
protocol boundary tests PASS separately. Uniform180s command/900s overall host
waits do not modify guest clocks or ordinary protocol packets/retries.

Exact proof: `build-runtime-state/uart-continuation/rom-stage/qualification-receipt.json`.
The original panic and fixed-single-total-burst validator failures remain
immutable. All **six same-binary ordinary UART/Arduino reruns PASS**, including
the full connected1517s run under the explicit3600s host diagnostic ceiling.
The900s expiration remains FAIL, with guest timing/assertions unchanged.
Publication is recorded separately in the stage's `publication-receipt.json`.
Previous Arduino stage **`a2a08e7`** is pushed.

Parallel I2S and pulse stages remain active. The scoped GDMA0008 partial
continuous RX completion correction now passes the entire I2S79 native suite,
including cleared full-descriptor IN_DONE non-reassertion; full ordinary148/
external68 gates remain active. RAW2626 and protected caches are unchanged.
The previously unexecuted LCD/camera66 suite now passes66/66 in an isolated
fixture candidate: named IRQ group, actual S3 GPIOs, complete registered terminal
layout/directions, real virtual-clock resume, wired input enables, valid wrong
topology, actual filter edge origins and explicit fresh IRQ initialization.
No LCD/CAM model or canonical source changed; ordinary25 remains unqualified.
Separate proof: `build-runtime-state/uart-continuation/lcd-stage/qualification-receipt.json`.
LCD/camera read-only scouts failed on provider quota/billing before any work;
the integration owner completed the reachable native investigation directly.
Full simulator acceptance remains active/incomplete; exact PCM/PDM converter
arithmetic, broader peripheral/reference gates and independent metrology are
not inferred. No physical-board operation.

## 2026-10-09 — ordinary pinned Arduino HardwareSerial and five IDF regressions PASS

The Arduino wrapper now completes ordinary `setup`/`loop`, all five exact
513-byte crossed HardwareSerial vectors and all three physical UART0 loopbacks.
Its per-vector `digitalRead` evidence requires both peer TX nets at real HIGH
idle before either receiver is installed. Pinned Arduino3.3.12/IDF5.5.5 native
source/build/image/ELF/configuration/driver identities are frozen.

The root fixture defect was teardown ordering, not startup panic: Arduino
detaches its directly owned TX pads to ordinary GPIO but does not disable their
LOW latches, while IDF's separate pin bookkeeping cannot release those pads.
The fixture uses ordinary `pinMode(INPUT)` after both peers end; existing real
10kΩ pull-ups establish idle. No RX flush, leading-zero trim, payload injection,
SDK/model patch or receiver-error suppression was added.

All **six ordinary UART scenarios PASS** on current backend984f71fe6aadd031,
SHA256 `03bb3bbcc2eb2b69ac4d2f376d7ab85f321b9a712c7da745c6f7ff703f9d00b6`.
The five unchanged IDF6.1 images were strictly re-frozen from their actual
compiled roots after the shared validator changed, then fully exercised.
The current simulator model is unchanged; its205 native qualification remains
the separate RMT publication proof. Arduino evidence does not imply IDF6.1 Arduino.

Exact proof: `build-runtime-state/uart-continuation/arduino-idle-stage/qualification-receipt.json`.
The RMT stage was pushed as **`3733ed7`**. ROM download remains separately
unqualified: actual checksum/status ACK traversal and selected-port/interrupt
debug evidence are still required. The full simulator goal is active and
incomplete. No physical-board operation.

## 2026-10-09 — current-foundation RMT TX/RX and real peers;205 native plus both ordinary profiles PASS

Current RMT candidate: `qemu-uart-40edccac4156-984f71fe6aadd031`,
executable SHA256
`03bb3bbcc2eb2b69ac4d2f376d7ab85f321b9a712c7da745c6f7ff703f9d00b6`.
All **147 applied source hashes** match the immutable receipt.

* **205 native cases PASS, zero skipped**: RMT23 plus the complete182-case
  SPI/NOR/UART/UHCI/GDMA/memory/I2C foundation suite.
* **Both complete ordinary pinned-IDF RMT profiles PASS** on the same backend:
  connected and disconnected. Actual phase clocks/pulses, exact closed RX
  symbols, filters, stop/recovery, carrier, synchronization, long PIO/GDMA,
  real powered WS2812 DIN decode and external NEC OD source/capture are asserted.
* New canonical integration uses existing clock/gate/reset/IRQ/GDMA/NativeNet
  owners once. It adds RMT to global peripheral cold reset and removes the last
  unimplemented-device mapping/helper/import. SPI0/1 memory paths remain intact.
* Ordinary stop timing initially failed because UART status reporting extended
  the active400us loop to about26ms and delayed the stop timestamp. The fixture
  now captures API results/timestamp before reporting, without relaxing either
  exact-prefix or stop-causes-idle assertions. Before/after evidence is retained.
* Phase-ready/DONE barriers require complete real UART lines. The bounded host
  watchdog permits3600s; the earlier600s expiration remains FAIL, not simulated
  guest timeout or a passing fallback. SDK source/build provenance is frozen on
  native Linux and the new exact source closure is tracked in RMT
  `foundation-dependencies.json`.

Exact evidence: `build-runtime-state/uart-continuation/rmt-stage/qualification-receipt.json`.
This is functional modeled timing, **not physical pulse metrology**. RX carrier
demodulation's exact envelope algorithm, broader RC_FAST/source/memory/fast-edge/
replay and independent silicon gates remain unqualified. The previous SPI stage
was pushed as **`522a589`** after182 native and25 ordinary outcomes passed;
those older ordinary results are not silently attributed to this executable.
The full simulator goal remains **active and incomplete**. No hardware operation.

## 2026-10-09 — source-bound SPI2/3 master PIO/GDMA/NOR;182 native and25 outcomes PASS

Current candidate: `qemu-uart-40edccac4156-eb6707129054f40a`, executable SHA256
`cabeb9c4eed50ab27f8896a2d3a0f9fe5860920450679ea1b3ffe8bead6bc967`.
All **136 applied source hashes** match its immutable preparation receipt.

* **182 native cases PASS, zero skipped**: SPI27, NOR8, UART64, UHCI17,
  GDMA18, memory12, I2C controller18 and I2C service18.
* **20 ordinary completed runs PASS on that same executable**: the previous
  thirteen I2C master/slave and five UART/UHCI regressions, plus both complete
  SPI connected-NOR and external-loopback fixtures. **Five additional strict
  SPI negatives PASS** as expected paused hardware states, not completed transfers.
* Both SPI controllers return actual8,201-byte NOR/loopback payloads with
  descriptor ownership, EOF, callback and chip-select isolation checks.
  Loopback includes all four clock modes, mixed bit order, non-byte phases,
  queue identity, short RX bounds and pre-first-byte reset/no-EOF behavior.
* Root-fixed missing released FSPIQ_OUT authority at ordinary MISO setup and
  shared GDMA reset coupling. IN/OUT peripheral resets now preserve their
  opposite active chain; memory-to-memory cancellation stays coupled.
  Retained actual-transfer regressions fail before/pass after.
* Fixture ARM records use the real nondeprecated console-idle API before
  strict pauses; the runner waits for complete DONE lines. Two raw reset probes
  share legal clock-divider fields rather than overflowing the hardware fields.
  All seven final source-identity`75b29a1690026621` variants were compiled,
  merged, pinned and actually exercised. Failing/cancelled/superseded images are
  preserved, not reused as PASS.
* The canonical source closure is tracked in SPI `foundation-dependencies.json`;
  the obsolete standalone prefix-copy preparer is removed. Three current
  aggregate recipes include canonical SPI sources plus direction-isolated GDMA.

Exact evidence: `build-runtime-state/uart-continuation/spi-stage/qualification-receipt.json`.
The full simulator goal remains **active and incomplete**: advanced/slave/
direct-IO_MUX/PSRAM-specific SPI, other peripheral/native/metrology gates,
Arduino/ROM and radio/SIMD independent proof are not implied. Parallel workers
staged I2S raw-data/mono-right and LEDC/MCPWM refinements without changing this
candidate; exact PCM/PDM converter coefficients/state/rounding/golden vectors
remain unavailable after primary-source review. Arduino idle/ROM timing findings
are staged and unqualified. No physical board operation occurred.

## 2026-10-09 — both ordinary I2C slave drivers;147 native and18 ordinary PASS

Current source-bound candidate:
`qemu-uart-40edccac4156-1e937cf0ad1822bb`, executable SHA256
`6e2c78411f0ef48215ad1c8be3f23fd0eef0ccfe4ea151c891a357ef8c49347f`.
All **127 applied source hashes** match its immutable preparation receipt.
The simulator's full goal remains **active and incomplete**.

* **147 native cases PASS, zero skipped**: UART64, UHCI17, GDMA18,
  memory12, I2C controller/error18 and service18.
* **18 ordinary scenarios PASS on that same binary**: the ten master and five
  UART/UHCI regressions, plus slave-connected100/400kHz and disconnected100kHz.
  Both public slave drivers receive65 bytes, respond with65 bytes through
  actual physical relay clocks, and prove that TX reset discards49 queued bytes
  before17 replacement bytes. Callback/request boundaries and silence on
  disconnected wires are asserted from actual driver output.
* Native debugger evidence localized the initial controller1 reset-row failure
  to an empty TX FIFO and address-stretch expiry while earlier UART reports
  blocked response service. The fixture now services pending requests before
  reporting; driver status/bytes/callback checks are not relaxed.
* Fixed a separate model watchdog defect: unrelated GPIO frames previously
  postponed a continuous-stretch deadline. The retained native regression
  toggles real unrelated GPIO output and fails before / passes after.
* Peer payloads are bounded at256 bytes for genuine FIFO32/software-ring
  crossings. Host scheduling uses public GPIO12 and actual QMP circuit Apply,
  never register/callback/RX injection. Native cache sources are not edited.

Stage1 was committed and pushed as **`fd43572`** on
`origin/codex/simulator-foundation`; subsequent verified refinements use separate
stage commits. Exact new-stage inputs/reports/snapshots:
`build-runtime-state/uart-continuation/i2c-slave-final/qualification-receipt.json`.
Broader ten-bit/read/restart/full multi-master/filter/RC/replay and other
canonical I2C gates remain open, alongside other peripherals, Arduino/ROM,
radio/SIMD and independent reference proof. SPI/RMT workers failed before edits
because the configured provider reported insufficient funds; no alternative
model was substituted. No physical-board operation occurred.

## 2026-10-09 — canonical I2C cutover;147 native and15 ordinary PASS

Current source-bound candidate is
`qemu-uart-40edccac4156-1411f3a2d8676631`, executable SHA256
`880b9dbe735f9c6258245b53f4b43929fd7abf5f2bc68410a29b025c2172c75a`.
All **127 applied source-file hashes** match its immutable preparation receipt.
The full simulator goal remains **active and incomplete**.

* **147 native cases PASS, zero skipped**: UART64, UHCI17, GDMA18,
  memory12, I2C controller/edge18 and I2C service18.
* **All15 strict ordinary firmware scenarios PASS on this same executable**:
  ten I2C runs on both controllers (connected100/400kHz and eight physical
  routing/pull/stuck-line/power negatives), plus connected/absent/disconnected/
  wrong/UHCI UART runs. The pinned SDK probe path uses100kHz regardless of
  the device-transfer frequency printed by the fixture.
* Active aggregate profiles now copy every canonical I2C controller, binding,
  service and test source rather than the preserved memory-frozen master set.
  Eight previously unregistered edge/slave/address-arbitration cases are active;
  a new typed-config rejection case preserves real queued data across bad Apply.
* Fixed zero-valued route-OK mistaken for false, actual SCL/SDA/STOP ordering,
  slave address-ACK/data separation and final-NACK STOP completion, first-script
  glitch dispatch, live-clock address arbitration and loser withdrawal without
  STOP. START/END ownership and repeated START remain state-machine operations.
  The malformed config regression proves old-executable crash / new rejection.
* Strict UART re-freeze uses the actual original compiled-source/build roots;
  their image/ELF/config hashes match the previously qualified snapshots.
  The unconfigured UART test invocation's single skip is retained separately;
  the required real-net driver then executes all64 cases.

Exact receipts, frozen manifests, raw reports and graph/QMP snapshots:
`build-runtime-state/uart-continuation/i2c-final/qualification-receipt.json`.
Controller0 slave/filter/address-arbitration vectors are not complete I2C:
SDK slave qualification on both controllers, broader ten-bit/master-data/full
multi-master/filter/RC/replay and other canonical gates remain explicit in the
I2C source map. Arduino/ROM, other peripherals, radio/SIMD and independent
reference gates remain open. Verified refinement stages are committed and pushed
individually; this147/15 checkpoint is the first publication boundary. Git keeps
hash-pinned patch artifacts byte-exact rather than normalizing their line endings.
No additional physical hardware operation occurred.

## 2026-10-09 — complete ordinary UART/UHCI software runs; initial I2C proof

This entry supersedes the later-timeout and UHCI-failure diagnoses in the
previous continuation section. The full simulator goal remains incomplete.

Fresh candidate `qemu-uart-40edccac4156-c35deea6f7043884`:
executable SHA256
`622935f7340d79a383dd5f72d0fec3f9489bf4e4ddddcc8cfbae7e26ef634643`;
all **127 applied source files** verified against its immutable preparation
receipt.

* **All five strict ordinary connected/absent/disconnected/wrong/UHCI
  firmware runners PASS** on this exact executable. Connected covers the
  complete baud/format, threshold/timeout, recovery, flow-control, host
  stop/resume, error, AT/autobaud, digital RS485 and receive-only IrDA suite.
  UHCI covers actual513-byte idle/multibuffer/length/break DMA packets,
  ownership/EOF/TX completion and concurrent physical UART0 activity.
* A captured active UART0 shifter with127 queued bytes, plus194208 DC
  factorizations at only2.9seconds guest virtual time, exposed the shorter
  host reporting cutoff. The runner now defaults to the existing900-second
  diagnostic limit; guest deadlines and failure checks are unchanged.
* The one remaining connected framing-IRQ failure was a fixture contract:
  pinned IDF's default mask omits frame errors. The fixture explicitly enables
  that interrupt through the public API before the mismatched-width stimulus.
* Central GDMA0006 corrects premature `IN_DONE`: the target defines a completed
  inlink descriptor, not one peripheral pump call. Early per-byte interrupts
  made UHCI recycle incomplete buffers and over-count callbacks. Completion
  now follows successful nonempty descriptor writeback; true packet/segment
  EOF and post-store fault accepted-byte counts retain their original contract.
* **64/64 UART,17/17 UHCI,18/18 GDMA,12/12 memory,9/9 I2C native cases PASS**,
  zero skipped. The native packet test checks no interrupt/ownership release
  after early accepted bytes. Active aggregate profiles include GDMA0006 and
  use the corrected canonical I2C test, not the preserved memory-frozen copy.
* Initial I2C timed7-bit master proof: **9/9 native controller cases,
 18/18 service cases**, and **ordinary pinned-IDF connected100kHz on both
  controllers PASS**. Its old paused harness never enabled timers; setup now
  continues after quiescent Apply, and later graph changes preserve prior
  run state. Advanced I2C/slave/10-bit/multimaster/edge gates remain open.

Current records:
`build-runtime-state/uart-continuation/dma-final/qualification-receipt.json`,
`uart-continuation/i2c/qualification-receipt.json`, and the UART/I2C/GDMA
source maps/readmes. Arduino/ROM/reference and other peripheral/radio/SIMD
qualification remain separate. Changes from this continuation have not yet
been committed or pushed. No additional physical hardware operation occurred.

## Continuation — console drain and native UART IO_MUX qualification

This section supersedes the earlier console-pollution and missing-Arduino-marker
diagnoses. Full VM implementation remains active and incomplete.

* A failed worker reverted the old cached `esp32s3_clk.c` to pristine. That
  checkout was preserved, not rebuilt or silently trusted. Fresh candidate
  `qemu-uart-40edccac4156-9feac24d8fa6b676` was reconstructed from hash-verified
  prerequisites and tracked lane inputs. All **127 applied source files**
  match its preparation receipt. Executable SHA256:
  `df894a160aa7a0689545ad810ce72d93d01fe55f45a19aa20c6e58e101ba6440`.
* Preserved trace proves `uart_param_config` reset discarded **127 queued
  console report bytes**. The fixture now drains UART0's hardware FIFO before
  installation and again after installation's queue log, before reconfiguration.
  **Strict absent/disconnected/wrong runners PASS**; no parser relaxation or
  binary-payload filtering was added.
* The electrical solver previously ignored direct IO_MUX outputs. Ordinary
  UART1 used GPIO17/function2, so its waveform never reached UART2 RX16. Native
  U0/U1 TX/RTS routes now reuse the controller's actual drive sources. A new
  waveform/byte regression verifies direct mux selection, release and restore
  with GPIO output-enable and matrix routing disabled.
* `TX_DONE` was incorrectly regenerated as an idle level after software clear.
  It now latches a busy-to-final-idle completion and remains clear until the
  next transfer completes; FIFO/reset cancellation does not fabricate a DONE.
  Per-controller clear/re-arm regressions cover the IDF RS485 invariant.
* **64/64 UART qtests + 17/17 UHCI vectors, zero skipped.** Eight initial
  ordinary crossed baud/format transfers, including `normal_restore`, PASS
  with 513 bytes received in each direction. Full connected and UHCI runs still
  time out later; advanced cases are not promoted from these initial passes.
* Arduino's actual SDK description is `b774170f`, emitted by the firmware.
  The validator now accepts that pinned git description alongside `5.5.5`,
  retaining full source/SDK commit checks. HardwareSerial report boundaries
  are repaired; remaining failure is behavioral completion, not identity.
* ROM relay waits for the complete CRLF-terminated banner and uses standard
  esptool no-reset SYNC acquisition without chip-detection register/security
  commands. No valid ACK traversal has been observed. A separate direct probe's
  second-SYNC bytes were **LoadStoreError panic text, not a response**
  (`EPC1=0x40044290`, `EXCVADDR=0x5ffd0000`).
* Two parallel workers were requested; both failed before work because the
  configured provider rejected requests. No model substitution occurred;
  the changes above were implemented inline.

Exact records: `qemu-extensions/prototypes/uart/source-map.json`,
`changelog.json`, and
`build-runtime-state/uart-continuation/final/qualification-receipt.json`.
Historical cached executables and earlier qualification records are not the
current executable's evidence. No additional hardware operation was performed.

## 2026-10-08 — takeover integration: UART finalized, hostbus enabled launch resolved

Takeover coordinator session (branch `codex/simulator-foundation`, commits
`c0c2281` docs/lock, `da8be21` GUI/tools/tests, `3c948e8` qemu-extensions lanes,
plus follow-ups, all pushed to origin):

* **UART lane loopback row fixed (same day, later):** `uart0_physical_loopback513`
  now reports **PASS** in the ordinary fixture (absent mode firmware reports
  `failures=0 result=PASS`), root cause proven by trace instrumentation: the RX
  edge detector's stale level baseline (carried from the console pad across the
  GPIO44→GPIO5 re-route) swallowed the first start bit; frames then assembled
  misaligned from a later data edge and `err_wr_mask` correctly discarded them.
  Fix: an RX input routing signature re-baselines edge detection on re-route,
  and the first valid low sample on a (re)routed input arms the frame.
  Vectors re-verified 60/60 + 17/17. Both fixture runners now use the
  deterministic profile (`tcg,thread=single; icount shift=0,align=off,sleep=off`)
  — under wall-clock TCG the RX sampler fast-forwards under the console-port
  MMIO storm — and the runner completion race is fixed (partial-line token match
  killed QEMU mid-print; uhci mode watched the wrong DONE token). Residuals
  (documented in `uart/source-map.json` blocked_by): console evidence-stream
  pollution (the loopback payload is physically on pad43 and interleaves the
  print stream, breaking the runner's vector-name scan while firmware reports
  zero failures), 4 uhci idle/EOF rows, the connected-mode host-stop dance under
  icount, the ROM-download SLIP banner interleave, and the Arduino boot-identity
  gate. Also earlier: link-bind double-unref fix kept; console-TX retry
  prototype reverted after proving teardown heap corruption.
* **Hostbus blocker resolved:** `qom-options.patch` applied through the guarded
  apply flow, extension rebuilt, and the previously failing enabled launch now
  passes: QAPI accepts `-object esp32s3-hostbus-probe` (`/objects` lists probe,
  `watchdog-ms`="5000", peer connects, prelaunch under `-S`, clean rc=0), and
  the **delay-fast scenario passes with the manifest-identical peer** (sha256
  `938e5368…`): 1 ms modeled latency honored, both CPUs enumerated, 4 captured
  frames. Evidence: `build-runtime-state/hostbus-qom-enabled-2026-10-08/` and
  `hostbus-enabled-scenarios-2026-10-08/delay-fast/`. Remaining contract
  findings (other scenarios, malformed lifecycle, two-core execution evidence,
  disabled baseline re-run) stay open; the prototype remains unqualified.
* **Memory lane:** `qualification.json` repointed to candidate `aa8b5f8e…`
  (evidence `memory-core04-aa8b5f8e-2026-10-08/`) after the IOMMU UNMAP event
  address fix; the earlier `6dfa7350` section below is superseded by it.
* **Radio research:** `docs/research/sar2-pwdet-calibration-register.md`
  identifies the Wi-Fi calibration poll target `0x6000e050[26:24]` as the
  PWDET/SAR2 measurement sequencer inside the undocumented I2C_MST (REGI2C)
  analog block, with the documented C6-family engine grammar and the SENS
  clock/reset/ownership prerequisite chain. H-RADIO modeling must implement the
  FSM (never a hardwired done=7); see the note for citations and [UNVERIFIED]
  items.


## 2026-10-08 — external PSRAM (CORE-04) requalified

Candidate `runtime 6dfa7350906414c4`, executable sha256
`2917db0b20c6ad513b76e274ea0e955853c411cd07ec2e9c5f82f977016f69b2`, from
`qemu-extensions/runtime-profile-native-foundations-memory.json`:

* **12/12** native memory qtests and **16/16** frozen GDMA cases on that exact binary.
* **4/4** ordinary SDK leaves (IDF 6.1 and 5.5.5, psram and internal), three boots
  each (initial, QMP `system_reset`, same-disk relaunch) with disk phase advance
  asserted.
* **3/3** frozen boot/control profiles; SPI1 control fails before / passes after
  the transport index-bounds fix (`spi1-high-duplex`, `spi1-asymmetric`).

Defects corrected (each proven by trace or watchpoint, not inference): cache MMU
translations for PSRAM targeted an address space rooted in an uninitialised region
(ordinary firmware faulted on its first PSRAM access while same-driver qtests
passed) — translations now resolve inside system memory through a private alias
whose base must stay inside the 32-bit CPU physical space; the native memory qtest
wrote `SPI_MEM_ADDR` with a stale `<<8` shift; the fixture's unmapped-alias probe now
verifies the MMU entry itself and never uses IDF's transient boot-partition page.
The host runner keeps live session artifacts on native disk and publishes them after
each session closes (WSL drvfs concurrent append/read faults otherwise abort runs).

Evidence: `build-runtime-state/memory-core04-6dfa7350-2026-10-08/`. Records:
`qemu-extensions/prototypes/memory/qualification.json` (status
`CORE04_NATIVE_QUALIFICATION_PASS_ON_6dfa7350`), `source-map-native-foundations.json`,
`tests/firmware/memory_native/qualification.json`, `docs/current-status.md`,
`docs/handoff/01-current-state.md`. The retired memory-only map
(`memory/source-map.json`) is marked superseded so the preparer refuses stale inputs.

Open in this workstream: other densities, OPI/DDR/DQS, CPU access to a proved-unmapped
alias, independent PSRAM instruction-cache/XIP, preload/autoload/tag assists,
cache-error IRQ and silicon timing, and the camera-capable consumer tree (blocked on
the I2C-lane restack; candidates re-prepared after this fix).

Next slice: **UART0/1/2** native qualification (timing, framing, FIFO, flow control,
DMA through native nets) per the reviewed plan.

## 2026-10-08 — UART lane prepared, built, first vectors executed

The lane (`qemu-extensions/prototypes/uart/`, packages UART-01/02/03) had never been
prepared, built or run. It now prepares from the frozen foundation, builds, and runs
its native vectors. Candidate checkout
`~/.cache/esp32s3vm/qemu-uart-40edccac4156-d7f4bed46d77c73a`, dependency record
`build-runtime-state/runtime-8ba824296e44c0fd-source.json` (native foundations +
CORE-04 memory overlay + shared GDMA/SPI inputs), evidence root
`build-runtime-state/uart-native-2026-10-07/`.

Defects fixed on the way (each verified by rebuild/run, logged in the lane changelog):

* `prepare.py` silently re-tightened the shared RAW2626 GDMA/API patch to a strict
  apply when the dependency record supplied it; the lane's declared
  `ignore-space-change` matching operation now governs that artifact.
* `integration.patch`, `integration-electrical.patch`, `integration-qtest.patch`
  were CRLF with stale/synthetic hunk context; re-authored as LF with the actual
  frozen-foundation context, added/removed content byte-identical modulo line
  endings, strict-applying.
* UART device `trace` is now a class property: a device property is rejected after
  realize, which the qtest (and any `qom-set`) needs.
* `fifo-memory` link is set from machine init, after the SoC is attached: setting it
  during the SoC's own `instance_init` resolves an empty canonical path and silently
  stores NULL.
* `esp32s3_uart.c` trace barrier uses the pinned log API (`qemu_log_trylock`/
  `qemu_log_unlock`); qtest array `signal[]` renamed to `matrix_signal[]` (libc clash).
* Unit-test teardown unparents owner-less memory regions (they are QOM-attached
  under `/machine/unattached`).

Verified: `test-esp32s3-uhci` **17/17 PASS**. `esp32s3-uart-test` now executes with a
working virtual clock: **14/60 vectors pass**, the suite stops at
`uart0/rs485-physical-errors-clash` (peer stop-bit error sets `FRAME_ERR` but not the
RS485 mirror `RS_FRAME` because the model's `tx_active` is already 0 at that sample;
the parity mirror one bit earlier passes).

The blocker was the harness, not the frame engine: the vector suite launched QEMU with
`-S`, and QEMU only enables `QEMU_CLOCK_VIRTUAL` in `vm_start` (`system/cpus.c`
`qemu_clock_enable(..., true)`), so `qtest clock_step` was a permanent no-op for every
timing vector. Setup now starts paused, applies the electrical project at the
quiescent boundary (the device rejects it while the VM runs), then issues `cont`.
An interim TX tick→bit remap was reverted (the authored convention pre-shifts
`tx_frame` by one; the earlier stop-bit symptom came from the frozen clock).
Composed foundation for this lane: `runtime-profile-native-foundations-memory-lcdcam-timebase.json`
(= the integrated profile plus the upstream timebase fix `prototypes/timebase/0002`).

Outstanding for this slice: the remaining 46 vector rows (next: RS485 TX-mode mirror
timing), then the six ordinary IDF/Arduino fixture images and the ROM-download
regression under the same paused→configure→cont contract.

### Update — native vector sets green, ordinary fixtures executing

* Harness: `-S` kept (the electrical device needs a quiescent boundary) but the VM is
  continued right after the project is applied, which is what enables
  `QEMU_CLOCK_VIRTUAL` and therefore `qtest clock_step`.
* `esp32s3-uart-test`: **60/60 PASS**, `test-esp32s3-uhci`: **17/17 PASS**, both on
  clean checkout `qemu-uart-40edccac4156-5b793f4831194b26` (executable sha256
  `b35f293a…`); tree verified free of diagnostics. RS485 rows were settled from
  TRM v1.8 register 26.17 (bit3 `RS485TX_RX_EN`, bit4 `RS485RXBY_TX_EN`, bit2
  `DL1_EN`): the model matched the TRM, the vector had omitted bit4.
* Ordinary fixtures (re-frozen as `build-runtime-state/uart-native-2026-10-07/frozen-r2`):
  five IDF 6.1 modes execute end-to-end; each fails only
  `uart0_physical_loopback513` (UART0 57600 8E2, GPIO4→GPIO5, 513 bytes each way).
  `connected` additionally fails `normal_restore` and needs a watchdog longer than
  120 s. Arduino build is blocked by the pinned vendor component's missing
  `esp_netif` REQUIRES; ROM-download reaches the real ROM banner and download mode
  but SYNC parsing fails on banner bytes interleaving the first SLIP frame.

## Current takeover session — supersedes older operational state

Read [the current continuation log](handoff/09-takeover-2026-10-07.md) first.
This session owns integration; foundation workers use separate native WSL
checkouts and publish tracked patches. No pristine official checkout is modified.

Verified this session:

* Required GUI regression **9/9, zero skips, 47.49 s** after inspector/migration/
  polling corrections; real Light/Dark previews inspected.
* IDF 5.5.5 and Arduino 3.3.12 boot/control fixtures each **3/3** on official QEMU.
  Esptool 4/5 command spelling is reconciled. Provenance uses Git's parser,
  tolerates repeated vendor metadata, rejects duplicate identity paths and emits
  portable relative paths: **5/5 local-repository tests** and real 5.5.5 manifest.
* Enabled hostbus probe's current harnesses pass, including GDB/reset/failure/
  restore denial and actual two-core activity. Coordinator smoke observed
  `armed-ns == stopped-ns == 1093955487`, unchanged across 40 QMP reads, and
  completion exactly one millisecond later after explicit release/continue.
* Physical all-image deployment was approved and completed. The initial stale
  backup preflight refused before mutation; user approved a current-flash
  re-baseline and explicitly waived restoration. Both 16 MiB images survive.
  Default recovery and waiver failure/cancellation behavior have **17 passing
  mocked tests**. Board was left with the BLE fixture.
* Hardware arbitration exposed 38/84 PIE vector mismatches. Three minimal
  translator patches now match the same-image hardware records **84/84**;
  baseline/fixed comparisons and boot smoke are retained.
* Standalone graph/DC adapter passes **4/4 CTest**, including sanitizers.
  Native pad/ADC integration remains unfinished.
* Current timer-integrated runtime, fingerprint `dbe2a8a5ad4e07a6`, binary
  `72ea3caf8f01c4cf5e27db8c2cb4faca0c7153ba7e3b48287b3a4ae3359473a8`,
  passed the complete subagent lane at 06:04 UTC: three boot/control profiles,
  SIMD 84/84, barriers/GDB, clock 10/10, matrix 9/9, GDMA 16/16 and native
  dual-core shared IRQs over two boots. Corpus inputs unchanged; runtime disks
  are evidence-local copies. Constant-completion bridge remains removed.
  Evidence: `build-runtime-state/combined-runtime-timer-integrated-verification/20261007T060433Z/`.
* Newer profile-selection smoke proves default/explicit preparation and selected
  build reuse that exact fingerprint/binary. Selected validation passes boot/control
  and SIMD 84/84, but hostbus delay-fast fails before arming: firmware marker absent
  after 90 seconds. Slow-delay/GDB checks were not exercised. The cause remains
  under investigation, not dismissed as flaky or covered by the earlier pass.
  Evidence: `build-runtime-state/runtime-profile-cli-verification/20261007T073543Z/`.
* The hostbus review cycle closed: phase-count defect fixed (probe 66f4e07f…),
  raw fault suite 31/31, lifecycle 7/7, contract doc refreshed to executed
  qualification. Hostbus lane frozen.
* RC independent review found four defects after the initial green suite.
  Floating-island current conservation, element-index charge preservation,
  crossing-budget boundary and committed-state crossing values are corrected;
  **637 RC checks / 6 CTest targets**, including the state-copy planner API and sanitizers, pass.
  Evidence: `build-runtime-state/rc-review-2026-10-07/`.
* Electrical adapter review closed: ABI 2 (unfinished wiring = physical state,
  private groups, no hidden wires); 14 scenarios/2780 checks.
* REGI2C rev B: sequencing-only, fail-closed behind a measurement-provider
  interface; Wi-Fi/BLE reach the measurement dependency and pause with one
  precise diagnostic; boot 3/3. Measurement data needs an authorized hardware
  MMIO-read trace (not requested). Next evidenced blocker: IQ poll in the
  undocumented 0x60006000-0x60007FFF modem window.

Active work: native GPIO acceptance and ADC driver proof/clean stacked SENS
ownership; NativeNetWorker's solver-owned v3 graph and live QMP electrical
bridge; native I2C, SPI2/3, RMT, UART and I2S layers. Exact TIMG prescaler and
rational-rate corrections are integrated and verified above. The typed
editor's circuit/theme/project targets passed 3/3 (9/6/18 Qt results), with
actual Light/Dark surfaces inspected.

Current native UI contract: actual QOM support/ABI handshake, acknowledged
Paused/Initializing Apply, immutable submitted v3 retained only after matching
QMP ACK, failed Apply preserving prior accepted graph, bounded context-safe
snapshot polling. Legacy preview stays Idle/Stopped-only. Native electrical
QMP/firmware round trips remain pending, not inherited from host GUI tests.

Peripheral composition uses one desired controller-signal authority:
`esp32s3_electrical_set_matrix_drive`; GPIO owns actual selector/OEN snapshots
and receives resolved pad inputs, not a second controller-output store.
CoreClockWorker owns additive common source/gate/reset outputs; SPI/GDMA own
the shared real partial-RX EOF hook consumed by RMT. These additions remain
unqualified until their own native combined runtime exercises them.

User requests all double-checks through subagents and no new encryption-engine
or physical security work. Existing QEMU components remain compatibility code.
Routine verification decisions are ours; new physical operations are not
authorized by software continuation. All source/evidence changes must retain
the full remaining core/peripheral scope; operational checklist completion is
not a percentage of the 119-package plan.

Live state: [the continuation ledger](handoff/09-takeover-2026-10-07.md).

## Historical frozen handoff context — superseded above

The user requested a fine-grained summary for other agents, explicitly including
WSL2 interaction. The completed entry point is [docs/handoff/README.md](handoff/README.md).
Read that packet before resuming older instructions below. All three original
workers froze their files and ended their isolated jobs. The full goal and original
heartbeat remain active; no pause/completion was requested. Coordinate current
ownership before any automatic continuation edits the shared source/build.

Recorded verification at the earlier freeze (historical counts and hashes):

* Rebuilt actual GUI and required lane: **9/9 CTest, 0 skipped, 38.45 seconds**,
  tools/validate-required-wsl.sh, exit 0, log
  build-runtime-state/handoff-gui-validation.log. Session 27782 is finished.
* Worker isolated circuit/theme/project: **5/5, 6/6, 16/16**.
* Fresh offscreen light/dark previews completed and were visually inspected;
  copies under docs/handoff/assets. First table header clips in both themes;
  general arbitrary-net/analog editing and changed-preview rollback are pending.
* Reviewed hostbus extension rebuilt with reset/parser/reconnect/debugger/restore
  corrections. First actual enabled run FAILED BEFORE QMP: qom-type rejects
  esp32s3-hostbus-probe. Keep build-runtime-state/hostbus-first-run.
  qom-options.patch passed pristine applicability and is now appended to source-map
  and guarded apply whitelist, but is **unapplied/unbuilt/untested** in the cache.
  No enabled timing, restore-denial or raw fault qualification is claimed.
* IDF 5.5.5 source/tools preparation completed, job 91019 exit 0. Boot not tested.
* Durable acquisition journal added and parent reran **14 mocked host tests**.
  Deployment controller also has **14 passing mocks**. No physical programming.

The handoff validation ledger and lane documents supersede incomplete statuses
retained below for historical reproduction.

## Latest resumed evidence (supersedes pending corrective status below)

* Full rebuilt suite: **9/9 CTest targets passed, 0 skipped, 37.32 seconds**, with
  both development firmware fixtures enabled. Project corrections include actual
  codec identifier roundtrip, removal/repair/undo of legacy faults, safe temporary
  iteration, and preserved UNC paths. Historical geometry faults stayed strict.
* Transport now requires the original `BusEnvelope` for `sendResponse`, rejecting
  delayed callbacks from old connection/reset contexts; partial-frame watchdog and
  semantic ownership docs added. Author standalone suite: 69 Qt results passed.
* Stable IDF v6.1 merged image **passed real official-QEMU BootSmokeTest** (3 Qt
  results, 4.076 seconds). Image SHA256
  `bbb445d0fbba1ee8bd4ad3094ae501ff1dcfd92c3d4aa4179c410bb328b1973a`.
  Metadata collector now verifies named gitlinks against direct child HEADs:
  **28 submodules verified, 15 vendor libraries hashed**. Three local-repository
  metadata tests pass, including nested mismatch rejection.
* Hardware runner uses pinned esptool's public API with a single identified serial
  handle per protected operation; structured identity/security reads replace
  mixed text-log checks. First original verification/write and restore/verify stay
  quiescent; fourteen mock tests pass. Automatic unidentified reconnect retries
  are disabled. Immutable image/backup bytes close file-reopen races. Recovery
  begins only at the write checkpoint: preflight/original-verification failures
  leave flash untouched; partial writes require restore/verify. Independent review
  approved the twelve-case safety fix before the two immutable-byte regressions.
  Default all-image dry-run still opens no port.
  **Hardware flash approval is still pending; no physical write was performed.**
* Next active work: pure-v3 editor/model integration with Save separated from Apply,
  and an opt-in QEMU chardev/virtual-clock barrier prototype. Baseline source remains
  pristine. Inspect actual agents/build handles before starting or replaying jobs.
* Standalone GPL DC kernel added under `qemu-extensions/electrical`: analytic
  divider/driver/open-drain/floating/source/bridge cases pass, including bounds,
  singular/invalid inputs and 100 identical repeat solves (553 checks in eight
  scenarios). Independent review found/fixed an external island-current imbalance
  hidden by large internal circulation and an overflow diagnostic error. ASan/UBSan
  pass the physical fix; latest normal test passes diagnostic correction too. No QEMU pin/ADC
  integration is claimed; see [electrical-dc.md](contracts/electrical-dc.md).
* First opt-in hostbus extension binary built successfully from tracked inputs in
  `/home/polar/.cache/esp32s3vm/qemu-extension-40edccac4156/build-hostbus`.
  Prototype-disabled stable-IDF-6.1 BootSmokeTest passed 3 Qt results/3.719 s.
  Actual Qt peer compiled. **Enabled barrier harness has not run yet.** Independent
  review corrections are being prepared: reset after released/disconnected
  completion, strict depth-64 JSON/UTF16/error/discard limits, same-session
  reconnect floors and fresh UUIDs, GDB preflight before PC/step/signal changes,
  and early incoming/snapshot restore denial before runstate/job/transport changes.
  Parent applies revised copies and additional tracked patches only after author
  freeze; script hashes previous applied files and preserves unrelated edits.
* IDF 5.5.5 source/tool preparation is running under WSL, job `91019`, log
  `build-runtime-state/idf-5.5.5-prepare.log`; checkout completed and submodules are
  being fetched. Do not restart it without inspecting the handle/state.

## Working checkout and ownership

Repository: `C:/Users/weyst/Documents/ChatGPT/ESP32S3VM/Project_ESP32S3VM`.
Branch: `codex/simulator-foundation`. All changes remain local/uncommitted.
Existing baseline work and new files must be preserved. The full plan is
[development-plan.md](development-plan.md), with three detailed workstreams and
the reviewed coverage manifest. Many physical/peripheral functions remain unfinished.

Agents resumed successfully after the earlier limits. `ux_plan` owns the pure-v3
BoardWorkspace/model integration and its tests; parent owns MainWindow wiring and
shared CMake/build. `peripherals_plan` owns tracked hostbus prototype source and
harness, not either QEMU checkout; parent applies/builds in the separate extension
checkout after source freeze. `radio_simd_plan` performs read-only independent
reviews and is now auditing the DC kernel. No agent may access physical hardware.
Inspect live state/build handles before starting jobs; this file is not a process
inventory.

## Delivered and directly verified

* Carbon semantic Light/Dark/System themes, persisted preference, Qt 6.4 system
  palette fallback, shared QSS/QPainter tokens, device-pixel preservation.
* Shared firmware toolbar, acknowledged runtime phases/capability details,
  unavailable step/breakpoint gating, bounded QMP reply handling.
* Canvas zoom/pan/selection preservation, keyboard movement, fit/inspect affordance
  and corrected GPIO dropdown row heights. Actual images:
  `gui-esp32s3-simulator/build-wsl/studio-light.png` and `studio-dark.png` (dark may
  precede the last small inspect/row patch; recapture if presenting it as final).
* Seven-target full CTest with both development firmware fixtures passed 7/7,
  0 skipped, 45.76 seconds; Python device smoke passed. Later targeted canvas/theme
  checks after P2 fixes also passed.
* Official Espressif source built independently and pristine in
  `build-qemu-official-base`; peeled commit
  `40edccac415693c5130f91c01d84176ae6008566`, binary SHA256
  `1f65ad644630670b6d889bb4b38eaa75d08bcc01c6e1682cec5b891f21567291`.
  The tag object `6cdce334...` is NOT the source commit. Exact release and toolchain
  identities are recorded in `runtime-lock.json`.
* New official binary passed the real development boot/control fixture, including
  CPU snapshots, pause/resume and reset (BootSmokeTest 3 Qt results passed).
* Host transport library: author reported 66 Qt results passing after reset,
  canonical-string and handshake-capacity corrections; parent integrated transport
  target passed again. This is host framing/negotiation only, not native buses.
* Project v3 library: typed topology/geometry, analog quantities, undo/save/migration
  and schema exist. Corrective fifteen-result model suite and shared nine-target
  regression pass; upcoming GUI integration requires new validation.
* Hardware acquisition/parser: nine host tests passed. Recovery controller: fourteen
  mock tests pass after the reviewed safety corrections; no physical deployment.
* Same unmodified SIMD image emits 84 actual PIE vectors. Ordinary 15-second
  startup grace was added for console acquisition; source/ELF/flash frozen and
  a fresh QEMU run completed 84/84. This is seven operation/alias forms, not full ISA.

## Historical corrective investigations (resolved by latest evidence above)

The following retains the causes/reproduction context of repaired failures. It is
not the active failure list. Do not replay fixed changes or rerun old job handles.

1. **ProjectDocument tests / recovery / UNC.** Latest shared exec `65685` ended
   exit 1: `legacyAdapterDoesNotIgnoreChangedMetadataOrInvalidSource` validity
   assertion failed around line 250. `generatedOpaqueIdsRoundTripThroughRealTransportCodec`
   aborted in Qt array operations. Inspect the temporary-range expression
   `document.components().first().terminals`: store the returned components in a
   local value before iterating, rather than retaining a subobject of a destroyed
   temporary. Reconcile legacy current-vs-historical diagnostics precisely; do not
   relax genuine invalid drafts merely to make tests pass.
   Independent review also found foreign UNC paths (`\\server\share` and
   `//server/share`) mis-normalized/rebased under WSL. Preserve their foreign-host
   referents with diagnostics; add load/save-as/undo tests. UX agent owns
   `backend/ProjectDocument.*`, its test and `docs/contracts/project.schema.json`.
2. **Transport delayed callback context.** `sendResponse(requestId,...)` uses the
   current connection and can answer a new ordinal 1 after reconnect when an old
   model callback arrives. Approve/migrate to `sendResponse(const BusEnvelope
   &originalRequest,...)`, matching original session/connection/epoch/topology/
   sequence/ordinal against the active incoming request. No production callers
   exist outside the author's transport/tests; Project test uses codec only.
   Add real delayed-reconnect/reset regression. Keep existing 66-case repairs:
   reset enqueued before notifications, sends gated during cancellation, absolute
   regex anchors, worst-case handshake bounds. Author owns transport/test/schema.
3. **Hardware runner safety before any write.** `tools/run-hardware-reference.py`
   is prepared/default dry-run only; it has NOT executed. Independent review found
   preflight combines logs and checks only the first MAC, accepting mixed board
   identities. Require every identity to match; recheck intended board before each
   write AND restore. Keep original verification and restore→verify quiescent in
   ROM/stub (`--after no-reset` as appropriate), avoiding original app NVS changes
   between verify and first write or between restore and verify. Prefer same
   identified connection for a write if the pinned esptool API supports it. Add
   mocked mixed-identity/reassignment/quiescence regressions. Parent owns runner
   and `tests/hardware_deployment_test.py`; no agent may operate hardware.
4. **Stable IDF metadata timeout.** IDF v6.1 source/toolchain prepared successfully
   at `build-idf-6.1`, full commit `fff9895c82d744c7237be8847347bdd1b07c6643`,
   GCC 15.2.0. Exec `96388` terminated exit 1 AFTER successfully building/merging
   `tests/firmware/boot_smoke/build-idf-6.1/boot-smoke.merged.bin`.
   `runtime-manifest.py` timed out (20 seconds) on recursive submodule status.
   Make metadata collection bounded/robust without dropping identity evidence,
   rerun the lightweight manifest/test phase using existing objects/image, and
   execute the real BootSmokeTest. Stable v6.1 boot has not yet been tested.

Other review conditions: clearly assign native bus phase/address/direction/length
semantics to the integration/controller layer; codec bounds are insufficient to
prove full I2C/SPI transaction correctness. Add/document incomplete-frame assembly
watchdog if device events can hold a virtual horizon. Endpoint IDs must resolve to
actual graph terminals; opaque formatter acceptance does not imply connectivity.

## Hardware approval and preserved artifacts

The pending asynchronous question asks whether to run all three temporary images,
SIMD only, or keep firmware unchanged. **"Continue after rate limits" is not flash
approval.** Do not execute `--execute` until an explicit selection arrives.

Read-only ROM/stub operations identified COM5 as S3 QFN56 revision 0.2, 40 MHz,
USB Serial/JTAG, 16 MiB quad flash and embedded 8 MiB PSRAM; secure boot/flash
encryption disabled. Full original flash was read, no flash/eFuse writes performed.
Backup: `build-hardware-host/board-original-16MB.bin`, 16,777,216 bytes,
SHA256 `610b31430392a0d32228d7600c9c377881d5043c103c0c6d535e400a736edbc3`.
Keep private backup/data local; it is ignored by Git.

Frozen deployment manifest: `build-hardware-host/deployment-manifest.json`, with
board identity hash and image/manifest hashes. Prepared dry-run succeeded.

* SIMD image SHA256 `c4263180bd9f6531408707c2ba2d628b98359d82fb012916b2d71a3e20a3caeb`.
* Wi-Fi image SHA256 `73928a983806aacba641aa75cb6897f3b95994d7d48785f3865b9ba7086c21b3`.
* BLE image SHA256 `435a71cd0daf9f4758f1929eafa032cf9afef3c2232348acb3ec3ad7218396d2`.

All have 15-second ordinary application delays and UART0+secondary USB console.
Manifests/ELFs live under each fixture's `build` folders. Physical runner uses pinned
Windows environment `build-hardware-host/Scripts/python.exe` with esptool 5.4.0.
Safety issues above have been repaired and mock-tested. If an approved physical
run starts and programming is attempted, restore the complete immutable backup and
verify it even after capture/write failure. Failed preflight/original verification
before any mutation must not restore a stale backup. A restoration failure remains
an active recovery requirement.

## Native radio evidence and QEMU next phase

Unchanged IDF 6.2-dev Wi-Fi initializes NVS/netif/event loop/Wi-Fi/mode then stalls
at `esp_wifi_start` full RF calibration in official and recovered QEMU. Captured
ROM PC `0x40036a46`, caller `ram_read_sar2_code`; it polls `0x6000e050[26:24]` for
7 after writes e060←00ff501a, e05c←0080016a and e05c←0088016a. Public bitfield name
was not found. Do not implement unconditional DONE: model timed analog/SAR/PWDET,
clock/reset/ownership behavior from source and hardware evidence.

BLE independently fails: official controller LP-clock assertion
`select_src_ret && set_div_ret`; recovered gets farther then LoadStorePIFAddrError
at PC0x40056f60/EXCVADDR0x3fc00100 (r_rw_rf_init/r_rwip_init/controller task).
Failures, hashes and traces are retained in [hardware-reference.md](hardware-reference.md)
and ignored fixture runs. No radio support is promoted.

QEMU barrier feasibility design is in
[qemu-hostbus-prototype.md](contracts/qemu-hostbus-prototype.md). No source patch
has been applied. Default virtual timers can execute in vCPU context under icount;
use main Aio-context virtual timer for safe exact stop. A central start-blocker
must prevent QMP/GDB advancing during unresolved dependency. No auto-resume:
response resolves, explicit release clears lease, separate cont preserves pause
intent. Prototype is QOM/chardev host testing, no fake firmware MMIO. Parent owns
extension checkout/patch manifest/common integration; preserve pristine baseline.

Resume corrective fixes/validation first, then UI project integration, physical
electrical kernel/GPIO/IRQ/clock/GDMA, native peripherals, radio and full SIMD
qualification. The original large goal is not achieved by these foundation tests.
