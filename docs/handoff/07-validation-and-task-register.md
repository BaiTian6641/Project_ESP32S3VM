# Validation ledger and ordered work

This ledger separates actual tests from source/design checks and unexecuted work.
Historical evidence stays attached to the source/profile it tested.

## Evidence at packet creation

| Area | Actual result | Boundary |
| --- | --- | --- |
| Full plan structure | 30 rows/119 packages/15 acyclic milestones/5 gates; 8 negative cases rejected | Structural coverage, not physical correctness |
| Earlier integrated GUI/runtime/native lane | 9/9 CTest, 0 skipped, 37.32 s | Before latest BoardWorkspace/model integration |
| Latest circuit editor isolated | 5/5 Qt results, 1.14 s | Actual net/save/drag/undo/PID/Apply tests |
| Latest theme isolated | 6/6 Qt results, 0.92 s | Includes latest editor API |
| Latest project model isolated | 16/16 Qt results, 0.43 s | Includes stricter exporter API/metadata guards |
| Latest shared editor build/lane | **9/9, 0 skipped, 38.45 s** | Rebuilt after frozen v3 editor integration |
| Runtime contract | 17 Qt results including real/fake QMP bounds | Unverified debug step/breakpoint still disabled |
| Host transport | 69 Qt results | Framing/context/reset/watchdog; not native bus semantics |
| Official IDF 6.1 boot | 3 Qt results, 4.076 s | Plain boot/control, not all peripherals |
| Initial extension-disabled IDF 6.1 boot | 3 Qt results, 3.719 s | Before prepared QAPI registration patch |
| IDF metadata collector | 3 repository tests; 28 gitlinks/15 libraries verified for 6.1 | Provenance, not compatibility |
| IDF 5.5.5 | Source/tools preparation completed | No boot fixture result yet |
| QEMU probe source | Pinned syntax and patch applicability; strict JSON 24 checks; actual peer compiled | Enabled launch currently fails before QMP |
| DC kernel | 8 scenarios/553 checks | Standalone analytic solver; no native pad/RC/ADC |
| DC sanitizers | ASan/UBSan passed physical balance correction | Latest normal run also covers diagnostic overflow correction |
| Hardware deployment controller | 14 mocks passed | No physical programming/restoration occurred |
| Acquisition/parser/journal | 14 mocked tests passed; parent rerun passed | No physical comparison; partial journals never qualified |
| SIMD | 84 actual QEMU vectors and repeat equality | Seven forms only; hardware comparison pending |
| Wi-Fi/BLE | Native failures traced and retained | Controller/data/security gates have not passed |

## Final shared verification

The parent completed tools/validate-required-wsl.sh after all workers froze:
**exit 0; 9/9 passed, 0 skipped, 38.45 seconds**. The actual application and
required test executables rebuilt. Log: build-runtime-state/handoff-gui-validation.log.
Two sequential offscreen application previews completed successfully and were
visually inspected; assets/light.png and assets/dark.png preserve them. The table
header clipping is a documented P2 polish issue.

The first inline Windows→WSL invocation lost shell variables and CMake saw
/gui-esp32s3-simulator. That was an invocation error, not a source compile failure.
The saved Bash helper avoids this quoting boundary and checks both required
firmware files before CTest, preventing optional skips from looking successful.

Exact worker isolated commands from PowerShell:

~~~powershell
wsl -d Ubuntu --cd /mnt/c/Users/weyst/Documents/ChatGPT/ESP32S3VM/Project_ESP32S3VM/gui-esp32s3-simulator -- env QT_QPA_PLATFORM=offscreen ./build-wsl/ux-standalone/circuit-v3-test
wsl -d Ubuntu --cd /mnt/c/Users/weyst/Documents/ChatGPT/ESP32S3VM/Project_ESP32S3VM/gui-esp32s3-simulator -- env QT_QPA_PLATFORM=offscreen ./build-wsl/ux-standalone/theme-v3-test
wsl -d Ubuntu --cd /mnt/c/Users/weyst/Documents/ChatGPT/ESP32S3VM/Project_ESP32S3VM/gui-esp32s3-simulator -- env QT_QPA_PLATFORM=offscreen ./build-wsl/project-standalone/project-document-ui-export-test
~~~

