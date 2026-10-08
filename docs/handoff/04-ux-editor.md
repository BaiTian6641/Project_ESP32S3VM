# Carbon UX and project editor takeover

Canonical packages: UX-01..14 in [the detailed UX plan](../plans/ux.md).
The source is Qt 6.4.2/C++17 under WSL2.

## Implemented behavior

ThemeManager provides System/Light/Dark, persisted appearance/mode, semantic
Carbon g10/g100 colors, QSS and painter tokens. Qt 6.4 cannot directly follow the
OS appearance API used by newer Qt; System uses a documented palette fallback.
Theme changes update existing canvas/device chrome while preserving actual display
pixel images, viewport, selection and project state.

MainWindow supplies a shared firmware toolbar, run/pause/resume/stop/reset,
appearance and runtime support details. ControlPanel/Debug share firmware identity.
CPU state clears stale/unavailable data. Unsupported step/breakpoint controls are
gated. Firmware expects a merged flash image; ELF is an explicit debug path.

The new BoardWorkspace source uses one ProjectDocument. Public APIs:

* projectModel(), circuitDocument(): typed model/pure v3 snapshot.
* openCircuit(path,error), saveCircuit(path), newCircuit(): document operations.
* applyCircuit(error), applyUnavailableReason(): explicit guarded legacy-preview Apply.
* setRuntimeStatus(const RuntimeStatus&): acknowledged runtime gating.
* setNativeCircuitAvailable(bool, reason): support confirmed by a live native QOM handshake; defaults unavailable.
* requestNativeApply(error), nativeApplyUnavailableReason(): Paused/Initializing-only native request, blocked while in flight, on invalid drafts or without explicit electrical model selection.
* nativeApplyRequested(const QJsonObject &document): pure v3 request; no device-preview export, process restart or optimistic acceptance.
* setNativeApplyInFlight(bool, result): parent clears the request after QMP success/failure and supplies the visible acknowledgement/error.
* setNativeCircuitSnapshot(snapshot, acceptedDocument): actual ABI1 QOM snapshot plus last acknowledged v3 graph, not the current draft. Missing/invalid voltages remain unavailable; subsequent electrical edits display stale. Geometry alone does not invalidate readings.

MainWindow now connects the runtime signal and initial status to that setter.
Shared CMake circuit/theme targets now link ProjectDocument and RuntimeContract.

Canvas wires come from actual graph endpoints. Shared GPIO/I2C/SPI edits are undo
macros; drag motion previews geometry and release creates one command. Ctrl+Z,
Ctrl+Y/Ctrl+Shift+Z and Ctrl+S are scoped to the workspace. Empty projects and removal
of the last external component are allowed.

The catalogue now includes voltage/current sources, resistors, capacitors,
potentiometers, switches, buttons (v3 switch kind), explicit ground and a new logical
S3 MCU with explicit VDD/GND terminals. Imported documents never acquire implicit
power terminals or supply wiring. Numeric quantity unit changes use quantityForUnit;
invalid repairable values and quantity extensions survive Save/Open.

Nets provides a keyboard-equivalent create/name/remove/connect/disconnect workflow
using global terminal IDs; Return renames and Delete in the member list removes only
the selected branch. Wire terminals enables two-dot pointer wiring/branching;
Ctrl+click connects to the selected net and Alt+click disconnects that terminal.
Different occupied nets require explicit branch disconnect before reconnecting.
Selected nets highlight all branches without turning crossings into connections.

Nets also selects the explicit native driver model and DC/RC edit charge policy.
The s3-explicit-finite-v1 simulation model uses 40 ohm output impedance, 45 kohm
firmware-enabled weak pulls and 0.25/0.75 rail thresholds; it is not silicon
characterization. Supply/reference must be real wired v3 terminals.
The bundled peripherals/project.analog-divider-button.example.json includes explicit
rails, an ADC divider and a pull-up/button node; firmware mux/open-drain behavior
still comes from the real native peripheral state, never the example metadata.


## Save/Apply rules to retain

Save and Open must never call PeripheralManager::loadConfig or restart devices.
Save writes pure v3, including invalid repairable drafts. Imported v2 UI defaults
to Save As. Legacy-preview Apply requires acknowledged Idle/Stopped.
Native Apply instead requires an acknowledged live Paused/Initializing machine and
confirmed QOM support; Running/unknown/stopping/error and in-flight requests block.
Only the parent's QMP result can accept the runtime graph. Stop has no live machine.

Legacy-preview Apply uses the strict legacy exporter with explicit resource destination/absolute
referents. Unsupported analog/topology/profile/firmware/runtime/terminal/net/decoder
changes need a visible reason. Do not silently drop fields or route via addresses
while claiming electrical behavior. A layout-only/no-op Apply should not restart
otherwise unchanged processes.

Manager polling/status must not reload and discard the document. Selection/zoom/
inspector follow stable IDs; scene reconstruction after an edit must not invalidate
a live drag or undo callback.

