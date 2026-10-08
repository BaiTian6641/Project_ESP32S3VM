# Native v3 electrical runtime

This GPL-2.0-or-later lane belongs to the **separate QEMU executable**. None of
`net-dc`, `net-adapter` or `net-rc` is linked into the Qt editor. Its input is the
existing complete [project v3 document](../../../docs/contracts/project.schema.json),
not an address decoder, second controller format, guest MMIO device, or firmware
hook. Native qualification is limited to the paths and binary recorded in
`build-runtime-state/electrical-runtime-2026-10-07/`; implementation availability
is not a silicon-characterization or physical-board claim.

## Apply and observations

The S3 machine creates `/machine/soc/electrical` after GPIO and RTC-IO are
realized, installing the typed ADC provider link **before SENS realization**.
QMP `qom-set` of the **string** property `project-json` submits the whole v3 JSON.
It accepts only paused/prelaunch (initializing), quiescent boundaries.
QMP acknowledgement is the commit boundary; validation or settlement failure
leaves the previous accepted document, topology, charge and timers intact.
`qom-get project-json` returns the accepted pure document, including extensions.
There is no process restart, automatic resume, or connection inferred from geometry.

`qom-get snapshot-json` returns a JSON string with ABI 1, implemented support,
`generation`, `source_generation`, last virtual `timestamp_ns`, status/diagnostic,
net voltages and floating flags, GPIO terminal voltages with analog `valid` and
separate `digital_valid`, signed primitive currents and original-equation residuals.
The generation counters and `timestamp_ns` are lossless decimal **strings**,
not JSON double-valued counters (Qt cannot preserve integers above 2^53).
Unknown voltages/currents are JSON null, never fabricated zero. `dc_factorizations`
makes source-only LU reuse observable. `generation` changes only after a committed
electrical change; display names, component/net geometry, document/artifact paths,
opaque extension data and array order do not change electrical identity. Stable
component/terminal/net IDs, roles, domains, directions, GPIO identity, quantities
and runtime electrical mode, module and explicit reservation profile do.
Electrical quantities are normalized to SI; free-form roles use length-prefixed
identity encoding, so punctuation cannot disguise a topology edit as a no-op.

## Explicit profile and supported graph

Declare the selected simulator model in the existing v3 runtime extension:

```json
{"electrical":{"driver_profile":"s3-explicit-finite-v1","mode":"dc"}}
```

This is the value of `runtime`, not an alternative project payload. Exactly one
MCU is required. Its signals are actual terminal IDs with `gpio` 0..48 except
unpopulated GPIO22..25 and digital/analog domain. Its explicit power terminals
are domain `power`, role `vdd` and domain `ground`, role `gnd`. Both must be wired
through `nets[].endpoints`; `gnd` must reach an explicit ground component's `ref`.
There is no inferred supply, reference, leakage, external pull, or v2 migration
wiring. The selected finite profile uses 40 ohm output resistance, 45 kohm for
**enabled native firmware** weak pulls, VIL=0.25 VDD and VIH=0.75 VDD. These are
declared simulator-profile values, **not** measured S3 drive-strength impedances.
FUN_DRV is observed but does not change this profile.

The strict graph adapter supports explicit grounds, resistors, voltage/current
sources, finite switches (closed requires `on_resistance`), potentiometers, and
MCU pads. Sources preserve declared polarity and finite series/shunt impedance.
Incomplete wiring is not hidden wiring: disconnected component terminals use
private nodes; named empty nets and inactive unwired MCU pads consume no solver
node. All 45 populated physical MCU pads remain solver-owned even when omitted
from the project. An actual native driver or pull allocates a private intrinsic
pad node against the explicitly wired MCU rails (40 ohm driver, 45 kohm pull);
it creates no external connectivity or inferred power. With neither source nor
pull, an enabled receiver is genuinely unknown, not valid zero. Disabled
receivers and non-populated GPIO22..25 retain the GPIO device's documented zero.
`pads` contains declared stable terminal IDs only. `physical_pads` separately
reports all 45 populated physical pads' actual voltage/validity/diagnostics and
current native receiver/mux/output-enable/pull controls; omitted terminals have
no invented ID. `sleep_configured` reports configuration, not active sleep.
The readonly snapshot remains available on a paused dependency for diagnosis.
The electrical profile reads the real RTC_CNTL `sleep_active` request through
the native GPIO getter. Programming SLP_SEL/SLP_* while awake does not enter
sleep or replace FUN_* drives. Actual sleep and hold requests remain strict
unsupported dependencies for this profile (`support.active_sleep` and
`support.hold` are false), rather than clamped samples or invented idle levels.
Generic device/LED nonlinear behavior,
controlled sources, inductors, AC/RF networks, unregistered peripheral/direct-
IO_MUX output providers, hold/sleep behavior and opaque behavioral quantities are rejected or
reported as unavailable, never silently dropped. Runtime matching addresses and
legacy decoder metadata cannot connect a terminal or inject a sample.

