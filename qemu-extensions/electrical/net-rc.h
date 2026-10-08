/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ESP32S3VM_NET_RC_H
#define ESP32S3VM_NET_RC_H

#include <stddef.h>
#include <stdint.h>

/* Bounded linear RC transient kernel (ANALOG-02 / H-ANALOG-01).
 *
 * Sibling of net-dc.h with the same conventions: node 0 is an EXPLICIT
 * caller-supplied reference, no ground/supply/pull/leakage is inferred,
 * floating absolute voltages stay NaN, SI units, fixed capacities. The
 * adapter layer (not this kernel) maps stable project terminal/net IDs to
 * local indices. This library does not implement a QEMU pad, clocks, an ADC,
 * or pad threshold classification.
 *
 * Integration method: backward Euler per step with a deterministic
 * step-doubling acceptance controller. Sources are piecewise constant
 * between caller events; steps always END EXACTLY at the requested deadline
 * so source/topology/sample events are never skipped or straddled. A
 * rejected or failed step leaves time and capacitor charge unchanged.
 *
 * Capacitor state (charge, expressed as the differential voltage v(p)-v(n))
 * persists across steps and across topology/source edits according to an
 * explicit charge policy. Absolute potentials of islands without a
 * conductive or capacitive path to node 0 remain unknown (NaN): an internal
 * gauge is used solely to compute observable relative quantities and is
 * never published. Inconsistent algebraic constraints (conflicting or
 * redundant ideal sources) are rejected, not repaired.
 *
 * The solver workspace is allocated once at creation, sized for the worst
 * case (191 unknowns, < 600 KiB) and reused: the per-step/per-sample path
 * performs no heap allocation. Determinism: identical call sequences on the
 * same build reproduce bit-identical results. Build without fast-math. */
#define EN_RC_MAX_NODES 64
#define EN_RC_MAX_ELEMENTS 128
/* Worst-case MNA dimension: (nodes-1) node rows + ideal voltage source
 * branches + capacitor branches in the instantaneous algebraization. */
#define EN_RC_MAX_DIM ((EN_RC_MAX_NODES - 1) + EN_RC_MAX_ELEMENTS)

typedef enum EnRcKind {
    EN_RC_RESISTOR,       /* value: resistance in ohms, finite > 0 */
    EN_RC_CAPACITOR,      /* value: capacitance in farads, finite > 0;
                             initial_voltage: v(p)-v(n) at reset/time zero */
    EN_RC_VOLTAGE_SOURCE, /* value: ideal voltage v(p)-v(n); branch current solved */
    EN_RC_CURRENT_SOURCE, /* value: signed amperes flowing p -> n */
    EN_RC_DRIVER          /* Thevenin source: value = v(p)-v(n) open circuit,
                             resistance > 0 finite */
} EnRcKind;

typedef struct EnRcElement {
    EnRcKind kind;
    unsigned p, n;
    double value;
    double resistance;      /* Used only for EN_RC_DRIVER. */
    double initial_voltage; /* Used only for EN_RC_CAPACITOR. */
} EnRcElement;

typedef struct EnRcCircuit {
    size_t node_count; /* Includes the explicit reference node, at least 1. */
    size_t element_count;
    EnRcElement elements[EN_RC_MAX_ELEMENTS];
} EnRcCircuit;

typedef enum EnRcStatus {
    EN_RC_OK = 0,
    EN_RC_CROSSED,         /* Watch fired; state is exactly at the crossing. */
    EN_RC_INVALID,         /* Bad arguments, bounds, values or watch target. */
    EN_RC_SINGULAR,        /* Conflicting/redundant ideal sources, singular
                              instantaneous algebraization. */
    EN_RC_FLOATING,        /* Success: some absolute voltages are unknown. */
    EN_RC_NO_OPERATING_POINT, /* Net external current into an unreferenced
                                 (floating) island: internal branch currents
                                 cancel in the island KCL sum and internal
                                 capacitors cannot absorb common-mode
                                 charge, so no solution exists. */
    EN_RC_STEP_MIN,        /* Required step fell below the configured minimum. */
    EN_RC_STEP_UNDERFLOW,  /* Remaining interval below double resolution. */
    EN_RC_REJECTED,        /* Per-step rejection budget exhausted. */
    EN_RC_WORK_EXCEEDED,   /* Per-advance matrix-factorization budget gone. */
    EN_RC_CROSSING_BUDGET, /* Crossing not located within the requested
                              bracket tolerance and bisection budget. */
    EN_RC_NUMERICAL,
    EN_RC_ALLOCATION_FAILED
} EnRcStatus;

