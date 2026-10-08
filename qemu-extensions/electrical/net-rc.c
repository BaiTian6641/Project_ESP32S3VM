/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "net-rc.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Numerical constants mirror the reviewed DC kernel contract
 * (docs/contracts/electrical-dc.md): row-equilibrated dense elimination with
 * deterministic partial pivoting, a 1e-12 scaled pivot floor, and residual
 * checks against the original, unmodified equations. */
#define EN_RC_PIVOT_TOL 1e-12
#define EN_RC_REL_TOL 1e-9
#define EN_RC_CURRENT_ABS_TOL 1e-12
#define EN_RC_VOLTAGE_ABS_TOL 1e-10

#define EN_RC_DIAG_MAX 160

struct EnRcSolver {
    EnRcCircuit circuit;
    EnRcOptions options;

    /* Derived topology (rebuilt at create/edit only, never per step). */
    size_t nsrc;                            /* Ideal voltage sources. */
    size_t ncaps;                           /* Capacitors (charge carriers). */
    size_t cap_element[EN_RC_MAX_ELEMENTS]; /* Cap ordinal -> element index. */
    int32_t source_slot[EN_RC_MAX_ELEMENTS];/* Element -> branch slot or -1. */

    /* Island cache: stamping elements (resistor, driver, voltage source,
     * capacitor) unite their nodes. Islands without node 0 have unknown
     * absolute voltage; one internal gauge per island is used only to solve
     * observable relative quantities and is never published. */
    unsigned parent[EN_RC_MAX_NODES];
    unsigned char floating[EN_RC_MAX_NODES];
    long double island_external_a[EN_RC_MAX_NODES];
    long double island_external_scale[EN_RC_MAX_NODES];

    /* Persistent state. */
    double time;
    double cap_v[EN_RC_MAX_ELEMENTS]; /* Capacitor v(p)-v(n), by ordinal. */

    /* Rollback snapshots (call entry, mainline step start, probe steps). */
    double snap_time[3];
    double snap_caps[3][EN_RC_MAX_ELEMENTS];

    /* Node voltage vectors (raw solution values; published outputs convert
     * floating islands to NaN). Reused scratch, never heap-allocated. */
    double last_v[EN_RC_MAX_NODES];
    double trial_v_a[EN_RC_MAX_NODES];
    double trial_v_b[EN_RC_MAX_NODES];
    double probe_v[EN_RC_MAX_NODES];
    double caps_a[EN_RC_MAX_ELEMENTS];
    double caps_mid[EN_RC_MAX_ELEMENTS];
    double probe_caps[EN_RC_MAX_ELEMENTS];

    double last_max_kcl_residual_a;
    double last_max_source_residual_v;
    /* Crossing chatter guard: after a watch fires, the same node/threshold
     * cannot fire again until the watched value has demonstrably returned to
     * the other side of the threshold by more than the declared error band
     * (abs_error_v + rel_error*|threshold|). Derived from published
     * tolerances; measured hysteresis profiles remain ANALOG-03 scope. */
    int watch_latch_active;
    unsigned watch_latch_node;
    double watch_latch_threshold;
    int watch_latch_rising;
    EnRcStats stats;

    /* Fixed MNA workspace sized for EN_RC_MAX_DIM in every mode. */
    double *mna_original; /* Unmodified equations for the residual check. */
    double *mna_a;        /* Elimination copy. */
    double *mna_rhs;
    double *mna_b;
    double *mna_x;
};

static void set_diag(char *dst, const char *message)
{
    snprintf(dst, EN_RC_DIAG_MAX, "%s", message);
}

static unsigned root(const unsigned *parent, unsigned node)
{
    while (parent[node] != node) {
        node = parent[node];
    }
    return node;
}

static void unite(unsigned *parent, unsigned p, unsigned n)
{
    p = root(parent, p);
    n = root(parent, n);
    /* Stable lowest-index gauge, independent of element order. */
    if (p < n) {
        parent[n] = p;
    } else {
        parent[p] = n;
    }
}

static void stamp_conductance(double *a, size_t dimension, unsigned p,
                              unsigned n, double g)
{
    if (p) {
        a[(p - 1) * dimension + p - 1] += g;
    }
    if (n) {
        a[(n - 1) * dimension + n - 1] += g;
    }
    if (p && n) {
        a[(p - 1) * dimension + n - 1] -= g;
        a[(n - 1) * dimension + p - 1] -= g;
    }
}

static void stamp_rhs(double *b, unsigned p, unsigned n, double injection)
{
    if (p) {
        b[p - 1] += injection;
    }
    if (n) {
        b[n - 1] -= injection;
    }
}

static void stamp_branch(double *a, size_t dimension, size_t row, unsigned p,
                         unsigned n)
{
    if (p) {
        a[(p - 1) * dimension + row] += 1;
        a[row * dimension + p - 1] += 1;
    }
    if (n) {
        a[(n - 1) * dimension + row] -= 1;
        a[row * dimension + n - 1] -= 1;
    }
}

/* Row-equilibrated dense Gaussian elimination with deterministic partial
 * pivoting (stable first-row tie breaks) and a scaled pivot floor. Same
 * algorithm as the DC kernel; failure statuses are EN_RC_* values. */