Operational mapping: H-BUS tasks implement NET-04/05; H-CORE-01/02/03 map to
canonical CORE-02/03/04 respectively; H-NET tasks map to NET-01..03 with ANALOG-01;
H-ANALOG-01 maps to ANALOG-02/03; H-ADC-01 maps to ADC-01..05. The H-* numbers are
handoff task identifiers, not renamed canonical packages.

## Immediate task ordering

| Order | IDs | Dependency/exit |
| --- | --- | --- |
| 1 | H-UX-01..05 | Frozen sources → shared regression → actual visual/state review |
| 2 | H-BUS-01..09 | Apply prepared QAPI patch → real launch → exact timing/debugger/lifecycle/fault/restore evidence |
| 3 | H-CORE-01..03, H-NET-01..02 | Proven schedule contract → clocks/resets/IRQ/GDMA → actual graph/pad paths |
| 4 | H-ANALOG-01, H-ADC-01 | Working digital/DC topology → RC/acquisition/calibration/native sampling |
| Parallel | H-REF-01, H-SIMD-01..04, H-RADIO-01 | Host preparation/version discovery can proceed; physical action waits for approval |
| Later | Canonical UART/I2C/SPI/RMT/I2S/LCD/CAM and SoC packages | Their clock/IRQ/DMA/pad dependencies must pass; no superficial port |
| Release | M11, UX-12/14 and per-feature qualification | Required profile matrix/accessibility/packaging/soak/evidence complete |

Each H-* task is operational decomposition of the existing full plan, not a new
replacement milestone. Detailed register/phase/timing/negative/reset/concurrency
acceptance remains in the canonical package documents.

## Required review pattern

For each task record:

1. Trigger/problem and exact current reproduction.
2. Changed tracked files, public API/schema changes and pinned base/patch order.
3. Positive, negative, reset and concurrency/timing tests appropriate to the feature.
4. Exact binary/source/config/firmware/tool hashes and command/environment.
5. Observed bytes/events/timestamps; retained failures and incomplete evidence.
6. Capability maturity/availability and remaining limitations.

A passing host/model test does not promote native hardware behavior. A full suite
with optional skipped native tests is not a required release lane. Source review
does not substitute for actual enabled QEMU execution.

## Artifacts to preserve

| Artifact | Use |
| --- | --- |
| build-runtime-state/hostbus-first-run | Actual enabled-launch failure |
| build-runtime-state/hostbus-extension-source.json | Applied source/patch hashes before QAPI patch |
| build-runtime-state/hostbus-runtime.json | Actual extension binary identity |
| build-runtime-state/idf-5.5.5-prepare.log | Completed source/tool preparation |
| tests/firmware/*/build*/runtime-manifest.json or reference-manifest.json | Firmware/tool/vendor-library identity |
| tests/firmware/*/build/runs | Captures/traces; failed radio attempts stay failed |
| build-hardware-host/deployment-manifest.json and original backup | Private original-board recovery material |
| gui-esp32s3-simulator/build-wsl | Shared test executables/previews; source is authoritative |

Most are ignored. Preserve needed artifacts separately when moving machines or
archiving a worktree; never upload the private original flash automatically.

## Ownership and ongoing automation

All three original workers were asked to freeze for documentation. Before edits,
confirm live agents/processes and claim a disjoint scope with one coordinator.
The original active goal and 30-minute heartbeat were not paused or completed.
Do not allow a new coordinator and that continuation to mutate the same shared
build/source simultaneously. Current source snapshots and logs are more reliable
than old execution handles or historical prose.
