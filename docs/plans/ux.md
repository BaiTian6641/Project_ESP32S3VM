# UX workstream: circuit studio, themes and evidence

Research and source audit date: **2026-10-07**. Status: **plan only**.

This workstream covers the Qt studio, not a replacement web frontend. The recorded
host baseline is Qt 6.4.2 under Ubuntu/WSL2. The maintained engine baseline is
Espressif's official QEMU fork. Existing code and passing fixtures are recorded in
[current-status](../current-status.md); the transport boundary is described in
[runtime-architecture](../runtime-architecture.md). Planned UX must not promote
unverified peripheral, radio, ADC or SIMD behavior to supported status.

The goal is a studio in which users can create/open a project, import normal
firmware, identify runtime capabilities, connect digital and analog components,
run and inspect a simulation, recover from errors and preserve their work using
either a keyboard or pointer, in persisted Light/Dark/System modes. A hardware
reference workflow can capture local evidence from an explicitly selected board
such as COM5. Hardware access is optional; ordinary simulation remains independent
of a connected board.

## Findings that constrain the implementation

The following findings are from the present source, not assumptions about the old
generated documents:

| Source | Present behavior | Required correction |
| --- | --- | --- |
| `src/main.cpp` | One hardcoded Gray 100 stylesheet and requested IBM Plex font | Shared semantic theme service, font fallback, persisted preference |
| `src/BoardWorkspace.cpp` | Graphics pens/brushes/text/grid use hardcoded dark colors | Theme-aware scene items; authentic device pixels remain unaffected |
| Device/pin panels | Inline dark colors in `PinConnectionWidget`, `SchemaDevicePanel`, `GenericDevicePanel`, `DevicePanelBase`, `DisplayPanel` and `PeripheralsWidget` | Full palette inventory, including dynamically built controls |
| `BoardWorkspace` | JSON mutation and full scene rebuild; resize/rebuild fits the entire scene | Document model, command-based editing, persistent viewport/selection |
| `BoardWorkspace::saveCircuit` | Save also reloads runtime configuration | Separate persistence, layout edits and runtime apply |
| `BoardWorkspace::removeDevice` | Last component cannot be removed | Empty projects are valid |
| `MainWindow` | Bundled devices auto-start; process-start event says runtime running | Explicit project entry flow and verified CPU/session state |
| Control/Debug panels | Duplicated firmware fields; normal panel defaults to 16 MB, CLI helper forces 4 MB | One firmware/runtime profile with validated configuration |
| `QemuController` debug methods | Step/breakpoint HMP requests announce intent without demonstrated successful acceptance | Capability-gated controls and acknowledged results |
| Serial panel/controller | Several physical/framing modes are selectable, but transport remains a host byte stream | Distinguish terminal transport controls from modeled UART/transceiver behavior |

Board GPIO assignments are currently metadata, with selected I2C paths verified
through the recovered extension. No analog voltage solver or ADC end-to-end fixture
is presently qualified. Existing screenshots are design evidence, not accessibility
or hardware fidelity evidence.

## Shared contracts and sequencing

UX agents must agree these contracts with the engine/electrical teams before
implementing dependent surfaces:

1. **Project document:** stable instance, pin, net and component IDs; schema version;
   board/module profile; firmware/runtime profile; analog component parameters;
   geometry stored separately from electrical graph; migration rules.
2. **Capability manifest:** engine revision, model revision, feature granularity,
   evidence IDs and supported profiles. Canonical maturity is catalogued,
   implemented, native-tested, hardware-compared or qualified; availability/reason
   is separate. UI labels verified/experimental/unavailable/not tested derive from
   this evidence and never promote it. A single bridge boolean is insufficient.
3. **Runtime state:** acknowledged launch/control transitions; session/reset ID;
   per-device process state; guest state versus host process state; pending apply
   generation and structured failure messages.
4. **Signal/trace stream:** virtual timestamps and sequence numbers; resolved
   analog/digital values; unknown/error states; sampling/decimation metadata;
   transaction IDs; channel units; bounded recording and explicit dropped counts.
5. **Apply semantics:** changes requiring pause/restart, changes safe during
   execution, rollback behavior and rejected topology/configuration results.