static EnRcStatus solve_matrix(double *a, double *b, double *x, size_t dim)
{
    for (size_t row = 0; row < dim; ++row) {
        double scale = 0;
        for (size_t col = 0; col < dim; ++col) {
            if (!isfinite(a[row * dim + col])) {
                return EN_RC_NUMERICAL;
            }
            scale = fmax(scale, fabs(a[row * dim + col]));
        }
        if (!isfinite(b[row])) {
            return EN_RC_NUMERICAL;
        }
        if (scale == 0) {
            return EN_RC_SINGULAR;
        }
        for (size_t col = 0; col < dim; ++col) {
            a[row * dim + col] /= scale;
        }
        b[row] /= scale;
        if (!isfinite(b[row])) {
            return EN_RC_NUMERICAL;
        }
    }
    for (size_t col = 0; col < dim; ++col) {
        size_t pivot = col;
        for (size_t row = col + 1; row < dim; ++row) {
            /* Equal candidates retain the first row, a stable tie break. */
            if (fabs(a[row * dim + col]) > fabs(a[pivot * dim + col])) {
                pivot = row;
            }
        }
        if (!isfinite(a[pivot * dim + col])) {
            return EN_RC_NUMERICAL;
        }
        if (fabs(a[pivot * dim + col]) <= EN_RC_PIVOT_TOL) {
            return EN_RC_SINGULAR;
        }
        if (pivot != col) {
            for (size_t k = col; k < dim; ++k) {
                double value = a[col * dim + k];
                a[col * dim + k] = a[pivot * dim + k];
                a[pivot * dim + k] = value;
            }
            double value = b[col];
            b[col] = b[pivot];
            b[pivot] = value;
        }
        for (size_t row = col + 1; row < dim; ++row) {
            double multiplier = a[row * dim + col] / a[col * dim + col];
            a[row * dim + col] = 0;
            for (size_t k = col + 1; k < dim; ++k) {
                a[row * dim + k] -= multiplier * a[col * dim + k];
            }
            b[row] -= multiplier * b[col];
        }
    }
    for (size_t row = dim; row-- > 0;) {
        double value = b[row];
        for (size_t col = row + 1; col < dim; ++col) {
            value -= a[row * dim + col] * x[col];
        }
        x[row] = value / a[row * dim + row];
        if (!isfinite(x[row])) {
            return EN_RC_NUMERICAL;
        }
    }
    return EN_RC_OK;
}

static void rc_save(EnRcSolver *s, int slot)
{
    s->snap_time[slot] = s->time;
    memcpy(s->snap_caps[slot], s->cap_v, sizeof(s->cap_v));
}

static void rc_load(EnRcSolver *s, int slot)
{
    s->time = s->snap_time[slot];
    memcpy(s->cap_v, s->snap_caps[slot], sizeof(s->cap_v));
}

/* Rebuild derived topology and the island caches from the current circuit.
 * Values (including ideal current source injections) change only through
 * en_rc_edit/en_rc_create, so this is never called on the step path. */
static void rc_rebuild_topology(EnRcSolver *s)
{
    const size_t nodes = s->circuit.node_count;
    s->nsrc = 0;
    s->ncaps = 0;
    for (size_t i = 0; i < nodes; ++i) {
        s->parent[i] = (unsigned)i;
    }
    for (size_t i = 0; i < EN_RC_MAX_ELEMENTS; ++i) {
        s->source_slot[i] = -1;
    }
    for (size_t i = 0; i < s->circuit.element_count; ++i) {
        const EnRcElement *e = &s->circuit.elements[i];
        switch (e->kind) {
        case EN_RC_RESISTOR:
        case EN_RC_DRIVER:
            unite(s->parent, e->p, e->n);
            break;
        case EN_RC_VOLTAGE_SOURCE:
            s->source_slot[i] = (int32_t)s->nsrc++;
            unite(s->parent, e->p, e->n);
            break;
        case EN_RC_CAPACITOR:
            /* Capacitor stamps are conductances within a step: they join
             * islands and can anchor absolute voltage through stored state. */
            s->cap_element[s->ncaps++] = i;
            unite(s->parent, e->p, e->n);
            break;
        case EN_RC_CURRENT_SOURCE:
            /* No conductive path; contributes external injections only. */
            break;
        }
    }
    for (size_t i = 0; i < EN_RC_MAX_NODES; ++i) {
        s->island_external_a[i] = 0;
        s->island_external_scale[i] = 0;
    }
    for (size_t i = 0; i < nodes; ++i) {
        s->floating[i] = root(s->parent, (unsigned)i) != root(s->parent, 0);
    }
    for (size_t i = 0; i < s->circuit.element_count; ++i) {
        const EnRcElement *e = &s->circuit.elements[i];
        if (e->kind != EN_RC_CURRENT_SOURCE) {
            continue;
        }
        unsigned rp = root(s->parent, e->p), rn = root(s->parent, e->n);
        if (rp != rn) {
            s->island_external_a[rp] -= (long double)e->value;
            s->island_external_a[rn] += (long double)e->value;
            s->island_external_scale[rp] += fabsl((long double)e->value);
            s->island_external_scale[rn] += fabsl((long double)e->value);
        }
    }
}

/* Summing the KCL rows of a floating island cancels every internal branch
 * current (each two-terminal element injects +i into one node row and -i
 * into the other, capacitors included), so net external current crossing an
 * unreferenced island's boundary must be zero for ANY solution to exist.
 * Internal capacitors store only relative charge and cannot absorb
 * common-mode charge; the gauge would silently swallow the imbalance. */
static EnRcStatus rc_check_operating_point(const EnRcSolver *s)
{
    for (size_t i = 1; i < s->circuit.node_count; ++i) {
        if (s->floating[i] && root(s->parent, (unsigned)i) == (unsigned)i &&
            fabsl(s->island_external_a[i]) >
                (long double)EN_RC_CURRENT_ABS_TOL +
                    (long double)EN_RC_REL_TOL * s->island_external_scale[i]) {
            return EN_RC_NO_OPERATING_POINT;
        }
    }
    return EN_RC_OK;
}

/* Build, factorize and verify one linear system.
 *   instant == 0: backward-Euler transient step of size step_h; capacitors
 *                 stamp as C/h conductances plus their history current.
 *   instant == 1: instantaneous algebraization; capacitors stamp as ideal
 *                 voltage sources holding their stored voltage.
 * One matrix factorization is counted. Gauge rows replace KCL only on the
 * elimination copy; the residual check runs against the original rows and
 * skips gauged rows (their imbalance is the known, published floating
 * drift, never a hidden repair). */
