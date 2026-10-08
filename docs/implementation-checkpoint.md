# Implementation continuation checkpoint — 2026-10-07

Goal remains **active and incomplete**. The user explicitly requested takeover and
parallel implementation in this session. Preserve the reviewed full scope; do not
reduce completion to the foundation slices below. Historical heartbeat metadata
is retained in the frozen packet; it is not an inventory of this session's jobs.

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