Native drive state is read from GPIO/IOMUX/RTC registers, not the resolved output
lines: mux ownership, data inversion, actual OE, open-drain and pull requests.
A low/high push-pull request inserts the profile's finite branch to the **actual
project ground/supply node**; open-drain release omits that output branch. Enabled
pulls are additional finite branches. Two conflicting output drivers therefore
resolve by KCL and publish real branch currents; an indeterminate midpoint is
not promoted to a valid logic sample.

Once a graph is applied, solver-provided voltage/validity owns pad sampling.
Missing pads, floating nets, threshold-gap and unpowered samples remain explicit
unknown dependencies. Strict GPIO and SENS consumers stop before fabricated
input/IRQ/ADC data is consumed. Fixing/applying a graph does not resume the VM.
GPIO raw-change notifications exclude solver sample updates; settlement bounds
reentrant delta cycles at 16 and reports oscillation instead of recursion/hang.

## RC and reset policy

Select `mode: "rc"` and explicitly declare `edit_charge: "keep"` or `"reset"`.
Capacitors require positive F/uF/nF/pF `capacitance` and explicit V/mV
`initial_voltage`. RC threshold scheduling requires an explicitly connected,
fixed ideal MCU VDD-to-ground source; moving supply thresholds are not qualified
by this profile. Numerical tolerances are 1 microvolt absolute and 1e-5 relative,
with bounded kernel work/rejection budgets, 1 ns crossing bracket and at most 64
crossing bisections. The next event is the earliest watched low/high crossing or
100 microsecond sample horizon, quantized upward to a virtual nanosecond. Crossing
prediction uses a reusable probe; it does not advance the live circuit ahead of
the VM. ADC samples advance the actual solver to the requested virtual aperture.

Source transitions keep charge. On topology Apply, `keep` remaps capacitor charge
by stable **component plus polarized p/n terminal IDs**, not incidental component
array order or projection index. New/identity-changed capacitors use declared
initial voltage; `reset` resets charge only on a real electrical topology edit.
No-op/rename/layout Apply preserves charge even with `edit_charge: "reset"`.

CPU-only reset preserves controller and graph state. Chip/peripheral/RTC reset
may change MCU register-derived sources under each controller's real reset domain,
but is **not an external circuit power cycle**. The external electrical device
has no MCU-reset callback that discharges capacitors. Charge changes only by
actual modeled circuit evolution, a real topology edit's declared policy, or
explicit power/source changes. Timers/IRQs owned by GPIO/ADC follow those models'
SOC reset-domain hooks; external sampling is replanned on actual source changes.
Reset-domain selection belongs to one complete QEMU reset transaction: it must
remain stable through every device hold callback, and must not leak a prior
CPU-only request into a later full chip reset. GPIO output enable must clear on
that full reset while the external capacitor voltage remains continuous.

## Shared reusable workspace

`en_dc_workspace_create/solve/destroy` allocates a bounded workspace once.
Source-only changes reuse row-equilibrated LU factors when the matrix-defining
endpoints/kinds/resistances are unchanged. Every solve still checks external
current balance for **each floating island** and the original KCL/source equations.
The legacy `en_dc_solve` API remains available and owns a one-call workspace.
`en_adpt_project` validates IDs/topology/quantities without allocation or solving;
its node/ownership tables let native firmware transitions avoid repeat graph
resolution. ABI 2 input struct sizes and existing status meanings are preserved.
The RC implementation must be the corrected, independently reviewed kernel,
not the superseded version with known floating-current/edit/crossing defects.

## ADC provider