static EnRcStatus rc_solve_system(EnRcSolver *s, int instant, double step_h,
                                  uint64_t *counter)
{
    const size_t nodes = s->circuit.node_count;
    const size_t nrows = nodes - 1;
    size_t dim = nrows + s->nsrc + (instant ? s->ncaps : 0);
    double *orig = s->mna_original;
    double *a = s->mna_a;
    double *rhs = s->mna_rhs;
    double *b = s->mna_b;
    double *x = s->mna_x;

    memset(orig, 0, dim * dim * sizeof(double));
    memset(rhs, 0, dim * sizeof(double));
    size_t cap_ordinal = 0;
    for (size_t i = 0; i < s->circuit.element_count; ++i) {
        const EnRcElement *e = &s->circuit.elements[i];
        switch (e->kind) {
        case EN_RC_RESISTOR:
            stamp_conductance(orig, dim, e->p, e->n, 1 / e->value);
            break;
        case EN_RC_DRIVER:
            stamp_conductance(orig, dim, e->p, e->n, 1 / e->resistance);
            stamp_rhs(rhs, e->p, e->n, e->value / e->resistance);
            break;
        case EN_RC_CURRENT_SOURCE:
            stamp_rhs(rhs, e->p, e->n, -e->value);
            break;
        case EN_RC_VOLTAGE_SOURCE: {
            size_t row = nrows + (size_t)s->source_slot[i];
            stamp_branch(orig, dim, row, e->p, e->n);
            rhs[row] = e->value;
            break;
        }
        case EN_RC_CAPACITOR: {
            if (instant) {
                size_t row = nrows + s->nsrc + cap_ordinal;
                stamp_branch(orig, dim, row, e->p, e->n);
                rhs[row] = s->cap_v[cap_ordinal];
            } else {
                double gc = e->value / step_h;
                stamp_conductance(orig, dim, e->p, e->n, gc);
                stamp_rhs(rhs, e->p, e->n, gc * s->cap_v[cap_ordinal]);
            }
            ++cap_ordinal;
            break;
        }
        }
    }
    memcpy(a, orig, dim * dim * sizeof(double));
    memcpy(b, rhs, dim * sizeof(double));
    unsigned char gauged[EN_RC_MAX_NODES] = {0};
    for (size_t i = 1; i < nodes; ++i) {
        if (s->floating[i] && root(s->parent, (unsigned)i) == (unsigned)i) {
            size_t row = i - 1;
            gauged[i] = 1;
            memset(a + row * dim, 0, dim * sizeof(double));
            a[row * dim + row] = 1;
            b[row] = 0;
        }
    }
    EnRcStatus status = solve_matrix(a, b, x, dim);
    ++s->stats.matrix_solves_total;
    if (counter) {
        ++*counter;
    }
    if (status != EN_RC_OK) {
        return status;
    }
    double max_kcl = 0, max_src = 0;
    for (size_t row = 0; row < dim; ++row) {
        if (row < nrows && gauged[row + 1]) {
            continue;
        }
        double value = 0, scale = fabs(rhs[row]);
        for (size_t col = 0; col < dim; ++col) {
            double contribution = orig[row * dim + col] * x[col];
            value += contribution;
            scale += fabs(contribution);
        }
        double residual = fabs(value - rhs[row]);
        int node_row = row < nrows;
        double absolute = node_row ? EN_RC_CURRENT_ABS_TOL : EN_RC_VOLTAGE_ABS_TOL;
        if (node_row) {
            max_kcl = fmax(max_kcl, residual);
        } else {
            max_src = fmax(max_src, residual);
        }
        if (!isfinite(residual) || !isfinite(scale) ||
            residual > absolute + EN_RC_REL_TOL * scale) {
            return EN_RC_NUMERICAL;
        }
    }
    s->last_max_kcl_residual_a = max_kcl;
    s->last_max_source_residual_v = max_src;
    s->last_v[0] = 0;
    for (size_t i = 1; i < nodes; ++i) {
        s->last_v[i] = s->floating[i] ? NAN : x[i - 1];
    }
    return EN_RC_OK;
}

/* One backward-Euler step from the current state. Never mutates solver
 * state; outputs raw node voltages and the new capacitor voltages. */
static EnRcStatus rc_transient_step(EnRcSolver *s, double h, double *node_v,
                                    double *caps_out, uint64_t *counter)
{
    EnRcStatus status = rc_solve_system(s, 0, h, counter);
    if (status != EN_RC_OK) {
        return status;
    }
    node_v[0] = 0;
    for (size_t i = 1; i < s->circuit.node_count; ++i) {
        node_v[i] = s->mna_x[i - 1];
    }
    for (size_t k = 0; k < s->ncaps; ++k) {
        const EnRcElement *e = &s->circuit.elements[s->cap_element[k]];
        double v = node_v[e->p] - node_v[e->n];
        if (!isfinite(v)) {
            return EN_RC_NUMERICAL;
        }
        caps_out[k] = v;
    }
    return EN_RC_OK;
}

typedef struct RcMarchAccum {
    unsigned steps_accepted;
    unsigned steps_rejected;
    double step_min, step_max;
    double last_err;
    size_t last_err_node;
} RcMarchAccum;

static RcMarchAccum march_accum_init(void)
{
    RcMarchAccum a;
    a.steps_accepted = 0;
    a.steps_rejected = 0;
    a.step_min = NAN;
    a.step_max = NAN;
    a.last_err = 0;
    a.last_err_node = 0;
    return a;
}

static int rc_watch_armed(const EnRcSolver *s, const EnRcWatch *w,
                          int rising_fire)
{
    /* The latch blocks only the direction that fired: an opposite-direction
     * crossing of the same node/threshold fires immediately, and the latch
     * clears once the value exits past the declared band on the other side
     * (rc_watch_rearm). */
    return !s->watch_latch_active || s->watch_latch_node != w->node ||
           s->watch_latch_threshold != w->threshold_v ||
           s->watch_latch_rising != rising_fire;
}

static void rc_watch_latch(EnRcSolver *s, const EnRcWatch *w, int rising_fire)
{
    s->watch_latch_active = 1;
    s->watch_latch_node = w->node;
    s->watch_latch_threshold = w->threshold_v;
    s->watch_latch_rising = rising_fire;
}

static void rc_watch_rearm(EnRcSolver *s, const EnRcWatch *w, double v)
{
    if (!s->watch_latch_active || s->watch_latch_node != w->node ||
        s->watch_latch_threshold != w->threshold_v) {
        return;
    }
    double band = s->options.abs_error_v +
                  s->options.rel_error * fabs(w->threshold_v);
    if (s->watch_latch_rising ? v < w->threshold_v - band
                              : v > w->threshold_v + band) {
        s->watch_latch_active = 0;
    }
}

/* Adaptive backward-Euler march from the current time to t_target using the
 * deterministic step-doubling acceptance controller. Every accepted step
 * requires the one-step-h and two-step-h/2 solutions to agree within
 * abs_error_v + rel_error*scale on every non-reference node; steps always
 * end exactly at the deadline. watch (allowed only when probe == 0) stops
 * the march exactly at the first detected threshold crossing, located by
 * bounded bisection against the configured tolerance. On error the caller
 * restores the snapshot in slot snap_slot; time/charge were left unchanged
 * by this function in that case. */