## Evidence at the handoff boundary

CircuitSurfaceVerify's 2026-10-07 circuit-editing build used
`/tmp/circuit-ux-build`, the real app and all nine existing suite targets,
`BUILD_JOBS=4`. Final targeted Qt results: circuit **9 passed**, theme **6 passed**,
project **18 passed**, no failures. Existing bridge **7**, launch **5**, runtime
contract **17** and transport **69** passed in the preceding suite run. Boot and
native I2C each skipped one runtime case because firmware/QEMU variables were absent;
this is not an all-runtime green claim. `final-build.log`,
`final-targeted-ctest.log`, `fixed-ctest.log` retain exact evidence in that directory.

The verifier inspected real app and BoardWorkspace Light/Dark captures at 1440x960:
`main-{light,dark}-fixed.png`, `workspace-{light,dark}-component.png`,
`workspace-{light,dark}-nets.png`, `workspace-{light,dark}-nets-bottom.png` and
`button-{closed,open}.png`. Parameters/units and terminal headers are readable;
inspector scrolling keeps lower disconnect/state controls reachable. A real-widget
harness exercised Button Set closed/open, 10 kohm to 10000 ohm conversion and exact
Save/reopen; `workspace-capture.log` and `button-divider-reopened.json` retain those
artifacts. Harness source was removed. Native values remained explicitly unavailable.
These are Qt offscreen captures and widget interactions, not a WSLg screen-reader or
physical input/accessibility qualification, and not native electrical/ADC evidence.

Initial destruction-time callback and occupied-endpoint pointer-branch failures
were fixed and verified by the same subagent; original failure/backtrace logs remain.
The following older results/captures describe the earlier handoff, not this source.

Previous complete GUI/runtime/model/transport lane: 9/9 passed, 0 skipped,
37.32 seconds, **before the latest editor rewrite**.

Latest worker isolated circuit-v3 run: **5 Qt results passed** (3 substantive
cases), 1.14 seconds. It covers shared actual-net edits/pure-v3 Save/catalogue,
one-command drag and keyboard undo/redo/save with unchanged PIDs, and runtime
Apply/invalid draft/empty removal. Theme **6/6** and project **16/16** also pass.
Parent then rebuilt the shared app and all required targets: **9/9 CTest, 0 skipped,
38.45 seconds**. Current MainWindow/API/CMake integration is covered by that build.

Do not transfer the earlier 9/9 result onto changed source without rebuilding.
Earlier studio-light/dark images precede this model integration. Fresh inspected
handoff-light/dark.png and copies under docs/handoff/assets cover the frozen source.
The right table's Terminal / parameter header is clipped in both previews at the
default window size. Canvas fit labels are small; Inspect provides a closer view.
Retain these as explicit polish findings, not a full accessibility pass.

Other important unfinished behavior:

* Project firmware/runtime metadata is retained but does not activate MainWindow's
  Run profile. Changed unsupported metadata blocks legacy Apply.
* Analog quantities and general terminal/net editing are now enabled. Actual live
  readings still require the parent to wire the native setters to acknowledged QOM
  snapshots; neither the editor nor legacy preview computes electrical voltages.
* New analogue components intentionally require native runtime support and may
  disable legacy Apply. Unsupported solver topology/parameters are rejected by the
  backend without replacing the accepted graph; the editable document remains.
* Full changed-compatible-preview restart/rollback and broader GUI fault/recovery
  flows are not covered by the current no-op/guard/PID tests.

## Fine-grained next tasks

| ID | Change/verification | Acceptance |
| --- | --- | --- |
| H-UX-01 | Finish shared build after worker freeze | MainWindow, executable, circuit/theme/project targets link with the published API |
| H-UX-02 | Run full required regression with both firmware variables | No unexpected skips/failures; record binary/profile and exact command |
| H-UX-03 | Recapture Light/Dark/System and inspect small/large windows | Readable pin rows/status/diagnostics, no clipped critical actions, preserved viewport |
| H-UX-04 | Validate imported v2 → Save As → relocate → reopen | Pure v3 only, same resources/net endpoints, undo clean state correct |
| H-UX-05 | Stress manager polling while editing/dragging/saving | No document loss, accidental restarts, stale selection or extra undo commands |
| H-UX-06 | Add analog component/voltage/ADC UI after native gates | Units and unsupported states visible; measured values tied to actual solver/controller |
| H-UX-07 | Accessibility and keyboard wiring/trace flows | Focus order, contrast, screen reader labels, zoom/scaling and non-color diagnostics verified |
| H-UX-08 | Catalogue provenance/labels and changed-preview recovery | Full template provenance, cached address/CS/units and restart/rollback failures handled explicitly |

The first five tasks are a completion/review slice. Analog/trace/debug/radio panels
are dependent functionality, not decoration that can simulate success.