6. **Hardware evidence job:** selected port/board, exact fixture build, requested
   operations, expected reset/flash behavior, local output paths and cancellation.

UX-01 and UX-02 can begin alongside engine foundations. UX-03 through UX-05 can
progress with simulated structured runtime events and must then be integrated
against the actual contract. Real net/analog interaction and traces cannot be
qualified until their corresponding engine paths exist. Mock values must be visibly
marked in development previews and excluded from support evidence.

| ID | Priority | Deliverable | Dependencies |
| --- | --- | --- | --- |
| UX-01 | P0 | Model/actions and verified user journeys | Shared contracts 1-3 |
| UX-02 | P0 | Light/Dark/System semantic themes | UX-01; Qt version/platform decision |
| UX-03 | P0 | Shell, entry flow and firmware import | UX-01; artifact inspection |
| UX-04 | P0 | Correct runtime state and capability feedback | UX-01; shared contracts 2-3 |
| UX-05 | P0 | Persistence, undo and safe recovery | UX-01; document migrations/apply contract |
| UX-06 | P1 | Circuit interaction and digital pin feedback | UX-05; net/board metadata and routing |
| UX-07 | P1 | Analog voltages, sources, resistors and ADC UI | UX-06; analog solver and ADC model |
| UX-08 | P1 | Structured problems and device recovery | UX-04-05; engine/device error taxonomy |
| UX-09 | P1 | Signal, bus and ADC traces | UX-04/06/07; timestamped recording contract |
| UX-10 | P1 | Debugger and chip-state inspection | UX-04/09; verified debugger/register API |
| UX-11 | P1 | Peripheral/radio interaction panels | UX-04/09; individual modeled data paths |
| UX-12 | P0/P1 | Accessibility and scalable interaction | Starts in UX-01; completes across all surfaces |
| UX-13 | P1 | COM5/local reference capture and comparison | UX-03/08/09; hardware runner and evidence schema |
| UX-14 | P0/P1 | Compatibility/evidence matrix and qualification | UX-04/13; versioned fixture results |

P0 items form the usable next studio slice. P1 delivery follows each engine gate;
P1 does not mean all peripherals can be completed in one iteration. Accessibility,
error handling and truthful capability labels are ongoing requirements, not final
polish tasks.

## UX-01: project model, actions and user journeys

Create a model-backed `ProjectDocument`, runtime-session view model and common
`QAction` commands. Widgets and graphics items subscribe to stable model IDs;
neither owns an independent version of the circuit. Undo commands operate on the
model. Backend state drives command enablement.

Walk through: first firmware boot; adding an I2C sensor and display; GPIO button
and LED; resistor divider into an ADC input; diagnosing missing ACK and unsupported
topology; pausing/register inspection; changing theme; saving/reopening a relocated
project; optional capture/comparison with a physical board.

Acceptance and failure checks:

- Every action declares its available states, document mutation, runtime side
  effect, undoability and recovery path. No duplicate firmware profile exists.
- Selection is the same in catalogue, canvas, inspector and component list.
- A missing runtime does not prevent opening/editing/exporting a project.
- Theme/keyboard review applies to each journey before implementation starts.
- Historical JSON examples have explicit migration fixtures and retain component
  IDs, execution paths, endpoints and saved layout.

## UX-02: semantic themes and persisted appearance

Create one theme service with pinned Carbon token data. Light uses Gray 10; Dark
uses Gray 100. Map semantic background/layer/text/field/border/focus/support,
selection, hover/pressed and disabled tokens into `QPalette`, generated QSS,
graphics-item pens/brushes and custom painting. Add application-specific tokens
for bus lanes, net states and analog overlays; do not scatter raw hex values in
constructors. Carbon supplies these theme families and role-based tokens, and
recommends matching system appearance absent a user choice. [S1][S2]

Persist `appearance/mode = system|light|dark`, readable font scale, density and
window/dock state through `QSettings`. Explicit mode overrides system changes.
Qt 6.4.2 has no `QStyleHints::colorScheme`: decide whether to raise the minimum
Qt version or provide a tested adapter with visible fallback. Qt 6.5 adds detection;
the setter is newer and is only a platform hint. Handle palette-change timing
correctly rather than assuming the old palette has already changed. WSLg and native
Windows must have separately documented appearance behavior. [S3][S4]