static EnRcStatus rc_step_march(EnRcSolver *s, double t_target,
                                const EnRcWatch *watch, uint64_t budget,
                                uint64_t *solves, int snap_slot, int probe,
                                RcMarchAccum *acc, char *diag, EnRcAdvance *out)
{
    const size_t nodes = s->circuit.node_count;
    const size_t ncaps = s->ncaps;
    double h = s->options.initial_step_s;
    double prev_watch_v = watch ? s->last_v[watch->node] : 0;
    unsigned rejections = 0;
    for (;;) {
        double time = s->time;
        double span = t_target - time;
        if (!(span > 0)) {
            return EN_RC_OK;
        }
        if (time + span <= time) {
            set_diag(diag, "Remaining interval is below double resolution");
            return EN_RC_STEP_UNDERFLOW;
        }
        double h1 = h < span ? h : span;
        double half = 0.5 * h1;
        if (!(half > 0)) {
            set_diag(diag, "Half step underflowed before trials");
            return EN_RC_STEP_UNDERFLOW;
        }
        if (*solves + 3 > budget) {
            set_diag(diag, "Advance work budget exhausted before step trials");
            return EN_RC_WORK_EXCEEDED;
        }
        rc_save(s, snap_slot);
        EnRcStatus status = rc_transient_step(s, h1, s->trial_v_a, s->caps_a, solves);
        if (status == EN_RC_OK) {
            /* Trial B: two half steps through an intermediate state. */
            status = rc_transient_step(s, half, s->trial_v_b, s->caps_mid, solves);
            if (status == EN_RC_OK) {
                memcpy(s->cap_v, s->caps_mid, ncaps * sizeof(double));
                status = rc_transient_step(s, half, s->trial_v_b, s->caps_mid, solves);
            }
        }
        rc_load(s, snap_slot);
        if (status != EN_RC_OK) {
            snprintf(diag, EN_RC_DIAG_MAX, "%s",
                     status == EN_RC_SINGULAR
                         ? "Transient step system is singular or ill-conditioned"
                         : status == EN_RC_NUMERICAL
                               ? "Transient step failed numerically"
                               : "Transient step failed");
            return status;
        }
        double err = 0;
        size_t err_node = 0;
        int rejected = 0;
        for (size_t i = 1; i < nodes; ++i) {
            double va = s->trial_v_a[i], vb = s->trial_v_b[i];
            if (!isfinite(va) || !isfinite(vb)) {
                set_diag(diag, "Nonfinite trial solution");
                return EN_RC_NUMERICAL;
            }
            double e = fabs(va - vb);
            double tol = s->options.abs_error_v +
                         s->options.rel_error * fmax(fabs(va), fabs(vb));
            if (e > err) {
                err = e;
                err_node = i;
            }
            if (e > tol) {
                rejected = 1;
            }
        }
        if (rejected) {
            ++rejections;
            ++acc->steps_rejected;
            ++s->stats.steps_rejected_total;
            if (rejections > s->options.max_step_rejections) {
                set_diag(diag, "Step rejection budget exhausted");
                return EN_RC_REJECTED;
            }
            h = half;
            if (h < s->options.min_step_s) {
                set_diag(diag, "Required step fell below the minimum step");
                return EN_RC_STEP_MIN;
            }
            continue;
        }
        /* Accept trial A: time, charge and the published node voltages all
         * commit together from the SAME (trial A) solution, so watch
         * decisions and reported crossings describe committed state rather
         * than the discarded verification half-steps. */
        memcpy(s->cap_v, s->caps_a, ncaps * sizeof(double));
        s->last_v[0] = 0;
        for (size_t i = 1; i < nodes; ++i) {
            s->last_v[i] = s->floating[i] ? NAN : s->trial_v_a[i];
        }
        s->time = h1 >= span ? t_target : time + h1;
        ++acc->steps_accepted;
        ++s->stats.steps_accepted_total;
        if (isnan(acc->step_min) || h1 < acc->step_min) {
            acc->step_min = h1;
        }
        if (isnan(acc->step_max) || h1 > acc->step_max) {
            acc->step_max = h1;
        }
        if (!probe) {
            if (isnan(s->stats.step_min_s_total) || h1 < s->stats.step_min_s_total) {
                s->stats.step_min_s_total = h1;
            }
            if (isnan(s->stats.step_max_s_total) || h1 > s->stats.step_max_s_total) {
                s->stats.step_max_s_total = h1;
            }
        }
        acc->last_err = err;
        acc->last_err_node = err_node;
        s->stats.last_error_abs_v = err;
        rejections = 0;
        h = 2 * h1;
        if (h > s->options.max_step_s) {
            h = s->options.max_step_s;
        }
        if (watch && !probe) {
            double t0 = time;
            double f0 = prev_watch_v - watch->threshold_v;
            double v1 = s->last_v[watch->node];
            double f1 = v1 - watch->threshold_v;
            unsigned edge = (unsigned)watch->edge;
            int fired = 0;
            int rising_fire = f0 < 0;
            if (edge == (unsigned)EN_RC_EDGE_RISING) {
                fired = f0 < 0 && f1 >= 0;
            } else if (edge == (unsigned)EN_RC_EDGE_FALLING) {
                fired = f0 > 0 && f1 <= 0;
                rising_fire = 0;
            } else {
                fired = (f0 < 0 && f1 >= 0) || (f0 > 0 && f1 <= 0);
            }
            if (fired && rc_watch_armed(s, watch, rising_fire)) {
                rc_watch_latch(s, watch, rising_fire);
                /* Bounded bisection of the crossing inside [t0, t1]. Each
                 * probe re-integrates from the saved step start, so a
                 * refined crossing is a trajectory point of the same
                 * integrator, not an extrapolation. The state at the bracket
                 * end that satisfies the threshold is kept. */
                double ta = t0, tb = s->time;
                double f_tb = f1;
                memcpy(s->probe_caps, s->cap_v, ncaps * sizeof(double));
                memcpy(s->probe_v, s->last_v, sizeof(s->last_v));
                /* Bisection runs at most crossing_max_bisections times and
                 * re-tests the bracket tolerance after every bisection, so
                 * a bracket that reaches tolerance on the final allowed
                 * bisection is reported, not rejected. */
                int converged = tb - ta <= s->options.crossing_tol_s;
                for (unsigned iter = 0; !converged &&
                     iter < s->options.crossing_max_bisections; ++iter) {
                    double m = 0.5 * (ta + tb);
                    if (!(m > ta && m < tb)) {
                        /* Floating-point resolution reached: report the
                         * crossing with its achieved bracket. */
                        converged = 1;
                        break;
                    }
                    rc_load(s, snap_slot);
                    RcMarchAccum probe_acc = march_accum_init();
                    char probe_diag[EN_RC_DIAG_MAX];
                    EnRcStatus pst = rc_step_march(s, m, NULL, budget, solves,
                                                   2, 1, &probe_acc, probe_diag,
                                                   NULL);
                    acc->steps_rejected += probe_acc.steps_rejected;
                    if (pst != EN_RC_OK) {
                        snprintf(diag, EN_RC_DIAG_MAX, "Crossing probe failed: %.130s",
                                 probe_diag);
                        return pst;
                    }
                    /* s->last_v is the probe's COMMITTED endpoint solution. */
                    double f_m = s->last_v[watch->node] - watch->threshold_v;
                    if ((f_m >= 0) == (f_tb >= 0)) {
                        tb = m;
                        f_tb = f_m;
                        memcpy(s->probe_caps, s->cap_v, ncaps * sizeof(double));
                        memcpy(s->probe_v, s->last_v, sizeof(s->last_v));
                    } else {
                        ta = m;
                        memcpy(s->cap_v, s->probe_caps, ncaps * sizeof(double));
                        memcpy(s->last_v, s->probe_v, sizeof(s->last_v));
                        s->time = tb;
                    }
                    converged = tb - ta <= s->options.crossing_tol_s;
                }
                if (!converged) {
                    set_diag(diag, "Crossing bisection budget exhausted before "
                                   "reaching the requested tolerance");
                    rc_load(s, snap_slot);
                    return EN_RC_CROSSING_BUDGET;
                }
                memcpy(s->cap_v, s->probe_caps, ncaps * sizeof(double));
                memcpy(s->last_v, s->probe_v, sizeof(s->last_v));
                s->time = tb;
                /* Publish the crossing state exactly as en_rc_sample() sees
                 * it: re-settle the instantaneous algebraization from the
                 * committed capacitor charge, so crossing_voltage_v equals
                 * the sampled voltage at the same time (the event TIME
                 * itself remains defined by the committed trajectory). */
                if (*solves >= budget) {
                    set_diag(diag, "Advance work budget exhausted before "
                                   "crossing settlement");
                    rc_load(s, snap_slot);
                    return EN_RC_WORK_EXCEEDED;
                }
                {
                    EnRcStatus settle = rc_solve_system(s, 1, 0, solves);
                    if (settle != EN_RC_OK) {
                        set_diag(diag, "Algebraic settlement at the crossing "
                                       "failed");
                        return settle;
                    }
                }
                ++s->stats.crossings_total;
                if (out) {
                    out->crossed = 1;
                    out->crossed_node = watch->node;
                    out->crossing_time_s = tb;
                    out->crossing_voltage_v = s->last_v[watch->node];
                    out->crossing_bracket_s = tb - ta;
                }
                snprintf(diag, EN_RC_DIAG_MAX,
                         "Node %u crossed threshold at t=%.17g (bracket %.3g s)",
                         watch->node, tb, tb - ta);
                return EN_RC_CROSSED;
            }
            rc_watch_rearm(s, watch, v1);
            prev_watch_v = v1;
        }
    }
}

