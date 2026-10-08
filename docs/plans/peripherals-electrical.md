# Peripheral and electrical implementation workstream — 2026-10-07

Status: **planned work, audited against source; no new capability is claimed**.
This document expands [the development plan](../development-plan.md). Its work
IDs are stable references for dependencies, acceptance evidence and parallel-agent
ownership. Preserve the existing source and uncommitted product changes while
building the new official-base patch series.

## 1. Required outcome and fidelity boundaries

Ordinary ESP-IDF and Arduino firmware must exercise actual ESP32-S3 registers,
interrupts, memory and signal paths. A component connected to a board terminal
responds because the circuit net and firmware pin routing connect it, not because
a GUI controller/address assignment bypasses the pins. Firmware must not require
JSON logging, replacement drivers, symbol interception or simulator-only bus APIs.

The first release includes **analog voltages and ADC behavior**, as requested by
the user, as well as digital nets. The bounded analog component set includes
rails/ground, voltage/current sources, source impedance, resistors, potentiometers,
finite-resistance switches, capacitors and GPIO driver impedance. Implement DC and
linear RC transient behavior with a published accuracy and convergence contract.
Arbitrary nonlinear semiconductor, op-amp, inductive or RF circuits require later
models and must receive an explicit unsupported-topology diagnostic.