Package IBM Plex Sans/Mono with the applicable attribution or use an intentional,
tested fallback. Support larger user-selected text. Device framebuffer colors,
camera frames and captured waveforms are data and must not be recolored to fit
the theme; their surrounding controls use theme tokens.

Acceptance and failure checks:

- Switch theme while running/paused, while a popup is open, after adding a dynamic
  panel and after loading a project. No session restart, dirty-document mutation,
  selection loss, viewport reset or dropped capture is permitted.
- Inspect every tab, file dialog policy, menu, combo popup, tooltip, card, grid,
  status and schema-generated control in both themes and all interaction states.
- Restart preserves explicit preference; System follows detectable platform
  changes and states its fallback when detection is unavailable.
- Measure contrast of intended token pairings, including custom wire/analog
  labels and focus. Use text/pattern indicators alongside signal colors.
- At 100%, 150% and 200% scale, and with font fallback, essential controls remain
  visible and dialogs usable. Screenshots supplement functional checks.

## UX-03: shell, onboarding and firmware import

Provide New/Open/Recent/Examples on an empty project state. Examples describe their
required capabilities and compatible fixture versions. Resolve catalogue/runtime
locations through installation/project configuration rather than expecting users
to know repository-relative paths. Keep a component catalogue, central circuit,
properties and dockable Console/Problems/Signals panes.

The persistent header exposes project, firmware identity, engine profile and
Run/Pause/Stop/Reset. One import flow identifies merged flash images, standalone
application BINs and ELF/debug symbols. It previews flash segments/offsets,
configured memory and relevant compatibility evidence. Unsupported imports receive
specific repair guidance; the app must not guess a valid bootloader or label ELF
execution equivalent to normal flash boot. Expert settings remain accessible in
the runtime profile without crowding the initial task flow.

Acceptance and failure checks:

- From a clean launch a user opens an example, selects its firmware and sees real
  boot output without visiting two separate firmware tabs.
- Invalid path, unreadable image, app-only BIN and incompatible flash/PSRAM settings
  are distinguished. Cancelling import keeps the previous profile and session.
- New empty project is valid; arbitrary device processes do not auto-start merely
  because the user opened the studio.
- The header commands and import fields have keyboard access, visible labels and
  identical enabled/disabled meaning in both themes.

## UX-04: runtime state and feature capability

Represent acknowledged states: Idle, Validating, Launching, Connecting QMP,
Initializing devices, Running, Paused, Waiting for debugger, Stopping, Stopped and
Failed. CPU execution state is distinct from QProcess state. Guest panic/watchdog
reset, user target reset and process restart are distinguishable. Give startup
stages and cancel/retry actions rather than an indefinite busy indicator.

Display engine/version and feature-specific status with reason/evidence. A device
can be running independently while its firmware transport is unavailable. Pending
electrical edits show an unapplied generation. Configuration changes requiring
restart say so before the action, using already authorized user intent; layout
changes require no runtime operation.

Acceptance and failure checks:

- Delayed QMP startup, timeout, failed capability probe, device-process crash,
  intentional pause, debugger wait and guest reset produce distinct states.
- Process-start alone never says CPU Running or firmware boot successful.
- Stop/cancel removes stale pending callbacks; late events from an earlier session
  cannot overwrite the new session's status.
- Unavailable controls are disabled with a short reason accessible by keyboard;
  experimental controls have an explicit status and are never called verified.

## UX-05: persistence, undo and safe editing

Separate Save, Save As and Apply circuit. Geometry and viewport settings are not
electrical mutations. Use command-based undo/redo with meaningful command names,
clean-state tracking and merged drag operations. Qt's undo stack supports these
behaviors. [S5]

Persist atomically, retain stable IDs, use portable project-relative asset paths
and restore panel/viewport state. Keep a recovery draft independent of saved files;
handle externally modified project files. Opening/closing/replacing a dirty
document must offer Save/Discard/Cancel without dropping the working circuit.

Acceptance and failure checks:

- One drag is one undo action. Undo restores component deletion, branches,
  electrical parameters and shared assignments; redo restores their IDs.
- Save layout while firmware runs: device processes, bus state and guest execution
  continue. Applying an electrical generation follows backend acknowledgement.
- Disk full/permission failure and invalid runtime topology preserve the draft and
  last accepted runtime. Saved-but-not-applied state is explicit.
- Save As/reopen from another directory resolves model executables/assets; missing
  assets identify the affected instance and provide relocation.
- Keyboard Save/Save As/Undo/Redo produce the same result as toolbar actions;
  unsaved and unapplied indicators remain distinct in both themes.

## UX-06: circuit editing and digital pin feedback

Provide persistent zoom/pan, Fit, search, selection/inspector synchronization,
rename/duplicate/remove, pin-to-pin wiring, branches/junctions, reconnect and
disconnect. Show explicit net IDs/names and highlight all members of a selected
net. Visual wire crossings must not imply electrical junctions. Add a model-backed
connection table as an equivalent keyboard route.

Use board/module profile metadata rather than displaying all chip pads as an
unqualified physical DevKit header. Expose unavailable/reserved flash/PSRAM pins,
strap pins, direction, actual GPIO-matrix function, driver mode, pulls, resolved
level and last edge time. Warn on contention, disconnected inputs, incompatible
drivers and address/CS collisions while retaining an editable invalid draft.

Acceptance and failure checks:

- Canvas and table operations create the same graph; branch deletion cannot
  accidentally disconnect unrelated members. Invalid GPIO/profile changes name
  affected endpoints and are undoable.
- High/low/floating/unknown/contention have text or pattern as well as color.
  A saved assignment alone never displays a live working connection.
- Resize/theme change does not reset zoom. Drag respects platform drag threshold;
  keyboard selection, movement, connect/disconnect and inspector access work.
- Engine loopback fixture shows a real output edge routed to an input/interrupt;
  button/LED interaction and open-drain/pullup cases are checked against actual
  events before live routing is marked verified.

## UX-07: analog voltages, resistor/source controls and ADC

Analog is part of the requested electrical scope. Include explicit ground/reference
and supply nodes, voltage sources, resistors/pullups and parameterized analog sensor
outputs. Values display and edit in V/mV and ohm/kohm with clear canonical units,
finite numeric validation and supported bounds. Source controls expose voltage,
source resistance/impedance and, when supported, waveform/time parameters. Resistor
controls identify endpoints and resistance; a divider is represented by a real
graph, not a hidden ADC-only slider.

The voltage inspector obtains solved values from the electrical engine, not a
duplicate UI solver. Show node voltage relative to the selected reference, relevant
current/impedance when modeled, solver state, timestamp and configuration generation.
Distinguish resolved, floating, underdetermined, contention, out of range,
unsupported topology, solver failure and stale/frozen values. A voltage gradient
overlay supplements numeric labels; it never substitutes for them.

At digital input pins, show the active electrical profile's low/high thresholds and
undefined region rather than silently rounding any voltage to zero/one. Open-drain
lines use their solved pullup/net voltage. Overvoltage/short/conflicting ideal-source
states identify contributing components and editable corrective actions. Unmodeled
impedance, reactive/nonlinear elements or solver topology must produce explicit
unsupported status, never an invented valid voltage. The initial supported topology
set and transient behavior come from the electrical workstream's capability manifest.

For ADC show unit/channel/pin, guest-selected attenuation and bit width, sample
time/rate, raw conversion, effective input voltage, calibrated result availability,
calibration/eFuse profile, clipping/out-of-range state and continuous-mode overflow.
Firmware-owned ADC settings are read-only observations unless an explicitly separate
debug override is active. Present ideal simulation and board-calibrated profiles as
different evidence/configurations. Espressif documents oneshot, continuous and
calibration paths; detailed usable input ranges belong to the chip datasheet and
the chosen profile, not a universal 0-3.3 V mapping. [S9]

Acceptance and failure checks:

- Keyboard-only create ground, source, two resistors and ADC connection; edit units,
  save/reopen and undo parameter/net changes. Both themes show voltage numbers and
  undefined/unsupported states with readable labels.