void en_rc_options_default(EnRcOptions *options)
{
    if (!options) {
        return;
    }
    options->abs_error_v = 1e-6;
    options->rel_error = 2e-4;
    options->min_step_s = 1e-12;
    options->max_step_s = 1e-3;
    options->initial_step_s = 1e-6;
    options->max_step_rejections = 50;
    options->max_matrix_solves = 200000;
    options->crossing_tol_s = 1e-9;
    options->crossing_max_bisections = 60;
}

static EnRcStatus rc_validate_circuit(const EnRcCircuit *circuit, size_t *bad)
{
    *bad = SIZE_MAX;
    if (!circuit || circuit->node_count < 1 ||
        circuit->node_count > EN_RC_MAX_NODES ||
        circuit->element_count > EN_RC_MAX_ELEMENTS) {
        *bad = SIZE_MAX;
        return EN_RC_INVALID;
    }
    for (size_t i = 0; i < circuit->element_count; ++i) {
        const EnRcElement *e = &circuit->elements[i];
        *bad = i;
        if (e->p >= circuit->node_count || e->n >= circuit->node_count ||
            e->p == e->n || !isfinite(e->value)) {
            return EN_RC_INVALID;
        }
        switch (e->kind) {
        case EN_RC_RESISTOR:
            if (e->value <= 0 || !isfinite(1 / e->value)) {
                return EN_RC_INVALID;
            }
            break;
        case EN_RC_DRIVER:
            if (!isfinite(e->resistance) || e->resistance <= 0 ||
                !isfinite(1 / e->resistance)) {
                return EN_RC_INVALID;
            }
            break;
        case EN_RC_CAPACITOR:
            if (e->value <= 0 || !isfinite(1 / e->value) ||
                !isfinite(e->initial_voltage)) {
                return EN_RC_INVALID;
            }
            break;
        case EN_RC_VOLTAGE_SOURCE:
        case EN_RC_CURRENT_SOURCE:
            break;
        default:
            return EN_RC_INVALID;
        }
    }
    *bad = SIZE_MAX;
    return EN_RC_OK;
}

static EnRcStatus rc_validate_options(const EnRcOptions *o)
{
    if (!isfinite(o->abs_error_v) || o->abs_error_v <= 0 ||
        !isfinite(o->rel_error) || o->rel_error < 0 ||
        !isfinite(o->min_step_s) || o->min_step_s <= 0 ||
        !isfinite(o->max_step_s) || o->max_step_s < o->min_step_s ||
        !isfinite(o->initial_step_s) || o->initial_step_s <= 0 ||
        o->initial_step_s > o->max_step_s ||
        o->max_step_rejections < 1 ||
        o->max_matrix_solves < 1 ||
        !isfinite(o->crossing_tol_s) || o->crossing_tol_s <= 0 ||
        o->crossing_max_bisections < 1) {
        return EN_RC_INVALID;
    }
    return EN_RC_OK;
}

/* Validate + commit a new circuit and capacitor remap, then re-settle the
 * instantaneous algebraic solution. On any failure the solver is left
 * exactly as before. Caller has checked solver != NULL. */