Peripheral timing follows the modeled peripheral clock and documented state
machine. CPU instruction execution is not claimed to be physically cycle accurate.
QEMU's instruction-count time is a scheduling tool, not a model of the LX7 pipeline.
[QEMU instruction counting](https://www.qemu.org/docs/master/devel/tcg-icount.html)

Capability maturity is per feature/profile: catalogued, implemented, native-tested,
hardware-compared, qualified. Availability and its reason are separate fields;
register tests are evidence rather than a competing maturity label. Do not convert
a successful driver
initialization, host Python smoke test or artificial DONE flag into full support.

## 2. Audited baseline and source gaps

The authoritative recorded acceptance baseline remains
[current-status.md](../current-status.md). Research inspected the recovered QEMU
gitlink `33c2bdd17104b3baeea3b788bafd27b2f921c13f`, not an assumed complete official
implementation. Port against official Espressif release
`esp-develop-9.2.2-20260417`, SHA
`40edccac415693c5130f91c01d84176ae6008566`, verified by the parent workstream.
The prior five-target integration run includes recovered-extension I2C; it is not
evidence of full peripheral support in the official binary.

Paths in this table are repository relative. Line numbers are investigation entry
points in the recovered revision and must be refreshed after porting.

| Source | Audited behavior | Required investigation/change |
| --- | --- | --- |
| `qemu/hw/gpio/esp32s3_gpio.c:55` | GPIO input is `OUT & ENABLE`; matrix selectors are stored; pending status directly raises one IRQ | Resolved external pad state, signal routing, edge/level detection, per-CPU interrupt masks |
| `qemu/hw/gpio/esp32s3_iomux.c` and its header | Per-pad register storage; uniform reset defaults; 49 indexed slots described as pads | Correct 45 physical GPIOs, holes, pad-specific functions/defaults and module reservations |
| `qemu/hw/xtensa/esp32s3_clk.c` | Clock gate/reset writes store bits without delivering their effects to peripheral models | Clock ports, gating, divider changes and reset-domain propagation |
| `qemu/hw/xtensa/esp32s3_intc.c` | Each source directly sets a CPU line; selector writes store mapping | Shared-source OR aggregation, asserted-source remapping, disabled mapping and bound tests |
| `qemu/hw/dma/esp_gdma.c` | Channel selection uses peripheral match OR START; batch helpers restart at link address without a persistent byte cursor | Correct selection/handshake, persistent channel FSM, bounded descriptor walking, partial bursts |
| `qemu/hw/xtensa/esp32s3.c:1027` | GDMA `soc_mr` links to the internal DRAM region | Audit reachable address spaces, PSRAM/EDMA and cache visibility |
| `qemu/hw/i2c/esp32s3_i2c.c:74` | RSTART ends the previous transfer internally | A passing cached sensor read does not prove repeated START without STOP |
| `qemu/hw/i2c/esp32s3_i2c_bridge.c` | stderr events; cached address/first-command-byte responses; sends always ACK; a response-map miss leaves prior response state | Stateful generic devices, per-byte ACK/NACK, current reads and explicit transaction phases |
| `qemu/hw/misc/esp32s3_gpspi.c` | Immediate completion; TX/rx-length stderr events; RX DMA receives the TX buffer; display DC samples raw low-bank GPIO_OUT | Actual MISO/CS/pin paths, phases/modes/bit length, timed completion and independent RX |
| `qemu/hw/char/esp32_uart.c` inherited by S3 | Immediate TX drain; fixed 40 MHz baud calculation with FIXME; coarse RX throttling; break TODO | Clock/framing/FIFO/flow-control timing and routed UART0/1/2 |
| `qemu/hw/timer/esp32s3_rmt.c` | Immediate TX completion; RX capture explicitly stubbed | Waveform execution/capture, clocked durations and DMA/IRQ behavior |
| `qemu/hw/misc/esp32s3_i2s.c:21` | 256-byte kick; TX payload discarded; RX zeros; immediate DONE | Streaming frames/samples, sample packing and persistent DMA |
| `qemu/hw/misc/esp32s3_lcd_cam.c:59` | LCD DMA drained/discarded; no proven sensor frame source; drain loop lacks an explicit bound | Pixel/sync streams, framebuffer ownership, camera RX and circular-chain hang tests |
| `qemu/hw/misc/esp32s3_sens.c` | SAR reports done on reads; temperature and touch return synthetic constants | Analog sampling/controller FSM and separately qualified temperature/touch |
| `qemu/hw/misc/esp32s3_apb_saradc.c` | Midscale data and immediate DONE on START | Real ADC conversion results, sample cadence, filters/monitors and GDMA |
| `qemu/hw/misc/esp32s3_rtc_io.c` | Explicit analog/deep-sleep register stub | RTC/analog mux, retention/hold and wake inputs |
| LEDC/PCNT/MCPWM/USB/ULP models | LEDC immediate overflow, PCNT no external pulse input, USB OTG/ULP explicit stubs; MCPWM needs waveform audit | Keep each in the SoC inventory; register presence is not functional support |

Suspected bugs need focused tests before correction; avoid changing masks or
interrupt encodings from comments alone. TRM, target register headers, IDF HAL and
hardware captures take precedence over old generated documents.

## 3. Common foundation tasks

### CORE-01 — Official-base audit and register contract

Dependencies: master-plan baseline/WSL build gate.

* Keep the recovered checkout intact. Diff each candidate model against the pinned
  official release, including parent classes, machine wiring and register headers.
* Inventory MMIO ranges, valid access widths, reset values, RO/W1C/W1TS/W1TC bits,
  state-machine effects, interrupt sources, DMA request IDs, clock/reset dependencies
  and implemented/unsupported features. Pin sources and TRM/errata versions.
* Separate actual silicon capabilities from framework driver support. No feature
  can be inferred from a generic IDF page for another ESP chip.
* Reproduce ROM/bootloader/normal app boot with the official base before and after
  each patch group; keep official-baseline and extension lanes distinct.

Acceptance: machine and model inventory is complete; every register touched by
qualified fixtures is mapped to a contract; unexpected accesses produce bounded
trace diagnostics; existing boot/QMP/pause/resume/reset tests stay green.

### CORE-02 — Clock tree and reset-domain propagation

Dependencies: CORE-01. Unblocks every timed model and analog/ADC sampling.

* Wire QEMU clock inputs/outputs for supported XTAL/PLL/APB/RTC/peripheral clocks.
  Clock gate/divider changes alter future progress; never merely change readback.
* Define fractional tick conversion, update latching and in-progress transfer
  behavior. Preserve sub-nanosecond clock precision until event scheduling.
* Implement peripheral soft reset, digital reset, CPU-specific reset, cold reset
  and supported RTC retention. Cancel/recompute timers and lower appropriate IRQs.
* Reset transactions, transport epoch, descriptor cursors and external devices
  according to their own reset/power terminals. A CPU reset is not automatically
  a power cycle of every connected component.

Acceptance: clock-gated transfers stop; divider changes produce expected periods;
reset at each transaction stage cancels or preserves documented state; watchdog,
software and CPU resets have distinct reasons/retention; no stale timer/IRQ fires.
[QEMU clocks](https://www.qemu.org/docs/master/devel/clocks.html),
[QEMU reset interface](https://www.qemu.org/docs/master/devel/reset.html)

### CORE-03 — Interrupt matrix and peripheral interrupt semantics

Dependencies: CORE-01; integrate with CORE-02.

* Track levels per source and recompute each CPU line as the OR of its routed
  active sources. Remapping an asserted source recomputes both old/new routes.
* Audit disabled mapping, valid bounds, edge/level types, CPU affinity, raw/masked
  status, W1C and level reassertion. Test boundary MMIO addresses explicitly.
* Firmware fixtures allocate interrupts through normal IDF APIs on both cores.

Acceptance: two shared sources cannot clear one another; mask/unmask/remap with a
pending source behaves correctly; clearing raw flags lowers lines only when no
source remains; reset clears defined state; qtests assert exact transitions.

### CORE-04 — Persistent GDMA and supported memory paths

Dependencies: CORE-01, CORE-02, CORE-03.

* Implement state per channel/direction: peripheral assignment, start/stop/restart,
  descriptor address, byte cursor, ownership, size/length/EOF flags, FIFO/handshake,
  interrupt state and bounded work quantum.
* Correct peripheral matching/start eligibility after checking the target contract.
  Use DMA memory access results and report errors without asserting host crashes.
* Support partial bursts across descriptors, writeback, EOF cadence and circular
  streaming. Bound zero-length/cyclic/malformed traversal without rejecting valid
  streaming rings; stop at a scheduled quantum instead of looping forever.
* Audit internal RAM, external RAM and EDMA paths separately. PSRAM addresses and
  cache coherency cannot be papered over by an internal-DRAM-only address space.
* Ensure channel allocation for one peripheral never consumes another's started
  channel. Qualify simultaneous channels and reset/clock changes.

Acceptance: multi-descriptor TX/RX, partial burst, owner errors, EOF/writeback,
stop/restart and ring streaming; five channels in each direction; two simultaneous
peripherals; invalid addresses, zero-length chain and circular stress never hang
QEMU or grow memory; supported RAM/PSRAM results agree with reference behavior.

### CORE-05 — Firmware qualification and failure evidence

Dependencies: CORE-01; used by every work package.

* Build ordinary drivers/framework fixtures with pinned toolchain/config/ROM/blob
  hashes. Test IDF latest patches in intended branches 5.3/5.4/5.5/6.0/6.1, dev 6.2
  as experimental, and 4.4/5.0/5.1/5.2 as extended compatibility. Re-resolve release
  tags at the baseline gate; do not infer patch versions from cached search text.
* IDF 4.4 is the documented S3 revision 0.1/0.2 historical support boundary. API
  migrations belong in fixtures; the emulator still implements hardware semantics.
* Required native integration lanes fail when a firmware image is absent. Skips
  may exist in local optional lanes but cannot produce a release support badge.
* Combine qtests/MMIO contracts with native firmware positive/error/reset cases.
  Driver-side input validation alone is not proof of underlying MMIO correctness.

Acceptance: machine-readable evidence identifies feature/range, source revision,
fixture, framework version, expected/observed results, trace/hash and hardware
comparison status. No unspecified skip is counted as a pass.
[IDF chip compatibility](https://github.com/espressif/esp-idf/blob/master/COMPATIBILITY.md)

## 4. Connectivity, scheduling and transport tasks

### NET-01 — One versioned netlist and terminal identity

Dependencies: CORE-01; coordinate the project-document/UX contract before edits.

* Stable component/terminal/net IDs survive rename, save-as, undo and reload.
  Power, ground, CS, DC/reset, SDA/SCL and ordinary GPIO are actual terminals.
* Nets contain endpoints; controller/address are decoding/device properties.
  Firmware's matrix is observed hardware configuration, not rewritten by the UI.
* Chip, package, module and board profiles determine available terminals and
  reservations. GPIO22–25 do not become physical pads because arrays have 49 slots.
* Validate legacy project migration without inventing missing rails/pullups.
  Distinguish connectivity errors from incomplete assumptions and unsupported
  component capabilities. Apply topology changes at a quiescent virtual boundary.

Acceptance: disconnecting a wire changes firmware behavior; moving a device to a
different disconnected net cannot respond through an address match; net sharing,
duplicate-address collision, save/undo/reload and topology generations are tested.

### NET-02 — IO MUX, GPIO matrix and RTC/analog routing

Dependencies: NET-01, CORE-02, CORE-03.

* Route internal inputs/outputs through real mux selectors, signal constants,
  inversions, output-enable source/inversion and pad configuration.
* Model digital input enable, push-pull/open-drain/high impedance, internal pulls,
  drive setting, pad hold and supported sleep/RTC ownership.
* Preserve direct IO MUX versus GPIO-matrix paths where documented differences
  matter. Analog selection disables/changes digital paths as documented.
* Expose effective firmware-selected routing to inspection; UI wire assignments
  must not override firmware to make a broken circuit appear functional.
* Integrate CPU-dedicated GPIO bundles with PIE/low-level GPIO instructions and
  actual matrix/pad routes. Bundle allocation, input/output masks, inversion and
  ownership belong to the selected CPU core; an instruction on the other core
  cannot silently operate that core's bundle.

Acceptance: route UART/SPI/RMT through different valid pads; invert input/output/OE;
fan one signal to two pads; move an input while active; sample held/sleep pads;
prove actual mux state determines signal and ADC ownership. Native dedicated-GPIO
bundles and PIE writes/reads must drive/sense wired pads on both cores, including
wrong-core operations and bundle release/reallocation. Register-only ISA vectors
are insufficient to qualify this electrical integration.
[GPIO/RTC GPIO](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/gpio.html)

### NET-03 — Resolved electrical pad state and GPIO IRQs

Dependencies: NET-02, the basic DC/pad-threshold subset of ANALOG-01; integrate
with CORE-03. This subset starts during M3 and is required at the M4 digital exit.
Full RC threshold-crossing evidence is additive in M4A and does not block M4.

* Resolve voltage and digital interpretation from all connected drivers/pulls.
  Open-drain release plus a pullup rises; missing pullups remain floating/unknown.
* Track low/high/high-impedance/floating/contention/indeterminate diagnostics,
  driver contributions, current and voltage under the chosen model.
* Use voltage thresholds from a documented/profiled S3 digital-input model. The
  unspecified interval is not a guaranteed threshold or a random Boolean.
* Guest registers still return ordinary bits: they cannot contain an `X`. In a
  strict diagnostic profile, pause before sampling an unresolved input; in a
  permissive profile, use an explicit persisted/seeded sampling rule and trace its
  provenance. Do not silently choose a universal zero, rail or hardware threshold.
* Edge/level detection and per-CPU IRQ masks use resolved input sampling. Reading
  GPIO_IN must not simply return the programmed output latch.
* Dedicated-GPIO readback must sample the resolved pad/net input, while output-latch
  reads remain a distinct operation. Test PIE-to-pad-to-input loopback, external
  driven input and software bit-banged UART/I2C/SPI with actual net routing; CPU
  affinity and matrix feedback cannot be hidden by copying output state to input.

Acceptance: native button/LED fixture, rising/falling/both/low/high interrupts,
input-disable, pulls, open drain, tri-state, two-driver contention and floating
pin; IRQ clearing while a level remains active follows the chip contract. At M4A,
add RC crossing edges at solver-derived times to the qualification evidence.
[Native dedicated GPIO](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/dedic_gpio.html)

### NET-04 — Virtual-time dependency barriers and deterministic host services

Dependencies for the M2 feasibility prototype: CORE-01, a minimal stable-ID net
fixture and the existing QEMU virtual clock. The prototype does not wait for the
new CORE-02 clock tree. Final CORE-02/03-integrated deterministic qualification
occurs in M3; NET-01 supplies the completed project graph at M4.

* Define negotiated lookahead/time horizons for external device processes. At an
  unresolved dependency, stop guest virtual progress at an explicit barrier while
  retaining an active QEMU Aio/main loop for QMP, shutdown and responses.
* Guest-visible responses have modeled virtual completion times. NACK, stretch,
  conversion-ready and timeout outcomes follow those times, not Python wall time.
* A separate bounded host watchdog reports a stalled/crashed service and pauses
  or errors the simulation. It must not arbitrarily inject a firmware bus timeout.
* Define stop/reset/clock change during barriers, dependency ordering, queue limits,
  multi-service deadlocks and stale-response disposal using epoch/generation IDs.
* Define an equal-time delta-cycle ordering: commit source/topology changes,
  propagate mux/driver/clock effects, settle the analog graph, evaluate pad
  thresholds, sample ADC/peripheral inputs, update peripheral state/DMA, then
  aggregate IRQs. Iterate zero-time consequences to a fixed point with a bounded
  delta count; trace/diagnose oscillating or contradictory same-time dependencies.
* Evaluate serialized TCG/icount with dual-core Xtensa and all timer models before
  choosing a deterministic default. Timestamped async traffic alone is insufficient.
* Scripted/barrier and replay modes accept only scheduled inputs. Interactive/live
  networking inputs are nondeterministic until recorded; live NAT packets may be
  scheduled at a recorded arrival boundary, but cannot be promised repeatable
  before that recording exists. Record or disable every external input source.

Acceptance: fast/slow/delayed host services produce identical guest memory/events
for the same input trace; intentionally stalled services remain controllable;
reset/reconnect discard obsolete responses; virtual timeout tests remain stable
under host CPU load; multi-service barriers have bounded deadlock diagnostics.

### NET-05 — Dedicated versioned bus transport and streaming contract

Dependencies: NET-04; coordinate master protocol specification.

* Use a separate chardev local stream for device events/responses. Do not mix bus
  messages into UART or log stderr. QMP handles control/inspection only.
* Frames carry version/capabilities, session/epoch/topology IDs, sequence, request
  ID, virtual timestamp, endpoint/net/controller, phase and explicit status.
* Preserve START/address/write/read/restart/STOP, ACK/NACK, bit length/order,
  mode/CS and payload length. Reads return actual device bytes before completion.
* Enforce negotiated frame/payload/in-flight/queue bounds. Specify fragmented and
  coalesced input parsing, disconnect, duplicate/out-of-order/late responses and
  partial accepted payload semantics. Logs are a separate channel.
* Add bounded binary stream batches/rings for audio/video after control correctness.
  Buffer ownership, backpressure, reconnect and sequence loss are explicit.

Acceptance: malformed/truncated/oversized frames, invalid IDs, unknown major version,
partial writes, model crash/restart and reset at every phase are handled without
corruption or unbounded growth; normal UART output stays untouched; native I2C and
SPI receive live bytes from stateful external models.

### NET-06 — Fast-path equivalence, trace and replay

Dependencies: NET-01..NET-05; each bus supplies its own equivalence fixtures.

* Transaction batching derives reachable devices and state from the same netlist,
  mux, driver and sampling model as edge execution. No second connectivity truth.
* Fall back to pin events for bit-banging, routing changes, ambiguous nets,
  contention or observers requiring waveforms. Define safe invalidation points.
* Record input seed, firmware/profile/model hashes, topology/epoch changes and
  nondeterministic inputs at virtual times. Trace net/pin/bus/IRQ/DMA/frame/error.
* UI redraw coalescing does not silently drop simulation data. Report trace loss.
  Replay independently of live Python models and compare final state hashes.
* Investigate native QEMU record/replay separately; do not claim whole-machine
  replay simply because a proprietary bus log can be reread.

Acceptance: edge and batched modes produce identical qualified firmware results,
including errors/reset and DC/CS sampling; replay reproduces memory, sample and
frame hashes; waveform trace retains ordering and uncertainty metadata.
[QEMU record/replay](https://www.qemu.org/docs/master/system/replay.html)

## 5. First-release analog kernel

### ANALOG-01 — DC component graph and nodal solution

Dependencies: a minimal NET-01-compatible graph contract. Implement the basic DC
solver and pad-threshold subset during M3/M4 so NET-03 can pass the digital M4
exit. Complete the analog catalogue, RC and ADC evidence in M4A. M4 never depends
on completion of M4A; M4A builds on the already working digital graph.

* Implement explicit units, common ground and VDD/board supply terminals; finite
  source/driver impedances, resistors, pots, switches and DC voltage/current sources.
* Modified nodal equations compute connected-node voltages and branch currents.
  Validate zero/negative/nonfinite parameters, source conflicts and singular nets.
* Report floating nodes, missing reference and unsupported topologies. Do not
  silently add a ground or hidden pull resistor just to force a numeric result.
* Supply profiles distinguish digital VDD and documented internal/reference domains;
  a voltage source at a pin is not an IDF API-return override.

Acceptance: resistor-divider ratios, loaded source, pot endpoints/midpoint, switch
open/closed, internal/external pulls and multi-source circuits agree with analytic
solutions within declared tolerance; singular/conflicting circuits fail clearly.

### ANALOG-02 — Deterministic linear RC transients

Dependencies: ANALOG-01, NET-04, CORE-02.

* Capacitor state persists across solver steps and appropriate topology/reset
  events. Specify initial conditions, integration method, maximum step, error
  bound and discontinuity handling.
* Choose a deterministic linear RC integrator after comparison with analytic
  exponentials and an independent reference. Backward Euler is a candidate;
  trapezoidal integration requires ringing/error analysis at discontinuities.
* Advance to peripheral sample times and digital threshold crossings. Steps cannot
  jump across a relevant sample/crossing without evaluating the event.
* Bound compute and convergence; explicit solver failure pauses/errors simulation
  rather than introducing arbitrary voltage or blocking QEMU indefinitely.
* Exercise stiff RC networks with widely separated time constants, near-zero
  resistance/capacitance and rapid topology changes. Specify minimum/maximum step,
  absolute/relative voltage error, step-rejection limit, iteration/work budget and
  interpolation/crossing accuracy. A rejected step cannot lose input events or
  silently advance time; underflow or exhausted budgets report a solver error.

Acceptance: RC charge/discharge, switched divider, source step and PWM+RC curves;
step refinement improves the declared error bound; identical replay reproduces
sample values/crossings; topology changes preserve or reset charge by contract.

### ANALOG-03 — Pad loading, thresholds and approximation profiles

Dependencies: ANALOG-01, ANALOG-02, NET-02.

* Model GPIO drive-strength impedance, leakage/pulls and ADC sample acquisition
  loading through explicitly parameterized approximations.
* S3 datasheet digital guaranteed limits define low/high interpretation. Treat the
  middle band as unspecified/indeterminate unless a separately measured threshold
  and hysteresis profile has been selected.
* Contention reports computed voltage/current under the model; it does not claim
  silicon damage simulation. Overvoltage and unpowered-input cases are diagnostics
  until specific clamp/power models exist.
* Distinguish ideal, datasheet-envelope and board-characterized profiles. Publish
  unsupported parameters/ranges and measurement uncertainty.

Acceptance: source impedance changes ADC settling; a heavily loaded output no
longer behaves as an ideal rail; open drain with an RC pullup gives a finite rise;
threshold crossings and unknown intervals appear consistently in inspector/trace.
[S3 electrical and ADC characteristics](https://documentation.espressif.com/esp32_s3_datasheet_en.pdf)

### ANALOG-04 — Model catalogue and solver evidence

Dependencies: ANALOG-01..ANALOG-03; coordinate UX terminal/property metadata.

* Catalogue rails, ground, resistors, pots, switches/buttons, caps and configurable
  waveform/sensor voltage sources with typed terminals and bounded properties.
* Archive analytic/reference vectors and solver version/tolerance with projects.
  Noise/parameter variation, if enabled, uses a persisted seed.
* Track later nonlinear/inductive/controlled-source models individually; do not
  present an arbitrary imported SPICE circuit as supported by the linear solver.

Acceptance: project save/load and replay retain units/component values/initial
states/seed; invalid numeric values are rejected; simple analog projects work
through real ADC registers and native firmware with no simulator-specific hooks.

## 6. ADC controller and native-driver tasks

### ADC-01 — ADC1/ADC2 oneshot through analog pads

Dependencies: CORE-02, NET-02, ANALOG-01..ANALOG-03.

* Verify ADC1 channels 0–9 on GPIO1–10 and ADC2 channels 0–9 on GPIO11–20 against
  the pinned target pin map; account for board use, USB pins 19/20 and analog mux.
* Implement acquisition/conversion/done timing, attenuation, supported 12-bit result
  width/output fields, controller ownership, clipping and clock/reset behavior.
* Evaluate solved voltage at sampling time. Voltage-to-code profiles include
  reference, offset/gain/nonlinearity and optional seeded noise.
* RTC SENS measurement state must stop reporting unconditional DONE and expose
  actual sampled result and documented invalid/interrupted flags.

Acceptance: ordinary IDF oneshot and Arduino analog read use resistor divider,
pot sweep, RC source, ground and clipped rail; multiple channels/attenuations;
reset/power/input ownership; floating and unsupported inputs diagnosed; exact
repeat with noise disabled and repeatable seeded variation when enabled.

### ADC-02 — Calibration eFuse and voltage conversion consistency

Dependencies: ADC-01, CORE-01; coordinate eFuse/chip profile.

* Implement consistent synthetic factory-calibration eFuse contents and conversion
  curves. Normal firmware performs the calibration calculation itself.
* S3 supports IDF curve-fitting calibration. Test valid and absent/invalid
  calibration data rather than intercepting calibration calls or returning mV.
* Hardware-characterized profiles store board reference/eFuse subset and tolerances;
  ideal profile cannot be advertised as physical chip accuracy.

Acceptance: `adc_cali_create_scheme_curve_fitting` and `adc_cali_raw_to_voltage`
produce appropriate native results across attenuation profiles; invalid eFuse
profile produces documented failure; calibrated sweeps agree within declared
profile tolerance and independent measurements when available.
[ADC calibration driver](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/adc/adc_calibration.html)

### ADC-03 — ADC1 continuous sampling and GDMA

Dependencies: ADC-01, CORE-04, NET-04.

* Implement conversion pattern table, sample cadence, channel/unit result tags,
  FIFO, DMA request/EOF and start/stop/restart. Advance analog state at sampling
  times, not once per GUI redraw or host response.
* Observe controller-clock/PM behavior and mutually exclusive oneshot/continuous
  ownership through actual registers; qualify normal IDF buffers/callbacks.
* Model overflow and driver-visible frame boundaries; never fill every RX buffer
  with an arbitrary fixed count regardless of descriptor capacity.

Acceptance: native multichannel stream matches expected order/data/sample count;
different buffers/descriptors, slow consumer overflow, stop/restart and reset;
clock divider/cadence and disabled-cache paths where supported; ring stress is
bounded and no sample is silently overwritten without documented status.

### ADC-04 — Filters, threshold monitors and interrupt state

Dependencies: ADC-03, CORE-03.

* Implement the two documented continuous-mode IIR filter instances and threshold
  monitors, coefficient/state update, channel binding, enable/reset and IRQs.
* Verify duplicated filter binding and raw-versus-filtered sampling/monitor order
  against the pinned TRM/HAL; do not infer this from API names alone.

Acceptance: step/ramp vectors match independent recurrence calculations; enable,
disable/reconfigure/reset preserves documented state; low/high threshold IRQs
occur at expected samples and W1C/masking works during continued conversion.
[ADC continuous driver](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/adc/adc_continuous.html)

### ADC-05 — ADC2 arbitration, silicon errata and radio interactions

Dependencies: ADC-01, CORE-01; radio workstream supplies native Wi-Fi/PWDET events.

The core ADC/analog release gate is independent of the radio implementation gate.
Qualify arbiter register/FSM behavior with scripted PWDET requests first. Native
Wi-Fi/PWDET integration is a later additive evidence gate; it cannot create a
circular dependency that blocks ADC oneshot/continuous implementation or delivery.

* Model S3 RTC ADC2/PWDET fixed/fair arbitration, configured priorities and
  interrupted/not-started result flags. Do not import original ESP32's blanket
  assumption that all ADC2 reads fail while Wi-Fi runs.
* Native IDF oneshot code includes protection for shared Wi-Fi use. Compare real
  chip behavior under scan/connect/traffic and idle; identify framework lock versus
  hardware-arbitration effects in the traces.
* ADC-183 affects S3 revisions 0.0/0.1/0.2: DIG ADC2 may enter an inoperative state;
  no fix is scheduled. Default capability rejects/limits ADC2 continuous DMA as
  the real supported firmware does. Forced legacy use gets an explicit erratum
  profile, not a promise of physically nonexistent stable DMA.
* Track RNG/ADC source interaction as a deterministic model profile; no claim of
  cryptographically secure randomness from a seeded simulator generator.

Acceptance: native ADC2 oneshot success/interruption/timeout cases are explained
by revision/controller/radio state; ADC1 continuous keeps its separate qualification;
ADC2 DMA restriction is visible; replay of radio arbitration produces identical
flags and samples.
[ADC oneshot limitations](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/adc/adc_oneshot.html),
[ADC-183 silicon erratum](https://docs.espressif.com/projects/esp-chip-errata/en/latest/esp32s3/03-errata-description/shared/sar-adc-adc2-not-work.html),
[TRM chapter39](https://documentation.espressif.com/esp32-s3_technical_reference_manual_en.pdf)

S3 has **no on-chip DAC**. Do not add ESP32/S2 DAC registers to satisfy generic
Arduino examples. External I2S/SPI DAC devices and PWM+RC are separate component
paths. The target capability header has no DAC capability; I2S's target table lists
no ADC/DAC mode for S3.
[Official S3 capabilities](https://raw.githubusercontent.com/espressif/esp-idf/v6.1/components/soc/esp32s3/include/soc/soc_caps.h),
[I2S target capability table](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/i2s.html)

## 7. UART, I2C and SPI

### UART-01 — Routed UART0/1/2 data and FIFO timing

Dependencies: CORE-02, CORE-03, NET-02..NET-05.

Implement actual TX/RX progression, FIFO thresholds, RX timeout and clock-source
divider behavior. Keep UART0 console separate from external UART components while
permitting explicit physical wiring. Do not erase the existing ROM boot/download
qualification while changing the inherited parent model.

Acceptance: IDF/Arduino full duplex peer and loopback on each controller, routing
changes, several baud rates, >FIFO transfers, threshold and timeout IRQs, empty/full
FIFO, absent peer, stop/reset and peer reconnect; byte streams and timing verified.

### UART-02 — Framing, flow control and protocol errors

Dependencies: UART-01, NET-06.

Cover documented data bits/parity/stop modes, break, parity/framing/overrun errors,
RTS/CTS and qualified RS485/IrDA/autobaud/AT-command features. Treat each advanced
mode as a separate evidence row instead of a single UART-supported flag.

Acceptance: normal driver receives deliberate wrong-framing/parity/break input;
CTS stalls/resumes without data loss; RX overload yields correct status; autobaud
uses actual input events in its qualified range; RS485 echo/collision is tested
through nets rather than forced API status.

### UART-03 — UHCI DMA and chip-specific memory behavior

Dependencies: UART-01, CORE-04.

Audit S3 shared UART FIFO-memory configuration and UHCI controller/register/DMA
paths. Qualify allocation, framed packets where supported, simultaneous UARTs and
reset/interrupt interaction separately from chardev console traffic.

Acceptance: native DMA transfers use correct controller/channel/descriptors and
completion/errors; concurrent UART traffic cannot consume another FIFO/channel.

### I2C-01 — Generic stateful native master transactions

Dependencies: CORE-02, CORE-03, NET-02..NET-05.

Replace cached first-byte response maps with a transaction state machine and
stateful device responses. Preserve START/address/write/read/restart/STOP and final
read NACK. Service FIFO refill/drain and both controllers. Return per-address and
per-byte ACK/NACK; clear stale data on misses/reset.

Acceptance: normal IDF master probe/transmit/receive/transmit-receive, > 32-byte
transfers, EEPROM register write/read and SHT21 timed conversions; sensor explicitly
distinguishes repeated START from STOP; absent address, data NACK and hot/reset
device cases; wrong/disconnected wiring prevents success despite matching address.

### I2C-02 — Open drain, timing, stretching and recovery

Dependencies: I2C-01, basic DC/pad thresholds and the per-I2C NET-06 equivalence
tests. Stretch/stuck-line/filter timing can qualify at M5 using the digital event
kernel; slow analog RC rise adds a separately tracked ANALOG-02/M4A evidence gate.

Use resolved SDA/SCL state, clock-source/divider, filter and timeout configuration.
Clock stretching and conversion-ready delays use virtual time. Missing pullups,
slow RC rise, stuck lines and arbitration must not be treated as cached reads.

Acceptance: supported 100/400 kHz native profiles, stretch below/above configured
limit, glitch filtering, missing pullup, SDA stuck low and bus recovery, two devices
on one net and address collision; compare waveform and firmware outcome in edge
and batched modes.

### I2C-03 — Slave, addressing and multiple-master envelope

Dependencies: I2C-02.

Qualify S3 slave registers/IRQ/FIFO,7/10-bit addressing where the target contract
supports it, general call and a second controller/external master sharing nets.
Implement or explicitly label multi-master/arbitration features from the hardware
contract; do not pretend two controllers are isolated because their IDs differ.

Acceptance: normal IDF slave callbacks exchange bytes with modeled master; address
and data-phase NACK, overflow and reset; two controllers on separate nets never
cross; shared-net arbitration and simultaneous-start cases follow the qualified
contract. [IDF I2C](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/i2c.html)

### SPI-01 — PIO transactions, MISO and CS over actual nets

Dependencies: CORE-02, CORE-03, NET-02..NET-05.

Implement SPI2/3 master with actual CS selection, command/address/dummy/data phases,
CPOL/CPHA modes 0–3, bit lengths/order, half/full duplex and independent RX bytes.
Sample display DC/reset through GPIO nets including high-bank pins. Use a
transaction FSM with clocked BUSY/DONE instead of self-clearing immediately.

Acceptance: native IDF PIO full duplex loopback, flash ID/status/read/write, two CS
devices, display commands, nonbyte payload lengths/MSB/LSB, keep-CS/setup/hold;
disconnected MISO/wrong CS, aborted/reset transfer and clock gate; exact RX and
waveform comparison. Slave data returned from an external model reaches firmware.

### SPI-02 — DMA, queued transfers and RAM/PSRAM paths

Dependencies: SPI-01, CORE-04.

Handle TX-only, RX-only and full duplex independently; continuous/queued payloads
cross descriptors without fixed 4096-byte truncation. Audit peripheral length,
buffer alignment, DMA direction/enable and documented half-duplex restrictions.
PSRAM DMA is a target/version-qualified path, not assumed internal RAM.

Acceptance: native polling and queued drivers transfer > 4096 bytes with exact TX/RX
hashes, multidescriptor boundaries, variable lengths, two devices and concurrent
peripheral DMA; wrong owner/address, RX shortage, abort and reset; qualified PSRAM
buffers and driver-supported temporary-copy cases.

### SPI-03 — Slave and multi-line advanced modes

Dependencies: SPI-02, NET-06.

Qualify SPI2/3 slave and S3-supported dual/quad/octal/DDR modes against capability
tables. External master clocks drive RX/TX and actual transaction-length reporting.
Mode support is a per-controller/range row, never inferred from SPI0/1 flash mode.

Acceptance: native slave queue/callbacks, shorter/longer master clocks, truncation,
CS abort, DMA and line ordering; multi-line flash/peer vectors and disconnected
data lanes. [SPI master](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/spi_master.html),
[SPI slave](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/spi_slave.html)

## 8. Waveform and streaming peripherals

### RMT-01 — Clocked TX and captured RX

Dependencies: CORE-02..CORE-04, NET-02..NET-06.

Execute symbol memory into pad waveforms with duration/level and end/idle state;
capture edges into RX symbols with configured input filters, timeout/thresholds,
memory boundaries and correct IRQs. Explicitly model active state and cancellation.

Acceptance: native IDF TX/RX loopback of exact symbol vectors, variable durations,
idle termination, RX filter/timeout/overflow, reset/disable midstream, IR/sensor and
WS2812 peer model; independent logic-analyzer reference for peripheral pulse timing.

### RMT-02 — Carrier, synchronization, rings/DMA and errata

Dependencies: RMT-01.

Add target-supported carrier modulation/demodulation, synchronized channels,
repeated/continuous TX, memory wrap and DMA. Review RMT silicon errata and IDF
workarounds for selected revisions before choosing end-marker/idle behavior.

Acceptance: waveform/carrier vectors, synchronized phase alignment, long DMA
payload, loop stop/restart and underflow; firmware workaround paths match revision
profile and do not signal immediate success without output.
[RMT driver](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/rmt.html)

### I2S-01 — Standard clocked sample streams

Dependencies: CORE-02..CORE-04, NET-02..NET-06.

Implement I2S0/1 standard Philips/MSB/PCM, mono/stereo slot packing, qualified
8/16/24/32-bit widths, TX/RX/full duplex/master/slave, clock-source/dividers and
FIFO-to-GDMA demand. External peers provide actual samples rather than zeros.

Acceptance: native IDF standard loopback/peer stream with exact samples and
BCLK/WS/frame cadence; circular/multidescriptor DMA, under/overflow, stop/restart,
clock source/divider updates and reset; audio presentation is separate from the
recorded sample stream and cannot hide missing samples.

### I2S-02 — S3-supported TDM and PDM modes

Dependencies: I2S-01.

Use target capabilities to qualify TDM slot mask/order and supported PDM TX/RX
converters/raw paths; no ESP32 internal ADC/DAC or I2S LCD/camera mode is invented
for S3. Each mode has its own sample-format and clock contract.

Acceptance: deterministic native TDM per-slot vectors and PDM bit/PCM conversion
vectors against independent/hardware references; DMA under/overflow and reset;
unsupported target/controller combinations report correct capability.
[I2S driver](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/i2s.html)

### LCD-01 — I80 command/data and displayed pixels

Dependencies: SPI-01 concepts, CORE-04, NET-02..NET-06.

Implement LCD_CAM I80 command/data, DC/CS/WR, bus width/bit ordering, pixel payload,
clock/phase and EOF/trans-done ordering. Device model consumes actual commands and
pixels; DMA drains may not discard data or spin without a work bound.

Acceptance: normal IDF `esp_lcd` I80 renders a known pattern with framebuffer hash;
command/data/pixel counts and DC routing, > 4096-byte transfers, queue/buffer
ownership, absent panel/wrong wire/underflow/reset; native callbacks occur after
the modeled transfer. [I80 LCD](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/lcd/i80_lcd.html)

### LCD-02 — RGB scanout, PSRAM and VSYNC buffer changes

Dependencies: LCD-01, qualified external-memory path in CORE-04.

Model HSYNC/VSYNC/DE/PCLK, porches/polarity and bus/pixel width independently.
S3 RGB routes through GPIO matrix. Cover internal and PSRAM framebuffer, double
buffer/VSYNC switch, bounce-buffer modes, refresh-on-demand, PCLK update and
restart-at-VSYNC. Timing/bandwidth starvation is an explicit model profile.

Acceptance: native IDF known frames/hash and sync/frame timings; buffer swap
without premature ownership, bounce callbacks, qualified PSRAM, starvation and
VSYNC recovery, invalid/missing sync/data nets and reset. UI skipped redraws do
not erase scanout frames from trace.
[RGB LCD](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/lcd/rgb_lcd.html)

### CAM-01 — Sensor configuration and DVP frame producer

Dependencies: I2C-01/02, CORE-04, NET-02..NET-06.

Model a concrete camera sensor's SCCB register behavior, reset/power/XCLK and DVP
pixel/HSYNC/VSYNC/PCLK output. Feed CAM RX via real sync/clock state and DMA;
provide deterministic test-pattern/image sources. No hidden direct framebuffer
injection may bypass the qualified sensor and controller paths.

Acceptance: unmodified official `esp32-camera` initializes/probes sensor through
SCCB and receives expected frame dimensions/payload/hash; disconnected/wrong
SCCB, missing XCLK/power/reset and missing sync fail meaningfully.

### CAM-02 — Formats, DMA ownership and frame errors

Dependencies: CAM-01, qualified PSRAM path where required.

Qualify raw RGB/YUV and sensor-produced JPEG payloads separately, byte/pixel
ordering, cropping/frame length, multi-buffer ownership and capture continuous/
single-frame behavior. Camera model encodes supported sensor formats; QEMU CAM
does not magically compress arbitrary pixels.

Acceptance: native buffers include exact tags/length/hashes, returned ownership,
multiframe capture and stop/restart; truncated frames, dropped sync, FIFO/DMA
overflow, slow consumer and reset; hardware capture compares valid frames and
error recovery. [Official camera component](https://github.com/espressif/esp32-camera)

Qualification checkpoint (2026-10-07): `qemu-extensions/prototypes/lcd-cam`
contains an authored unified LCD_CAM block, resolved-terminal ST7789/RGB sinks,
OV2640 SCCB/DVP service with real functional JPEG encoding, immutable QOM capture
windows, and ordinary IDF/locked official camera fixtures. These four packages
are **not native-qualified or complete**. Archived-prefix replay and historical
reference-only checks passed; neither proves LCD/CAM execution. The subsequent
independent bus/pixel-width RGB migration is unverified. NativeNet/typed-ADC,
SCCB binding and real LEDC-XCLK integration freezes, native builds, actual graph
captures, ordinary boot regression and post-cutover reviews remain required.
Qualified PSRAM CPU-cache/GDMA visibility is a separate CORE-04 prerequisite;
internal-RAM results must not be promoted to PSRAM or hardware evidence.

## 9. Remaining SoC inventory and explicit work IDs

These are required inventory rows for the full-chip ambition. They need their own
source contract and fixture before promotion; scheduling an implementation depends
on the master roadmap and user-facing release priorities.

| Work ID | Functionality | Minimum qualification/error gate |
| --- | --- | --- |
| SOC-01 | LEDC | Routed PWM period/duty/fade, shadow update, clock source, overflow/fade IRQ, disable/reset; PWM+RC analog fixture |
| SOC-02 | PCNT | External edge/control/gating, positive/negative counts, glitch filter, limit/watchpoint IRQ, stop/reset and overflow |
| SOC-03 | MCPWM | Two-group timer/operator/generator, compare/deadtime/sync/capture/fault/brake, routed waveforms and clock/reset |
| SOC-04 | General timers/system timer/watchdogs | Native alarms/counters, wrap, reload, clock gates, IRQ mask/clear, each reset/watchdog reason and retained state |
| SOC-05 | RTC, sleep/wakeup and power domains | RTC GPIO mux/hold, light/deep sleep, timer/ext wake, retained RTC RAM, CPU stall/reset, power/clock state and wake reason |
| SOC-06 | ULP-FSM and ULP-RISC-V | Actual supported ISA execution, RTC memory/I/O/ADC interaction, timer/wakeup and mutual exclusion; no register-only success |
| SOC-07 | Temperature and touch sensing | Configured temperature/code/calibration/ready; touch scan/filter/threshold/debounce/wakeup and revision errata; physical approximation profiles |
| SOC-08 | eFuse/identity and chip revision | Read layout/protection/revision/MAC/calibration consistency, virtual burn semantics only in explicit test profile, persistence and invalid-profile checks |
| SOC-09 | SPI0/1, flash/PSRAM/cache | ROM boot, IDs/modes, partition/NVS/OTA/persistence, QPI/OPI and memory mappings; erase/program errors; cache disable/coherency and DMA visibility |
| SOC-10 | AES/SHA/RSA/HMAC/DS/XTS/RNG | Native known-answer operations, FIFO/DMA/busy/IRQ/reset, key/eFuse linkage, fault/error behavior; seeded RNG clearly nonsecurity profile |
| SOC-11 | USB OTG and USB Serial/JTAG | Host/device enumeration, endpoints/transfer/IRQ/reset, descriptors and absent/disconnect/error; separate native USB from UART console capability |
| SOC-12 | TWAI | Native TX/RX, filters, arbitration/ACK, error counters/bus-off/recovery/loopback; net/transceiver model and bounded disconnect |
| SOC-13 | SD/MMC host | Native card init/register/command/data/DMA, storage persistence, CRC/timeout/remove and supported bus widths/modes |
| SOC-14 | Debug/PMS/world/cache/assist blocks | Verified memory/register inspection, break/step/watchpoints, access permissions/exceptions, CPU affinity and reset; no fabricated unavailable values |
| SOC-15 | Secure boot/flash encryption | Actual supported S3 ROM/eFuse/crypto boot behavior and failure vectors, encrypted flash image tooling and explicit QEMU limitations |
| SOC-16 | Sigma-delta output | Native SDM channel allocation/density/clock/inversion/enable-disable and waveform state; routed PDM pulses and measured mean/ripple through supported passive RC load; density extremes, resource exhaustion, clock gate/reset and connected ADC comparison |
| SOC-17 | Brownout detection | Configured VDD ramp/drop crosses profiled detector threshold, delay/hysteresis and enable state; reset/IRQ/reason/retention domains and recovery; uncertain physical threshold has a range/profile rather than invented precision; noisy/chattering rail, disabled detector and repeated brownout cases |
| SOC-18 | APB backup and RTC retention DMA | Actual descriptor/address/alignment, register-domain save/restore ordering and completion/error state for modem/APB backup and CPU/cache-tag retention; native sleep/wakeup restore, invalid descriptor/domain/capacity, reset mid-save/restore and bounded missing completion |

SOC-16 depends on CORE-02, NET-02/03 and the applicable ANALOG-02/ADC evidence;
the digital SDM waveform can qualify before its analog reconstruction. A passive
RC fixture fits the first-release solver, whereas active filters and LC/Class-D
circuits require later component models. Do not label SDM itself an on-chip SAR DAC.
[Native sigma-delta driver](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/sdm.html)

SOC-01/02/03/16 native prototype source and feature-granular register contracts
are published in [pulse/source-map.json](../../qemu-extensions/prototypes/pulse/source-map.json):
[LEDC](../../qemu-extensions/prototypes/pulse/copies/ledc-contract.json),
[PCNT](../../qemu-extensions/prototypes/pulse/pcnt-contract.json),
[both MCPWM groups](../../qemu-extensions/prototypes/pulse/copies/mcpwm-contract.json)
and [SDM](../../qemu-extensions/prototypes/pulse/sdm-contract.json).
This is not a generic “PWM supported” release flag; authored implementations
and tests do not satisfy the routed native/firmware/IRQ/boot qualification gates.

The explicitly named SDM functional waveform profile
`s3-sdm-second-order-transfer-v1` implements the documented second-order transfer
relation, not a first-order pulse accumulator or an averaged GPIO voltage.
Its independent mathematical oracle has certified bounded reachable orbits for
all 256 signed densities, with exact density on each certified orbit; native
physical pulse, passive-RC mean/ripple and connected-ADC comparisons remain
separate gates. The public QOM profile/assumptions disclose its quantizer tie,
zero-error density-change trajectory and retained clock/divider phase choices.
Exact S3 bitstream/reset qualification remains source-blocked: the
[TRM v1.8 §6.5.4](https://documentation.espressif.com/esp32-s3_technical_reference_manual_en.pdf)
and pinned SDK do not disclose hidden integrator widths, overflow behavior,
quantizer ties or startup/pipeline state. No silicon-bit-exact or physical
metrology claim follows from this functional profile, and SOC-16 full completion
remains open until its exact-converter/reference prerequisite and named gates pass.

SOC-17 integrates ANALOG-01/02 supply voltages with CORE-02 reset/power-domain state
and SOC-05 retention. SOC-18 adds backup/retention-engine semantics beyond generic
GDMA (CORE-04) and cross-checks SOC-05 save/wakeup paths. The pinned official S3
capability header declares dedicated GPIO, SDM, brownout reset, APB backup DMA,
RTC CPU retention and modem retention through backup DMA. These explicit blocks
must not disappear inside a generic "power" inventory row.
[Official IDF v6.1 S3 capability inventory](https://raw.githubusercontent.com/espressif/esp-idf/v6.1/components/soc/esp32s3/include/soc/soc_caps.h)

Wi-Fi, Bluetooth LE, coexistence, LX7/SIMD and related ISA/debug work are coordinated
with [radio-simd.md](radio-simd.md); they still depend on clock/reset/IRQ/memory
semantics and integrate ADC2/PWDET and peripheral stress cases here.

## 10. Chip/module/board restrictions and errata

* S3 GPIO numbering has holes 22–25. Its 45 physical GPIOs are not 49 freely wireable
  pads. GPIO0/3/45/46 have strapping roles; GPIO46's input/output restrictions must
  follow the pinned target contract. GPIO19/20 are USB-related on many boards.
* Flash/PSRAM reserve GPIO26–32 and additional GPIO33–37 for relevant octal/module
  profiles. Module and board terminals differ from naked-chip pads. Read exact
  module/board schematic before offering a pin as available.
* Store chip revision, flash/PSRAM type/size/voltage, module and board separately.
  User-facing warnings describe the actual profile reservation and consequence.
* Archive current TRM and errata version/hash at CORE-01. Research identified TRM
  v1.8 and datasheet v2.2; the browser could search the large TRM but could not fetch
  its complete PDF in one request. Baseline work must verify the full pinned file.
* Model documented errata that affect firmware semantics, including ADC-183 and
  relevant RMT/touch/GPIO issues. A workaround in new IDF may expose different
  accesses than older IDF; both can be valid chip behavior.

[Official S3 pin restrictions](https://github.com/espressif/esp-idf/blob/master/docs/en/api-reference/peripherals/gpio/esp32s3.inc),
[S3 datasheet](https://documentation.espressif.com/esp32_s3_datasheet_en.pdf),
[S3 chip errata](https://docs.espressif.com/projects/esp-chip-errata/en/latest/esp32s3/index.html),
[S3 TRM](https://documentation.espressif.com/esp32-s3_technical_reference_manual_en.pdf)

## 11. Hardware reference acquisition through COM5

### EHW-01 — Read-only identity and capture preparation

Dependencies: CORE-01. Parent already enumerated COM5 without opening it.

* Enumerate Windows port/USB metadata and record native USB versus USB-UART bridge,
  exact board/module, chip revision, flash/PSRAM and available terminals.
* Opening serial can toggle DTR/RTS and reset a board. Configure control-line
  behavior deliberately before reading an existing app's UART; do not assume
  read-only software means execution state cannot change.
* ROM/esptool identification and eFuse reads need ROM-download availability and
  may require resetting execution. Obtain an actual reachable read-only state;
  do not repeatedly force resets or infer identity from a COM number alone.
* Prepare exact fixture source/binary hashes, pin map, flash-offset list, capture
  script and backup/restore procedure before proposing flash/erase. No eFuse burns.

Acceptance: identity and permitted capture state recorded, fixtures reviewable,
reserved pins excluded, no firmware/flash/eFuse mutation in this preparation task.

### EHW-02 — Native digital and streaming reference vectors

Dependencies: EHW-01 plus concrete authorized reference-firmware action.

Collect native GPIO/IRQ/UART/I2C/SPI/GDMA/RMT/I2S/LCD/CAM result records and register
snapshots as applicable. Use a logic analyzer for independent peripheral pulse
timing; one-board loopback is useful functional evidence but not an independent
timing oracle. Record wiring, revision/toolchain, trigger alignment and uncertainty.

Acceptance: reproducible golden vectors per feature and failure case; simulator
comparison reports exact data differences and documented timing tolerance; missing
instruments/device hardware retain a visible hardware-qualification gap.

### EHW-03 — Analog/ADC metrology and profile fitting

Dependencies: EHW-01, ADC-01/02 and concrete authorized fixture action.

* Measure VDD and input with a DMM/calibrated source. Sweep in safe documented
  ranges with chosen attenuation/channel and record raw/calibrated values, noise,
  settling, source resistance and reference/eFuse calibration subset.
* Compare low/high source impedances and RC steps. PWM+RC can generate stimuli but
  is not a calibrated voltage reference without independent measurement.
* Characterize ADC2 with native Wi-Fi idle/scan/traffic and distinguish hardware
  arbitration from firmware locking. Do not attempt to qualify physically defective
  ADC2 DMA as stable merely because one trace happened to work.
* Keep synthetic ideal profiles separate from board-specific curves. Multiple
  boards are needed before claiming chip-population tolerance envelopes.

Acceptance: voltage-to-code/calibration and settling profiles have measured
uncertainty/range, seed/noise settings and board metadata. If no DMM/source exists,
analytic solver validation continues but hardware ADC accuracy remains unqualified.

## 12. Delivery slices, review and completion gates

1. M0: CORE-01/05 plus EHW-01 pin sources, inventory, baseline boot and framework
   lanes. M1 UX and M7 SIMD tooling can proceed in their own workstreams.
2. M2: NET-04/05 feasibility uses existing virtual clock and a minimal graph
   contract. M3 integrates CORE-02/03/04 and final scheduling qualification; basic
   ANALOG-01 DC/pad thresholds begin here alongside NET-02 endpoints.
3. M4: NET-01/02/03 and per-feature NET-06 establish real button/LED, shared/open-
   drain nets and DC/threshold behavior. M4A subsequently completes ANALOG-01/02/
   03/04 and ADC-01/02/03/04: voltage-divider, pot, RC and native ADC circuits.
   These analog capabilities are required for the first electrical release.
4. M5: UART-01/02, I2C-01/02 and SPI-01/02 plus core stress can proceed alongside
   M4A once M4 passes; analog-sensitive bus evidence integrates when ready. The first useful
   circuit release requires native bidirectional buses and ADC behavior through
   actual topology, with trace/replay and negative/reset checks.
5. RMT-01/02 plus SOC-01/02/03, then I2S-01/02. Advanced UART/I2C/SPI modes receive
   separate evidence rows rather than broad unsupported labels.
6. LCD-01/02, CAM-01/02 with qualified memory/buffering. Continue remaining SoC and
   radio/SIMD work according to master dependencies and feasibility gates.
7. Mixed-peripheral soak, every-stage reset/stop/error, edge/fast equivalence,
   compatibility and hardware comparison before any full-fidelity claim.

Each task reports: implemented range; passing positive/error/reset fixtures;
required framework versions; hardware reference status; known unsupported features;
trace/evidence IDs; and updated capability manifest. Independent review checks
coverage, dependency cycles, host-versus-virtual timeout behavior, same-topology
fast paths, analog approximations and realistic chip errata.

Meaningful verification uses qtests for MMIO/IRQ/virtual-clock edges and ordinary
firmware for end-to-end behavior. Add tests for actual new hardware semantics;
do not duplicate implementation constants as supposed independent reference data.
[Qtest framework](https://www.qemu.org/docs/master/devel/testing/qtest.html)