/* Published tolerances and budgets. Defaults from en_rc_options_default();
 * every field is validated at solver creation. abs_error_v and rel_error
 * bound the estimated per-step voltage error (max over non-reference nodes
 * of |one step h - two steps h/2| against abs_error_v + rel_error*scale).
 * Steps clamped to a deadline may be shorter than min_step; min_step is the
 * halving floor of the adaptive controller. */
typedef struct EnRcOptions {
    double abs_error_v;               /* > 0 volts. */
    double rel_error;                 /* >= 0. */
    double min_step_s;                /* > 0, <= max_step_s. */
    double max_step_s;                /* >= min_step_s. */
    double initial_step_s;            /* > 0, <= max_step_s. */
    uint32_t max_step_rejections;     /* >= 1 consecutive per step. */
    uint64_t max_matrix_solves;       /* >= 1 per advance call. */
    double crossing_tol_s;            /* > 0 bracket target for crossings. */
    uint32_t crossing_max_bisections; /* >= 1. */
} EnRcOptions;

void en_rc_options_default(EnRcOptions *options);

/* Opaque solver: circuit + persistent capacitor state + fixed workspace. */
typedef struct EnRcSolver EnRcSolver;

/* Validates the circuit, builds the island/topology caches, checks for a
 * consistent operating point and settles the instantaneous algebraic
 * solution at time 0. Capacitors start at their element initial_voltage.
 * Returns NULL with *status set on failure. */
EnRcSolver *en_rc_create(const EnRcCircuit *circuit, const EnRcOptions *options,
                         EnRcStatus *status);
void en_rc_destroy(EnRcSolver *solver);

/* Charge policy applied by en_rc_edit to capacitors whose element index is a
 * capacitor in both the old and the new circuit.
 *   EN_RC_KEEP_CHARGE:  v_new = C_old * v_old / C_new (charge invariant).
 *   EN_RC_RESET_CHARGE: v_new = new element initial_voltage. */
typedef enum EnRcChargePolicy {
    EN_RC_KEEP_CHARGE = 0,
    EN_RC_RESET_CHARGE = 1
} EnRcChargePolicy;

/* Atomically replace the topology/source values. The new circuit is fully
 * validated first; on any failure the previous circuit, charge and time are
 * left untouched. Capacitor state is remapped by element index under the
 * given policy; new capacitors start at initial_voltage. Re-evaluates the
 * island caches and the operating-point checks. Time is unchanged. */
EnRcStatus en_rc_edit(EnRcSolver *solver, const EnRcCircuit *circuit,
                      EnRcChargePolicy policy);

/* Explicit state access by ELEMENT index (EN_RC_CAPACITOR required).
 * charge = C * voltage. Setters make the next algebraic evaluation use the
 * new state; they never advance time. */
EnRcStatus en_rc_set_capacitor_voltage(EnRcSolver *solver, size_t element_index,
                                       double voltage);
EnRcStatus en_rc_set_capacitor_charge(EnRcSolver *solver, size_t element_index,
                                      double charge);
double en_rc_capacitor_voltage(const EnRcSolver *solver, size_t element_index,
                               EnRcStatus *status);
double en_rc_capacitor_charge(const EnRcSolver *solver, size_t element_index,
                              EnRcStatus *status);

/* Restore every capacitor to its element initial_voltage and set time.
 * Cumulative statistics are solver-lifetime and survive reset. */
EnRcStatus en_rc_reset(EnRcSolver *solver, double time);

/* Copy the EVOLVING simulation state from src to dst: time, capacitor
 * charges, the crossing-chatter latch, and the published algebraic values
 * (node voltages / residuals, so a probe solver continues bit-faithfully).
 * Everything that determines future advance/sample results is copied;
 * deliberately NOT copied: circuit, options, matrix workspace, island and
 * topology caches (both solvers must already carry byte-identical circuits
 * and options, enforced bytewise), cumulative statistics, and scratch trial
 * buffers. No allocation, no heavy caches, src is never modified; dst == src
 * is a validated no-op. The adaptive controller restarts every advance at
 * initial_step_s by design, so no hidden next-step state exists.
 * Returns EN_RC_INVALID when the circuits or options differ. */
EnRcStatus en_rc_copy_state(EnRcSolver *dst, const EnRcSolver *src);

double en_rc_time(const EnRcSolver *solver);
size_t en_rc_node_count(const EnRcSolver *solver);
size_t en_rc_element_count(const EnRcSolver *solver);

typedef enum EnRcEdge {
    EN_RC_EDGE_ANY = 0,
    EN_RC_EDGE_RISING,
    EN_RC_EDGE_FALLING
} EnRcEdge;