static EnRcStatus rc_commit_circuit(EnRcSolver *s, const EnRcCircuit *circuit,
                                    EnRcChargePolicy policy)
{
    size_t bad = SIZE_MAX;
    EnRcStatus status = rc_validate_circuit(circuit, &bad);
    if (status != EN_RC_OK) {
        return status;
    }
    /* Save everything the edit can touch. */
    EnRcCircuit saved_circuit = s->circuit;
    size_t saved_nsrc = s->nsrc, saved_ncaps = s->ncaps;
    size_t saved_cap_element[EN_RC_MAX_ELEMENTS];
    int32_t saved_source_slot[EN_RC_MAX_ELEMENTS];
    unsigned saved_parent[EN_RC_MAX_NODES];
    unsigned char saved_floating[EN_RC_MAX_NODES];
    long double saved_ext[EN_RC_MAX_NODES];
    long double saved_ext_scale[EN_RC_MAX_NODES];
    double saved_caps[EN_RC_MAX_ELEMENTS];
    double saved_last_v[EN_RC_MAX_NODES];
    memcpy(saved_cap_element, s->cap_element, sizeof(saved_cap_element));
    memcpy(saved_source_slot, s->source_slot, sizeof(saved_source_slot));
    memcpy(saved_parent, s->parent, sizeof(saved_parent));
    memcpy(saved_floating, s->floating, sizeof(saved_floating));
    memcpy(saved_ext, s->island_external_a, sizeof(saved_ext));
    memcpy(saved_ext_scale, s->island_external_scale, sizeof(saved_ext_scale));
    memcpy(saved_caps, s->cap_v, sizeof(saved_caps));
    memcpy(saved_last_v, s->last_v, sizeof(saved_last_v));

    s->circuit = *circuit;
    rc_rebuild_topology(s);
    /* Remap capacitor state by ELEMENT index under the explicit policy: a
     * capacitor whose element index was already a capacitor keeps its state
     * (KEEP_CHARGE preserves charge: v = C_old*v_old/C_new); element slots
     * that were not capacitors start at their declared initial voltage.
     * Ordinal positions never participate, so removing an earlier capacitor
     * cannot silently reset an unchanged later one. */
    size_t new_cap_ordinal = 0;
    for (size_t i = 0; i < circuit->element_count; ++i) {
        if (circuit->elements[i].kind != EN_RC_CAPACITOR) {
            continue;
        }
        double v_new = circuit->elements[i].initial_voltage;
        for (size_t k = 0; k < saved_ncaps; ++k) {
            if (saved_cap_element[k] != i) {
                continue;
            }
            if (policy == EN_RC_KEEP_CHARGE) {
                double c_old = saved_circuit.elements[i].value;
                double c_new = circuit->elements[i].value;
                double v_old = saved_caps[k];
                v_new = c_new == c_old ? v_old : (c_old * v_old) / c_new;
            }
            break;
        }
        if (!isfinite(v_new)) {
            /* Restore and reject; charge is never invented. */
            s->circuit = saved_circuit;
            memcpy(s->cap_element, saved_cap_element, sizeof(saved_cap_element));
            memcpy(s->source_slot, saved_source_slot, sizeof(saved_source_slot));
            memcpy(s->parent, saved_parent, sizeof(saved_parent));
            memcpy(s->floating, saved_floating, sizeof(saved_floating));
            memcpy(s->island_external_a, saved_ext, sizeof(saved_ext));
            memcpy(s->island_external_scale, saved_ext_scale,
                   sizeof(saved_ext_scale));
            memcpy(s->cap_v, saved_caps, sizeof(saved_caps));
            memcpy(s->last_v, saved_last_v, sizeof(saved_last_v));
            s->nsrc = saved_nsrc;
            s->ncaps = saved_ncaps;
            return EN_RC_NUMERICAL;
        }
        s->cap_v[new_cap_ordinal] = v_new;
        ++new_cap_ordinal;
    }
    status = rc_check_operating_point(s);
    if (status == EN_RC_OK) {
        status = rc_solve_system(s, 1, 0, NULL);
    }
    if (status != EN_RC_OK) {
        s->circuit = saved_circuit;
        memcpy(s->cap_element, saved_cap_element, sizeof(saved_cap_element));
        memcpy(s->source_slot, saved_source_slot, sizeof(saved_source_slot));
        memcpy(s->parent, saved_parent, sizeof(saved_parent));
        memcpy(s->floating, saved_floating, sizeof(saved_floating));
        memcpy(s->island_external_a, saved_ext, sizeof(saved_ext));
        memcpy(s->island_external_scale, saved_ext_scale, sizeof(saved_ext_scale));
        memcpy(s->cap_v, saved_caps, sizeof(saved_caps));
        memcpy(s->last_v, saved_last_v, sizeof(saved_last_v));
        s->nsrc = saved_nsrc;
        s->ncaps = saved_ncaps;
        return status;
    }
    /* Crossing history belongs to the circuit state: an edit re-arms watches. */
    s->watch_latch_active = 0;
    return EN_RC_OK;
}

EnRcSolver *en_rc_create(const EnRcCircuit *circuit, const EnRcOptions *options,
                         EnRcStatus *status)
{
    EnRcStatus ignored;
    if (!status) {
        status = &ignored;
    }
    *status = EN_RC_INVALID;
    if (!circuit) {
        return NULL;
    }
    EnRcOptions defaults;
    if (!options) {
        en_rc_options_default(&defaults);
        options = &defaults;
    }
    if (rc_validate_options(options) != EN_RC_OK) {
        return NULL;
    }
    const size_t workspace =
        (2 * (size_t)EN_RC_MAX_DIM * EN_RC_MAX_DIM + 4 * EN_RC_MAX_DIM) *
        sizeof(double);
    EnRcSolver *s = calloc(1, sizeof(*s) + workspace);
    if (!s) {
        *status = EN_RC_ALLOCATION_FAILED;
        return NULL;
    }
    double *tail = (double *)(s + 1);
    s->mna_original = tail;
    s->mna_a = tail + (size_t)EN_RC_MAX_DIM * EN_RC_MAX_DIM;
    s->mna_rhs = s->mna_a + (size_t)EN_RC_MAX_DIM * EN_RC_MAX_DIM;
    s->mna_b = s->mna_rhs + EN_RC_MAX_DIM;
    s->mna_x = s->mna_b + EN_RC_MAX_DIM;
    s->options = *options;
    s->time = 0;
    s->stats.step_min_s_total = NAN;
    s->stats.step_max_s_total = NAN;
    size_t bad = SIZE_MAX;
    EnRcStatus status2 = rc_validate_circuit(circuit, &bad);
    if (status2 != EN_RC_OK) {
        free(s);
        *status = status2;
        return NULL;
    }
    s->circuit = *circuit;
    rc_rebuild_topology(s);
    for (size_t k = 0; k < s->ncaps; ++k) {
        s->cap_v[k] = s->circuit.elements[s->cap_element[k]].initial_voltage;
    }
    *status = rc_check_operating_point(s);
    if (*status == EN_RC_OK) {
        *status = rc_solve_system(s, 1, 0, NULL);
    }
    if (*status != EN_RC_OK) {
        free(s);
        return NULL;
    }
    return s;
}