- Engine DC divider fixture displays the backend's solution and corresponding
  normal-driver ADC raw/calibrated output under specified tolerances.
- Missing ground, zero/negative/nonfinite resistance where unsupported, conflicting
  sources, floating ADC input, source-impedance limitation, unsupported component,
  rail overvoltage and failed solver each identify the cause without fake readings.
- Sweep source voltage through digital thresholds and ADC clipping/attenuation
  boundaries; inspector and traces agree with the same timestamped engine events.
- Continuous ADC recording declares sample rate, channel ordering, lost samples and
  decimation. Paused/stopped and stale readings cannot look live.
- Physical voltage/sample comparison needs a recorded source/reference measurement;
  serial logs alone are not claimed to measure real pad voltage.

## UX-08: problems and recovery

Use structured, searchable problems with severity, category, affected component/net,
failed stage, session/time and recovery action. Inline field errors link to their
fields; broader runtime faults appear in a Problems pane and persistent status.
Keep raw diagnostics available without requiring users to read implementation logs
for routine recovery. Do not inject simulator diagnostics into guest UART output.

Acceptance and failure checks:

- Missing QEMU/device executable, malformed project, duplicate endpoint, bus timeout,
  unsupported analog topology and device crash have specific Edit/Retry/Locate/Open
  evidence actions as applicable.
- Retry is bounded, cancel works, and failure does not destroy the prior project or
  reconnect a rejected graph. Repeated faults coalesce without hiding occurrence count.
- Error focus moves to a useful target and returns after dismissal; no popup/focus
  trap. Severity is communicated in text/icon, independent of theme or color.

## UX-09: trace, logic analysis and ADC plots

Provide a Signals pane with virtual-time digital lanes, analog voltage traces and
ADC raw/calibrated samples. Bus decode includes controller, endpoint, transaction
ID, TX/RX, ACK/NACK, CS/mode, repeated start, duration and error. Link selected events
to canvas nets/components and support triggers, cursors, search and export.

Retain session/reset identity, sequence order, units and recording fidelity. Bound
buffers; explicitly report drop/overflow/backpressure. UI coalescing/plot decimation
must be separate from recorded-data loss. Page large captures rather than appending
millions of text lines. Mark analog interpolation and unavailable channels.

Acceptance and failure checks:

- Known firmware GPIO/RMT/bus/ADC fixtures yield ordered timestamped traces; decode
  selection highlights the correct net and device.
- Rate pressure, ring wrap, disconnect and reset preserve loss markers and session
  boundaries. Export is reproducible with runtime/project/fixture identity.
- Keyboard channel/cursor/transaction navigation and numeric/tabular equivalent
  expose meaningful data in both themes without requiring color discrimination.
- Agree performance budgets and reference hardware/capture sizes before optimizing;
  measure interaction latency and memory with those captures rather than asserting
  an unmeasured component/sample count limit.

## UX-10: debugger, SIMD and internal chip status

Consolidate execution/debug actions and separate configured GDB settings from an
active endpoint. Display acknowledged breakpoint/step results. Registers and memory
show selected core, snapshot time and fresh/stale/unavailable status. Include reset,
interrupt, clock/power and chip/eFuse identity views as backend qualification permits.

Display true 128-bit SIMD Q registers and configurable lane interpretations only
when observable backend support exists. Keep floating-point F registers distinct.
Guest stepping/debugger wait must not be described as a successful firmware boot.

Acceptance and failure checks:

- Debugger disconnect, invalid address, failed command and stale snapshot are
  visible; requested breakpoint is not shown as installed before acknowledgement.
- Ordinary scalar/PC evidence remains available even if vector/clock data is absent.
- Real stepping/breakpoint and SIMD register fixtures are prerequisite to enabling
  their controls as verified. Keyboard/table inspection works in both themes.

## UX-11: peripheral and radio interaction

Use model capabilities to reveal appropriate device panels:

| Path | Planned user-visible controls/observations |
| --- | --- |
| UART | Text/hex bytes, terminal line endings, timestamps/search/export; distinguish host transport from guest framing/electrical mode |
| I2C/SPI | Endpoint/address/CS, mode, returned data, missing ACK and timeout/clock-stretch diagnostics |
| GPIO/RMT | External input/button events, driver/pull/level state, pulse/edge waveform |
| I2S | Audio source/sink, channels/format/rate, buffer occupancy, underrun/overrun |
| LCD/camera | Authentic frame view, format/resolution/frame/time, dropped frames; supported source injection |
| Analog/ADC | Voltage/resistance/source controls and actual conversion observations from UX-07 |
| Wi-Fi | Virtual AP/peer catalogue, channel/signal, connectivity/failure injection and packet capture |
| BLE | Advertising peers, connection/GATT/notifications and security state; unsupported pairing remains visible |

Acceptance and failure checks:

- Device process Running is not used as proof the guest can communicate with it.
- UI handles missing schema, version mismatch, dropped stream and disconnected
  firmware bridge; controls requiring unavailable model features are disabled.
- Each promoted panel has a normal-firmware fixture exercising meaningful data and
  a failure case; radio security, audio/frame delivery and SPI MISO do not inherit
  support merely from a working GUI or host-side Python model.
- Dynamic controls use theme tokens, accessible labels and keyboard actions.

## UX-12: accessibility and scalable use

Use visible labels, keyboard shortcuts with discoverable descriptions, logical
focus order and clear focus rings. Provide accessible names/roles/actions/state for
canvas objects, or a full equivalent model-backed table workflow while custom
graphics semantics mature. Standard Qt widgets provide accessibility metadata;
custom controls must expose it. [S6][S7] Follow Carbon keyboard, contrast and
non-color guidance as design input; do not claim Qt application accessibility from
React/browser tests. [S8]

Acceptance and failure checks:

- Complete New/Open/import/connect/analog-edit/run/pause/inspect/save/reopen flows
  with keyboard only. Shortcuts do not intercept text entry unexpectedly.
- At larger font/scaling settings, controls and essential error actions remain
  reachable. Support reduced motion and avoid flashing trace/status decoration.
- Screen reader announces components, connections, value units, state changes and
  errors; log/trace announcements are throttled to avoid constant speech.
- Verify assistive-tool support on the actual Linux/WSLg deployment and document
  any platform limitation; do not assume Windows screen readers can access WSLg
  Qt applications identically to native Windows applications.

## UX-13: local COM5 reference capture and comparison

Provide a separate Reference board workflow with selectable port (COM5 when it is
actually present), board identity supplied/confirmed from detection and fixture
selection. The Windows/WSL2 runner adapter must resolve actual access; do not
hardcode a fictional Linux path for COM5. Mere board attachment does not start a
job. Preview requested operations: passive capture, identification, fixture flash,
reset or measurement stimulus, with exact artifact/build and expected state changes.

Serial open can toggle reset-related DTR/RTS on some boards/drivers; distinguish
capture intended to avoid reset from fixture jobs that intentionally flash/reset.
Espressif documents reset behavior and explicit reset-mode controls. [S10] The
runner must report what it actually did, rather than calling every serial operation
read-only. Flash/reset jobs follow the user's authorized operation scope.

Record outputs locally by default. The capture preview identifies files and data
classes: firmware serial logs, chip identifiers, calibration data, traces and network
traffic. Do not silently upload captures or expose Wi-Fi credentials. Redaction and
identifier policy are explicit in export; preserve original evidence locally when
appropriate and label redacted derivative artifacts. Board calibration parameters
used by simulation are separate from original board dumps.

Acceptance and failure checks:

- Absent/busy COM5, USB disconnect, permission failure, unexpected board identity,
  flash verification failure and cancellation have recoverable job states.
- A device unplugged mid-capture cannot produce a successful/completed evidence
  result. Partial capture is marked partial with last confirmed stage.
- Capture and emulation may run separately; unavailable hardware does not block
  simulator editing/builds. Job operation preview and cancellation work by keyboard.
- Comparison shows fixture/hash, board/engine/version/calibration context, units,
  tolerance and aligned timestamps. Failure means the checked property failed,
  never automatic proof an entire peripheral is unsupported/supported.
- ADC reference jobs include physical source/reference measurement metadata and
  required instrumentation; serial samples alone do not establish voltage accuracy.