/* Watch one node against one threshold level. The node must have a known
 * absolute voltage (not in a floating island). A crossing fires when the
 * watched value leaves one side of the threshold and reaches the other side
 * at a step endpoint: rising fires for f<0 -> f>=0, falling for f>0 ->
 * f<=0, with f = v - threshold. A value sitting exactly on the threshold at
 * a step start is not a new crossing. After a crossing is reported, the
 * same node/threshold cannot fire again until the watched value has moved
 * to the other side by more than the declared error band (abs_error_v +
 * rel_error*|threshold|): a deterministic chatter guard derived from the
 * published tolerances, so a caller resuming after a digital edge always
 * makes progress; the latch blocks only the direction that fired, and an
 * opposite-direction crossing of the same node/threshold fires immediately.
 * Measured hysteresis profiles remain ANALOG-03 scope.
 * en_rc_edit and en_rc_reset re-arm every watch. Crossings are detected and
 * reported on the COMMITTED step solution: crossing_voltage_v equals what
 * en_rc_sample() publishes at the crossing time, and a bracket that reaches
 * crossing_tol_s exactly on the final allowed bisection is reported rather
 * than rejected. */
typedef struct EnRcWatch {
    int enabled;
    unsigned node;
    double threshold_v;
    EnRcEdge edge;
} EnRcWatch;

typedef struct EnRcAdvance {
    EnRcStatus status;
    double time;            /* Solver time after the call. */
    unsigned steps_accepted;/* Mainline integrator steps. */
    unsigned steps_rejected;/* Includes refinement re-marches. */
    uint64_t matrix_solves; /* Factorizations consumed by this call. */
    double step_min_s;      /* Accepted mainline step sizes; NaN if none. */
    double step_max_s;
    double error_abs_v;     /* Last accepted step error estimate (volts). */
    size_t error_node;      /* Node index of the maximum estimate. */
    int crossed;
    unsigned crossed_node;
    double crossing_time_s;    /* Exact solver time of the reported crossing. */
    double crossing_voltage_v; /* Watched voltage at crossing_time_s. */
    double crossing_bracket_s; /* Achieved bracket width (<= crossing_tol_s
                                  unless the FP resolution limit was hit). */
    char diagnostic[160];
} EnRcAdvance;

/* Advance from the current time to t_target (>= current time). Steps end
 * exactly at t_target; a watch crossing inside (t, t_target] stops the
 * advance with state committed exactly at the crossing time and status
 * EN_RC_CROSSED, so callers can commit digital edges and resume. On any
 * failure the solver time and capacitor charge are exactly as at entry.
 * Budgets: at most options.max_matrix_solves factorizations and bounded
 * steps per call; every exhaustion is a reported error, never an
 * unbounded loop. */
EnRcStatus en_rc_advance(EnRcSolver *solver, double t_target,
                         const EnRcWatch *watch, EnRcAdvance *out);

typedef struct EnRcSample {
    EnRcStatus status; /* EN_RC_OK, EN_RC_FLOATING or a failure status. */
    double time;
    double voltage[EN_RC_MAX_NODES]; /* NaN on floating islands or failure. */
    unsigned char floating[EN_RC_MAX_NODES];
    double current[EN_RC_MAX_ELEMENTS]; /* Signed p -> n; capacitors carry
                              the instantaneous algebraic current the
                              network forces through them at this time,
                              not the trajectory average. */
    double max_kcl_residual_a;
    double max_source_residual_v;
    char diagnostic[160];
} EnRcSample;

/* Instantaneous algebraic solution at the current time: resistor/driver/
 * source network with capacitors treated as ideal voltage sources holding
 * their stored charge. Never changes time or charge; at least one matrix
 * factorization per call. Creation and edits validate this algebraization,
 * so a capacitor ganged to an ideal voltage source (indeterminate
 * instantaneous branch current) is rejected up front instead of being
 * repaired; state injected later through the explicit capacitor setters
 * that breaks the residual checks is reported here with time and charge
 * unchanged. */
EnRcStatus en_rc_sample(EnRcSolver *solver, EnRcSample *out);

typedef struct EnRcStats {
    uint64_t matrix_solves_total;    /* Since solver creation. */
    uint64_t steps_accepted_total;
    uint64_t steps_rejected_total;
    double step_min_s_total;         /* Accepted steps since creation. */
    double step_max_s_total;
    double last_error_abs_v;         /* Last accepted step estimate. */
    unsigned crossings_total;
} EnRcStats;

void en_rc_stats(const EnRcSolver *solver, EnRcStats *out);

#endif