void en_rc_destroy(EnRcSolver *solver)
{
    free(solver);
}

EnRcStatus en_rc_edit(EnRcSolver *solver, const EnRcCircuit *circuit,
                      EnRcChargePolicy policy)
{
    if (!solver || !circuit ||
        (policy != EN_RC_KEEP_CHARGE && policy != EN_RC_RESET_CHARGE)) {
        return EN_RC_INVALID;
    }
    return rc_commit_circuit(solver, circuit, policy);
}

static EnRcStatus rc_cap_ordinal(const EnRcSolver *s, size_t element_index,
                                 size_t *ordinal)
{
    if (!s || element_index >= s->circuit.element_count ||
        s->circuit.elements[element_index].kind != EN_RC_CAPACITOR) {
        return EN_RC_INVALID;
    }
    for (size_t k = 0; k < s->ncaps; ++k) {
        if (s->cap_element[k] == element_index) {
            *ordinal = k;
            return EN_RC_OK;
        }
    }
    return EN_RC_INVALID;
}

EnRcStatus en_rc_set_capacitor_voltage(EnRcSolver *solver, size_t element_index,
                                       double voltage)
{
    size_t ordinal;
    if (!solver || !isfinite(voltage) ||
        rc_cap_ordinal(solver, element_index, &ordinal) != EN_RC_OK) {
        return EN_RC_INVALID;
    }
    solver->cap_v[ordinal] = voltage;
    return EN_RC_OK;
}

EnRcStatus en_rc_set_capacitor_charge(EnRcSolver *solver, size_t element_index,
                                      double charge)
{
    size_t ordinal;
    if (!solver || !isfinite(charge) ||
        rc_cap_ordinal(solver, element_index, &ordinal) != EN_RC_OK) {
        return EN_RC_INVALID;
    }
    double v = charge / solver->circuit.elements[element_index].value;
    if (!isfinite(v)) {
        return EN_RC_NUMERICAL;
    }
    solver->cap_v[ordinal] = v;
    return EN_RC_OK;
}

double en_rc_capacitor_voltage(const EnRcSolver *solver, size_t element_index,
                               EnRcStatus *status)
{
    size_t ordinal;
    EnRcStatus ignored;
    if (!status) {
        status = &ignored;
    }
    *status = EN_RC_INVALID;
    if (rc_cap_ordinal(solver, element_index, &ordinal) != EN_RC_OK) {
        return NAN;
    }
    *status = EN_RC_OK;
    return solver->cap_v[ordinal];
}

double en_rc_capacitor_charge(const EnRcSolver *solver, size_t element_index,
                              EnRcStatus *status)
{
    size_t ordinal;
    EnRcStatus ignored;
    if (!status) {
        status = &ignored;
    }
    *status = EN_RC_INVALID;
    if (rc_cap_ordinal(solver, element_index, &ordinal) != EN_RC_OK) {
        return NAN;
    }
    *status = EN_RC_OK;
    return solver->circuit.elements[element_index].value *
           solver->cap_v[ordinal];
}

EnRcStatus en_rc_reset(EnRcSolver *solver, double time)
{
    if (!solver || !isfinite(time) || time < 0) {
        return EN_RC_INVALID;
    }
    for (size_t k = 0; k < solver->ncaps; ++k) {
        solver->cap_v[k] =
            solver->circuit.elements[solver->cap_element[k]].initial_voltage;
    }
    solver->time = time;
    solver->watch_latch_active = 0;
    return EN_RC_OK;
}

EnRcStatus en_rc_copy_state(EnRcSolver *dst, const EnRcSolver *src)
{
    if (!dst || !src) {
        return EN_RC_INVALID;
    }
    if (dst == src) {
        return EN_RC_OK;
    }
    if (dst->circuit.node_count != src->circuit.node_count ||
        dst->circuit.element_count != src->circuit.element_count ||
        memcmp(&dst->circuit, &src->circuit, sizeof(EnRcCircuit)) != 0 ||
        memcmp(&dst->options, &src->options, sizeof(EnRcOptions)) != 0) {
        return EN_RC_INVALID;
    }
    dst->time = src->time;
    memcpy(dst->cap_v, src->cap_v, sizeof(dst->cap_v));
    dst->watch_latch_active = src->watch_latch_active;
    dst->watch_latch_node = src->watch_latch_node;
    dst->watch_latch_threshold = src->watch_latch_threshold;
    dst->watch_latch_rising = src->watch_latch_rising;
    memcpy(dst->last_v, src->last_v, sizeof(dst->last_v));
    dst->last_max_kcl_residual_a = src->last_max_kcl_residual_a;
    dst->last_max_source_residual_v = src->last_max_source_residual_v;
    return EN_RC_OK;
}

double en_rc_time(const EnRcSolver *solver)
{
    return solver ? solver->time : NAN;
}

size_t en_rc_node_count(const EnRcSolver *solver)
{
    return solver ? solver->circuit.node_count : 0;
}

size_t en_rc_element_count(const EnRcSolver *solver)
{
    return solver ? solver->circuit.element_count : 0;
}

