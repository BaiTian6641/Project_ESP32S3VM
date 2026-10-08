# Bounded linear DC kernel — 2026-10-07

Status: ANALOG-01 standalone DC primitive plus the strict H-NET-01 graph adapter.
Neither is connected to QEMU pads, project Apply, native GPIO IRQs or ADC yet.
The source under `qemu-extensions/electrical` is GPL-2.0-or-later for the separate QEMU core.
No code was copied from ngspice. Its
[manual](https://ngspice.sourceforge.io/docs/ngspice-manual.pdf) is a reference for
circuit-analysis conventions, not a claim of SPICE compatibility.

## Input and supported scope

The adapter maps globally unique project terminal/net IDs to at most 64 active
solver nodes and 128 elements. Its node 0 represents only explicitly declared
ground groups; with no declared ground it remains unused by the projected graph.
Geometry never contributes connectivity. Empty named nets have no solver node;
single-endpoint nets and unwired terminals do not create hidden connections.
Absent undriven MCU pads remain floating. Supplies, pulls, leakage and driver
impedances must be explicitly provided.

All values use SI units. Resistors require finite positive resistance. Independent
current sources flow from p to n; ideal voltage sources constrain Vp−Vn. A driver
is a Thevenin source with finite positive resistance, with current
`(Vp−Vn−sourceVoltage)/R`. Its output impedance is explicit model input. A GPIO
open-drain release omits its low driver; it does not insert a high driver. Fixed
resistors, finite-resistance switches, potentiometer segments and source impedance
are lowered by the typed graph adapter. A zero-ohm ideal connection is an explicit
node merge at an exact potentiometer endpoint, not an invalid resistor workaround.

Capacitors, nonlinear devices, controlled sources, inductors, AC/RF networks,
temperature dependence and physical damage are outside this DC kernel. No
silicon-characterized drive-strength impedance is inferred from datasheet current
specifications. Currents enable later contention diagnostics; multiple opposing
drivers do not make the equations invalid merely because they disagree.

## Numerical contract

Modified nodal analysis solves node voltages and ideal-source currents. Dense
row-equilibrated Gaussian elimination uses deterministic partial pivoting, stable
first-row tie breaks and a `1e-12` scaled pivot floor. There are at most 191 unknowns;
allocation is bounded below 600 KiB and work is O(n³). A singular or rejected
ill-conditioned system is an error with unknown outputs, never repaired by an
invisible shunt to ground. Conflicting ideal sources and redundant source loops
are rejected; the latter have ambiguous source currents even when voltages agree.

Every solution is checked against the original, unmodified equations. KCL residual
tolerance is `1e-12 A + 1e-9 × sum(abs(terms))`; voltage constraints use `1e-10 V`
instead of the absolute current term. Nonfinite coefficients, solutions, currents
or residuals fail. These are numerical residual bounds, not a blanket accuracy
guarantee for an ill-conditioned circuit or a hardware-characterization claim.

Conductive islands are formed by resistors, finite-impedance drivers and voltage
sources. Ideal current sources do not establish an absolute reference. A floating
island receives one internal gauge solely to compute relative differences/currents;
its public absolute voltages remain NaN with a floating flag. Original KCL still
applies at the gauged node. Before selecting any gauge, a separate extended-
precision check balances each unreferenced island's cross-island current sources.
Its tolerance scales only to those external injections, so large internal
circulating currents cannot hide a missing DC return path. Net external current
beyond `1e-12 A + 1e-9 × sum(abs(external injections))` reports no operating point.
No stored charge is modeled.

Repeating identical inputs in the same build must produce identical result bytes.
Cross-architecture/compiler bit identity is not qualified. Source/element order
must retain voltages/currents within numerical tolerance, not necessarily identical
floating-point bytes. Build without fast-math. Failure clears previous successful
voltage/current outputs to NaN, preventing stale data from appearing valid.

## Graph projection ABI

`net-adapter.h` defines ABI **2**, with borrowed input storage and bounded C tables.
Component, terminal and net identities must resolve exactly; duplicate/unknown
endpoints, terminal membership on two declared nets, unsupported component kinds
and invalid quantities are named errors. The DC projection supports grounds,
resistors, voltage/current sources, potentiometers, switches and explicit MCU
terminal drivers. Capacitors and generic devices require separate models.

Incomplete wiring is not a graph-wide rejection. Each unwired element terminal
gets a private node; it never becomes connected through a name or geometry.
An open switch without an off resistance contributes no element. Empty nets and
unwired noncontributing pads/switches consume no solver capacity and return
`UINT_MAX`/NaN as applicable. A disconnected resistor terminal can have a finite
potential through its other terminal while carrying zero branch current; an
isolated voltage source retains unknown absolute potentials even if a separate
ground symbol exists elsewhere.

Ground references are declared, not inferred from driver return names. Every
active MCU driver names a real return terminal and supplies voltage/impedance;
open-drain release inserts no high driver. Output terminal membership (`on_net`)
is separate from voltage/floating state. ABI 1's obsolete dangling-net/unwired-
terminal error values were removed; consumers use `EN_ADPT_ABI_VERSION`.

Review reproduced an unwired-open-switch negative array index under UBSan.
The correction and incomplete-net/source-isolation regressions now pass **14
adapter scenarios / 2780 checks**, alongside the unchanged 553 DC checks and
ASan/UBSan targets (**4/4 CTest**). This remains host/numerical evidence, not
native electrical routing or ADC qualification.

## Pad interpretation boundary

The helper receives VDD and low/high threshold fractions for the actual power
domain. The ESP32-S3 datasheet specifies VIL maximum 0.25×VDD and VIH minimum
0.75×VDD, with a typical weak pull resistance of 45 kΩ. Those values may parameterize
an explicit profile; they are not automatic circuit elements. The threshold gap
returns indeterminate and a floating node returns floating. Out-of-rail voltages
return invalid for later diagnostic/overvoltage handling rather than a coerced bit.
[ESP32-S3 datasheet](https://documentation.espressif.com/esp32_s3_datasheet_en.pdf)

This helper does not model hysteresis, clamp/leakage current or sample acquisition.
Digital edge/IRQ behavior and conversion to an ADC code require separately reviewed
native pad/controller and analog acquisition models.

## Validation and next integration

Build and run independently under WSL:

```sh
cmake -S qemu-extensions/electrical -B build-electrical-dc -G Ninja
cmake --build build-electrical-dc
ctest --test-dir build-electrical-dc --output-on-failure
```

Tests compare independent analytic divider, loaded-driver, opposing-driver,
open-drain/pull, floating-source/current-balance and bridge solutions. They also
cover invalid/singular circuits, clean failure output, threshold boundaries,
maximum bounds, element reordering and repeated solves. They do not test MCU
registers or firmware. Integration still requires the actual v3-to-C projection
at the native boundary, timestamped driver state, virtual-time scheduling,
reset/power ownership, observable diagnostics and native GPIO input/IRQ evidence.
RC and ADC gates remain separate work packages.
