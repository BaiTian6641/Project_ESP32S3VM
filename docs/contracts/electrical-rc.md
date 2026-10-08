# Bounded linear RC transient kernel — 2026-10-07

Status: ANALOG-02 / H-ANALOG-01 standalone primitive, sibling of the reviewed
DC kernel. Not connected to QEMU pads, project Apply, native GPIO IRQs, pad
thresholds or ADC; those integrations remain unavailable and are the parent's
cutover. Source under `qemu-extensions/electrical` is GPL-2.0-or-later for the
separate QEMU core. No code was copied from ngspice; its
[manual](https://ngspice.sourceforge.io/docs/ngspice-manual.pdf) informed the
local-truncation-error conventions.

## Scope and inputs

Node 0 is an explicit caller reference; no ground, supply, pull, leakage or
impedance is inferred. Elements: resistors (finite positive ohms), capacitors
(finite positive farads with an explicit initial voltage), ideal voltage
sources (branch current solved), ideal current sources (signed p→n) and
Thevenin drivers (finite positive source resistance). SI units throughout.
The adapter layer keeps ownership of stable project IDs; geometry never
contributes connectivity. A zero-ohm connection is a node merge, not a
resistor workaround.

## Integration method and events

Backward Euler per step, chosen over trapezoidal integration for L-stable
damping at discontinuities (no ringing at source edges or into stiff
modes). Every accepted step is verified by deterministic step doubling: the
one-step-h solution must agree with the two-step-h/2 solution within
`abs_error_v + rel_error * max(|v_A|, |v_B|)` on every non-reference node;
the estimate is published per advance. Acceptance commits trial A (plain BE,
keeping L-stability; no local extrapolation). After acceptance the next
trial doubles up to `max_step_s`; after a rejection it halves down to
`min_step_s` (`EN_RC_STEP_MIN` below that).

Steps always end exactly at the requested deadline, so source, topology and
sample deadlines are never straddled: callers commit changes at a time t
(then sample, then continue), zero-length advances are allowed, and observed
node voltages are right-continuous in committed changes while capacitor
charge stays continuous. Watched crossings stop the advance exactly at the
crossing time (`EN_RC_CROSSED`), so digital edges land on virtual-time
boundaries and no aperture is skipped.

Every failure — rejection budget, minimum-step floor, work budget, crossing
bisection budget, subnormal half-step, singular system — returns an explicit
status with solver time and capacitor charge exactly as at call entry. There
is no unbounded loop and no invented voltage.

## Numerical contract

Modified nodal analysis: `(nodes-1)` node rows plus ideal-source branch rows
(plus capacitor branch rows in the instantaneous algebraization), dense
row-equilibrated elimination with deterministic partial pivoting, stable
first-row tie breaks and a `1e-12` scaled pivot floor — the same algorithm
and constants as `net-dc.c`. At most 191 unknowns; the workspace is one
allocation at creation (< 600 KiB) and the step/sample path performs no heap
allocation. Every solve is verified against the original, unmodified
equations (KCL `1e-12 A + 1e-9·Σ|terms|`, voltage rows `1e-10 V`); gauge rows
are excluded because their imbalance is the known, published floating drift.
Nonfinite inputs, solutions or residuals fail explicitly.

`abs_error_v`/`rel_error` bound the estimated per-step local truncation
error, not the global trajectory error. As with SPICE `reltol`, a
first-order method under per-step control refines the global error roughly
as √tolerance; measured on the canonical τ=10 ms charge (below), tightening
rel from 2e-4 to 2.5e-5 improved the worst error 3.9e-3 V → 1.2e-3 V.
Callers needing tighter absolute accuracy tighten the declared bounds; the
convergence suite pins monotone improvement.

## Islands, floating voltage and charge

Stamping elements (resistors, drivers, voltage sources, capacitors) unite
nodes into islands. An island without node 0 has unknown absolute voltage:
public voltages are NaN with floating flags, one internal gauge per island
(lowest node) exists only to solve observable relative quantities, and it is
never published. Summing a floating island's KCL rows cancels every internal
branch current — each two-terminal element, capacitors included, injects +i
into one node row and -i into the other — so net external current crossing
an unreferenced island's boundary must be zero for any solution to exist:
internal capacitors store only relative charge and cannot absorb
common-mode charge. Any unbalanced external injection into a floating
island (gauge node or not) is therefore rejected `EN_RC_NO_OPERATING_POINT`
at creation/edit, exactly like the DC kernel's missing return path; a
current source with both terminals inside the island is balanced, allowed,
and drives the observable differential state analytically.

Capacitor charge persists across steps and across `en_rc_edit` under an
explicit policy keyed by ELEMENT index: a capacitor whose element index was
already a capacitor keeps its state (`EN_RC_KEEP_CHARGE`:
`v_new = C_old·v_old/C_new`, bit-exact when C is unchanged;
`EN_RC_RESET_CHARGE`: element `initial_voltage`); element slots that were
not capacitors start at their declared initial voltage. Ordinal positions
never participate, so removing an earlier capacitor cannot silently reset
an unchanged later one (regression-tested). Edits are atomic: a failed
validation or settlement restores the previous circuit, charge and caches.
`en_rc_reset` restores every `initial_voltage`, sets time and re-arms
crossing watches; edits also re-arm watches.

Conflicting or redundant ideal voltage sources, and a capacitor ganged to an
ideal voltage source (indeterminate instantaneous branch current), are
rejected `EN_RC_SINGULAR` at creation/edit — never repaired by a hidden
shunt. Sampled currents are signed p→n; capacitor currents are the
instantaneous algebraic current the network forces through the capacitor at
that time (a trajectory-average current is a step-quantity, not a state).

## Threshold crossings

A watch stops the advance at the first detected crossing, located by bounded
bisection in which every probe re-integrates from the saved step start with
the same adaptive controller — the reported crossing is a trajectory point,
not an extrapolation. Detection and bracketing always read the COMMITTED
step solution (trial A), never the discarded verification half-steps. The
requested `crossing_tol_s` bracket is re-tested after every bisection, so a
bracket that reaches tolerance on the final allowed bisection is reported;
exhausting `crossing_max_bisections` without reaching tolerance is
`EN_RC_CROSSING_BUDGET` with state restored. At the committed crossing time
the instantaneous algebraization is re-settled, so the published
`crossing_voltage_v` equals what `en_rc_sample()` returns at that same time
bit-for-bit; the event TIME remains defined by the committed trajectory.
Floating-node watches are rejected (`EN_RC_INVALID`) rather than silently
resolved.

After a crossing is reported, the same direction on the same node/threshold
cannot fire again until the value demonstrably moves to the other side by
more than `abs_error_v + rel_error·|threshold|`; an opposite-direction
crossing fires immediately. This latch is derived from the published
tolerances so a caller resuming after a digital edge always makes progress;
it is not a measured hysteresis profile, which remains ANALOG-03 scope with
S3 datasheet thresholds (VIL ≤ 0.25·VDD, VIH ≥ 0.75·VDD) applied by the pad
layer, not here.

`en_rc_copy_state(dst, src)` copies the entire evolving state (time,
capacitor charges, crossing latch, published algebraic values) between
solvers with byte-identical circuits and options — no allocation, no matrix
or topology cache traffic, source untouched. A reusable probe continued from
a copied state reproduces the live solver's watched advance bit-for-bit
(same crossing time and committed sample); the adaptive controller restarts
every advance at `initial_step_s` by design, so no hidden controller state
exists.

## Determinism and validation

Identical call sequences on the same build reproduce bit-identical results
(verified by memcmp of advance records, samples and crossing times). Build
without fast-math. Cross-architecture bit identity is not qualified.

Validation (`tests/net-rc-test.c`, 616 checks, normal + ASan/UBSan builds;
the existing DC and adapter suites remain green): analytic charge/discharge,
source step with zero-length advance and right-continuity, switched divider,
20-period PWM against a recursive exponential reference (max error 1.6e-3 V),
two-time-constant stiff ladder against an independent fixed-step backward
Euler reference implemented in the test (max difference 9.2e-5 V at a
~2e5 time-constant ratio), τ=1e-15 near-zero RC, tolerance-refinement
convergence, floating charged island / island loop / internal-source drive /
injection rejection (gauge and non-gauge node) / balanced-source drive,
ideal-source conflicts, all bounded failure modes with state-unchanged
verification, byte-identical replay, crossing accuracy against analytic
times (1.8e-4 s at rel 1e-3, 2.1e-5 s at rel 1e-5; falling 1.4e-5 s),
crossing budget-edge and crossing-sample bit-consistency regressions,
charge-policy and element-index remap transitions, atomic-edit contracts,
and bounds validation. Review-cycle numbers are recorded in
`build-runtime-state/rc-review-2026-10-07/` (initial lane evidence in
`build-runtime-state/rc-2026-10-07/`).

Integration must first provide the strict graph adapter mapping, virtual-time
event scheduling, pad threshold profiles and native ADC paths. This kernel
makes no native pad, GPIO or ADC capability claim.