EnRcStatus en_rc_advance(EnRcSolver *solver, double t_target,
                         const EnRcWatch *watch, EnRcAdvance *out)
{
    if (!solver || !out) {
        return EN_RC_INVALID;
    }
    memset(out, 0, sizeof(*out));
    out->time = solver->time;
    out->step_min_s = NAN;
    out->step_max_s = NAN;
    out->crossing_bracket_s = NAN;
    if (!isfinite(t_target) || t_target < solver->time) {
        set_diag(out->diagnostic, "Target time must be finite and not in the past");
        return out->status = EN_RC_INVALID;
    }
    EnRcWatch w;
    if (watch && watch->enabled) {
        w = *watch;
        if (w.node >= solver->circuit.node_count) {
            set_diag(out->diagnostic, "Watch node is outside the circuit");
            return out->status = EN_RC_INVALID;
        }
        if (!isfinite(w.threshold_v) ||
            (unsigned)w.edge > (unsigned)EN_RC_EDGE_FALLING) {
            set_diag(out->diagnostic, "Watch threshold or edge is invalid");
            return out->status = EN_RC_INVALID;
        }
        if (solver->floating[w.node]) {
            set_diag(out->diagnostic,
                     "Watch node voltage is unknown (floating island)");
            return out->status = EN_RC_INVALID;
        }
    } else {
        w.enabled = 0;
    }
    if (!(t_target - solver->time > 0)) {
        set_diag(out->diagnostic, "Zero-length advance");
        return out->status = EN_RC_OK;
    }
    rc_save(solver, 0);
    uint64_t solves = 0;
    if (w.enabled) {
        if (solves + 1 > solver->options.max_matrix_solves) {
            set_diag(out->diagnostic, "Advance work budget exhausted before baseline");
            return out->status = EN_RC_WORK_EXCEEDED;
        }
        EnRcStatus status = rc_solve_system(solver, 1, 0, &solves);
        if (status != EN_RC_OK) {
            rc_load(solver, 0);
            set_diag(out->diagnostic,
                     "Baseline algebraic evaluation failed before advancing");
            return out->status = status;
        }
    }
    RcMarchAccum acc = march_accum_init();
    char diag[EN_RC_DIAG_MAX];
    EnRcStatus status = rc_step_march(solver, t_target, w.enabled ? &w : NULL,
                                      solver->options.max_matrix_solves, &solves,
                                      1, 0, &acc, diag, out);
    out->steps_accepted = acc.steps_accepted;
    out->steps_rejected = acc.steps_rejected;
    out->matrix_solves = solves;
    out->step_min_s = acc.step_min;
    out->step_max_s = acc.step_max;
    out->error_abs_v = acc.last_err;
    out->error_node = acc.last_err_node;
    out->time = solver->time;
    if (status == EN_RC_OK || status == EN_RC_CROSSED) {
        out->status = status;
        if (!diag[0]) {
            set_diag(diag, status == EN_RC_CROSSED
                               ? "Advanced to a watched threshold crossing"
                               : "Advanced to the requested deadline");
        }
        snprintf(out->diagnostic, sizeof(out->diagnostic), "%s", diag);
        return status;
    }
    /* Bounded failure: time and capacitor charge return to call entry. */
    rc_load(solver, 0);
    out->time = solver->time;
    out->status = status;
    snprintf(out->diagnostic, sizeof(out->diagnostic), "%s", diag);
    return status;
}

EnRcStatus en_rc_sample(EnRcSolver *solver, EnRcSample *out)
{
    if (!solver || !out) {
        return EN_RC_INVALID;
    }
    const size_t nodes = solver->circuit.node_count;
    const size_t nrows = nodes - 1;
    memset(out, 0, sizeof(*out));
    out->time = solver->time;
    for (size_t i = 0; i < EN_RC_MAX_NODES; ++i) {
        out->voltage[i] = NAN;
    }
    for (size_t i = 0; i < EN_RC_MAX_ELEMENTS; ++i) {
        out->current[i] = NAN;
    }
    EnRcStatus status = rc_solve_system(solver, 1, 0, NULL);
    if (status != EN_RC_OK) {
        out->status = status;
        set_diag(out->diagnostic,
                 status == EN_RC_SINGULAR
                     ? "Instantaneous algebraization is singular (inconsistent "
                       "ideal constraints at this time)"
                     : "Instantaneous algebraic evaluation failed");
        return status;
    }
    const double *x = solver->mna_x;
    for (size_t i = 0; i < nodes; ++i) {
        out->floating[i] = solver->floating[i];
        out->voltage[i] = solver->last_v[i];
    }
    size_t cap_ordinal = 0;
    for (size_t i = 0; i < solver->circuit.element_count; ++i) {
        const EnRcElement *e = &solver->circuit.elements[i];
        /* Currents come from the raw solution: relative currents in a
         * floating island are observable even when absolute voltages are
         * not published. */
        double vp = e->p ? x[e->p - 1] : 0;
        double vn = e->n ? x[e->n - 1] : 0;
        double current = NAN;
        switch (e->kind) {
        case EN_RC_RESISTOR:
            current = (vp - vn) / e->value;
            break;
        case EN_RC_DRIVER:
            current = (vp - vn - e->value) / e->resistance;
            break;
        case EN_RC_CURRENT_SOURCE:
            current = e->value;
            break;
        case EN_RC_VOLTAGE_SOURCE:
            current = x[nrows + (size_t)solver->source_slot[i]];
            break;
        case EN_RC_CAPACITOR:
            current = x[nrows + solver->nsrc + cap_ordinal];
            ++cap_ordinal;
            break;
        }
        if (!isfinite(current)) {
            out->status = EN_RC_NUMERICAL;
            set_diag(out->diagnostic, "Nonfinite branch current in sample");
            for (size_t k = 0; k < nodes; ++k) {
                out->voltage[k] = NAN;
            }
            return out->status;
        }
        out->current[i] = current;
    }
    out->max_kcl_residual_a = solver->last_max_kcl_residual_a;
    out->max_source_residual_v = solver->last_max_source_residual_v;
    int any_floating = 0;
    for (size_t i = 0; i < nodes; ++i) {
        any_floating |= out->floating[i];
    }
    if (any_floating) {
        out->status = EN_RC_FLOATING;
        set_diag(out->diagnostic,
                 "Floating absolute voltages are unknown; relative quantities solved");
        return out->status;
    }
    out->status = EN_RC_OK;
    set_diag(out->diagnostic, "Instantaneous solution satisfies original equations");
    return out->status;
}

void en_rc_stats(const EnRcSolver *solver, EnRcStats *out)
{
    if (!out) {
        return;
    }
    if (!solver) {
        memset(out, 0, sizeof(*out));
        out->step_min_s_total = NAN;
        out->step_max_s_total = NAN;
        return;
    }
    *out = solver->stats;
}