- Export preview/redaction is usable in both themes and preserves evidence identity.

## UX-14: recent ESP-IDF compatibility and evidence surface

Display a filterable matrix by exact ESP-IDF release/commit, Arduino core version,
board/module configuration, flash/PSRAM, engine/model revision and feature fixture.
Show passed, failed, not tested and unavailable with the linked reproducible
evidence. Separate boot compatibility from subsystem qualification and from timing
or electrical fidelity. Normal unmodified framework drivers are the default
qualification path; special firmware hooks must be identified as such.

The current 6.2-dev fixture is one baseline, not broad recent-IDF compatibility.
The central qualification plan chooses and pins recent release versions; the UI
consumes its manifest rather than assuming a changing `latest` label. Hardware
emulation implementation, not a patched framework, is the compatibility strategy.

Acceptance and failure checks:

- A single passing boot fixture never renders Full support for its framework.
- Update/failure/stale evidence is visible; failed and skipped cases cannot merge
  into a passed summary. Matrix filters and links work with keyboard and both themes.
- Imported firmware metadata that cannot identify its framework is shown as
  unknown; users may supply declared build context without it becoming verified.
- Capability promotion cites fixture, expected output, failure case and reference
  source/capture. Experimental models remain experimental until those gates pass.

## Plan verification and independent review

Before implementation, reviewers must verify the following against the consolidated
development plan and engine/electrical workstream documents:

- All user goals map to a stable UX ID and dependent engine/evidence gate.
- Analog/ADC is included in document schema, catalogue, inspector, solver faults,
  traces and hardware reference workflow, not just one ADC parameter widget.
- Every journey covers both themes, keyboard operation, failure recovery and
  persistence; advanced panels are conditioned on observable engine capabilities.
- Shared schema/state changes have one owner; agents do not concurrently rewrite
  `MainWindow`, `BoardWorkspace`, project parsing or CMake without coordination.
- Tests cover model behavior and real backend/fault outcomes. Visual snapshots
  alone do not prove electrical routing, accessibility or firmware compatibility.
- Qt version/platform appearance and accessibility limitations are explicit;
  Carbon alignment is reviewed across behavior and design, never claimed solely
  from copying colors into QSS.

## Primary sources

- **S1:** [Carbon themes and system preferences](https://www.carbondesignsystem.com/building-blocks/foundations/themes/code)
- **S2:** [Carbon semantic color tokens](https://www.carbondesignsystem.com/building-blocks/foundations/color/tokens)
- **S3:** [Qt QStyleHints: colorScheme version and palette-change behavior](https://doc.qt.io/qt-6/qstylehints.html)
- **S4:** [Qt QSettings persistence](https://doc.qt.io/qt-6/qsettings.html)
- **S5:** [Qt QUndoStack commands and clean state](https://doc.qt.io/qt-6/qundostack.html)
- **S6:** [Qt accessibility overview](https://doc.qt.io/qt-6/accessible.html)
- **S7:** [Qt QAccessibleInterface semantics](https://doc.qt.io/qt-6/qaccessibleinterface.html)
- **S8:** [Carbon accessibility overview](https://www.carbondesignsystem.com/building-blocks/foundations/accessibility/overview), [keyboard guidance](https://carbondesignsystem.com/guidelines/accessibility/keyboard/), [forms](https://www.carbondesignsystem.com/building-blocks/core/patterns/forms)
- **S9:** [ESP32-S3 ADC overview, driver modes and calibration](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/adc/index.html), [ESP32-S3 Technical Reference Manual](https://documentation.espressif.com/esp32-s3_technical_reference_manual_en.pdf)
- **S10:** [Espressif esptool reset options](https://docs.espressif.com/projects/esptool/en/latest/esp32/esptool/advanced-options.html), [boot-mode/reset circuitry](https://docs.espressif.com/projects/esptool/en/latest/esp32/advanced-topics/boot-mode-selection.html)

Links to `stable`, `latest` or unversioned documentation are discovery sources.
Implementation/qualification records must pin the applicable versions and chip
documentation revisions; they must not rely on those aliases remaining unchanged.