The same electrical object implements `esp32s3-adc-sample-provider`; SENS links
`sample-provider` to it. ADC1 channels 0..9 map to GPIO1..10; ADC2 to GPIO11..20.
A valid analog sample is the solved voltage at the requested virtual time,
including an omitted physical pad resolved by its actual native driver/pull.
Unresolved, floating, unpowered and digitally driven pads return explicit validity.
Digital FUN_IE is not an analog-ownership
gate. ADC conversion/scaling and fail-closed completion are the SENS model's
responsibility; no sample codes are injected by this device.

## Explicit I2C model registry

Only native `kind: "device"` models with exact `type: "sht21"` or `"24c02"`
are registered. They require four real terminals: `sda`/`scl`, digital/inout;
`vdd`, power/input; and `gnd`, ground/input. The only accepted parameter is
`address`, integer count 1..127. This is the existing v3 quantity format, not an
address-routing controller. Unknown models remain unsupported.

The explicit ideal-idle registry profile declares high-Z released lines and zero
static rail load; a native service's asserted SDA/SCL contributes 40 ohm to that
model's **own wired GND**. Actual SDA and SCL physical-net reachability precedes
address lookup. Same-address unreachable devices cannot ACK; duplicate reachable
addresses report collision. Power comes from solved model VDD minus GND:
SHT21 2.1..3.6 V, 24C02 2.5..5.5 V. Unknown supply is an explicit dependency.
Series-resistor-split bus route equivalence is not supported by this direct-net
service profile, rather than guessed from geometry or conductive island names.

The public `esp32s3_electrical.h` bridge samples solved lines with validity and
registers genuine mux-selected native controller output signals. It never injects
GPIO input bits. Bounded multicast delivers every actual solved/published frame
to registered controllers/services, including native waveform edges and route
changes. Callers revalidate physical state, never cancel healthy traffic merely
because notified. Reentrant drives queue the next delta; explicit output batches
publish coherently and settle synchronously at the outer boundary.
Device output requests survive no-op/geometry/rename Apply by stable
model/component/terminal identity. Stateful
SHT21 conversion/CRC/environment and EEPROM write-cycle/data behavior belong to
the separate native I2C lane; their qualification requires its end-to-end evidence,
not this graph registry's existence.

`terminal_voltage` exposes absolute physical voltage/validity independently of
device power: a released, unpowered output can be externally pulled to a known
3.3 V. It never invents a logic reference from zero-volt rails. Operational
`terminal_sample` separately requires the device's actual powered threshold
domain before consumption. Power availability epochs/timestamps come only from
committed solver samples, not a first matching address.

Additional exact registered types are `spi-nor-1m`, `i2s-sample-peer`,
`i2c-scripted-master`, `ws2812-functional-3v3`, `nec-envelope-source`,
`st7789-i80`, `rgb-panel` and `ov2640-dvp`. Approved complex model configs use
the existing v3 component attributes; all require their real registered service
factory preflight before Apply. No generic DEVICE/high-Z fallback exists.

## Packaging and evidence

The electrical patch is additive to official base
`40edccac415693c5130f91c01d84176ae6008566` through the final parent's explicitly
ordered runtime profile. The accepted CPU15 patch follows clock14; GPIO
ownership/test corrections must not be overwritten by superseded copy snapshots.
Canonical ADC copies precede the electrical nine-copy/six-kernel boundary.
`source-map.json` records the interfaces; the final parent receipt records exact
ordered inputs and installed hashes. Use only that content-addressed source and
write only its `build-runtime`, never the pinned base or historical lane trees.
BUILD_JOBS=4. Native tests use QTest-visible
pad/IRQ/provider states and ordinary IDF 6.1 firmware, not source-text checks,
mock decoder echoes, or private code injection. Evidence reports supported range
and remaining gates separately; no native GPIO/ADC/RC claim without its exercised
end-to-end path.
Native graph tests start paused for Apply, then explicitly continue before
conversion or RC timer advancement. A stopped machine is not an ADC completion;
tests retain the real DONE/code and charge assertions, and stop again before
changed graph Apply.

The current coherent raw-input receipt is `source-freeze.json`; canonical
parent preparation owns the content-addressed full source and its verifier
owns `build-runtime`. Historical lane trees are preserved, not relinked into a
claimed new binary. **SOURCE_COPY_FREEZE is not BINARY_READY or qualification.**
