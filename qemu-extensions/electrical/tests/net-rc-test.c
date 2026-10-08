/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "net-rc.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned assertions;
#define CHECK(condition) do { ++assertions; if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); exit(1); \
} } while (0)
#define NEAR(actual, expected, tolerance) \
    CHECK(isfinite(actual) && fabs((actual) - (expected)) <= (tolerance))

static EnRcElement elem(EnRcKind kind, unsigned p, unsigned n, double value)
{
    EnRcElement e;
    memset(&e, 0, sizeof(e));
    e.kind = kind;
    e.p = p;
    e.n = n;
    e.value = value;
    return e;
}

static EnRcElement cap_elem(unsigned p, unsigned n, double c, double v0)
{
    EnRcElement e = elem(EN_RC_CAPACITOR, p, n, c);
    e.initial_voltage = v0;
    return e;
}

static EnRcElement driver(unsigned p, unsigned n, double v, double r)
{
    EnRcElement e = elem(EN_RC_DRIVER, p, n, v);
    e.resistance = r;
    return e;
}

static void default_options(EnRcOptions *o)
{
    en_rc_options_default(o);
}

/* ------------------------------------------------------------------ */
/* 1. Analytic charge/discharge against independent exponentials.      */
static void charge_discharge_analytic(void)
{
    const double tau = 10e-3;
    EnRcCircuit c;
    memset(&c, 0, sizeof(c));
    c.node_count = 2;
    c.elements[0] = driver(1, 0, 1.0, 10e3);
    c.elements[1] = cap_elem(1, 0, 1e-6, 0.0);
    c.element_count = 2;
    EnRcOptions o;
    default_options(&o);
    EnRcStatus st;
    EnRcSolver *s = en_rc_create(&c, &o, &st);
    CHECK(s && st == EN_RC_OK);
    const double marks[] = {0.25, 0.5, 1.0, 2.0, 3.0, 4.0, 5.0};
    double max_err = 0;
    for (size_t i = 0; i < sizeof(marks) / sizeof(marks[0]); ++i) {
        double t = marks[i] * tau;
        EnRcAdvance adv;
        CHECK(en_rc_advance(s, t, NULL, &adv) == EN_RC_OK);
        CHECK(adv.time == t); /* Deadline hit exactly. */
        EnRcSample sm;
        CHECK(en_rc_sample(s, &sm) == EN_RC_OK);
        double expected = 1.0 - exp(-t / tau);
        max_err = fmax(max_err, fabs(sm.voltage[1] - expected));
    }
    printf("RC_EVIDENCE charge max_abs_err_v=%.3e\n", max_err);
    NEAR(max_err, 0.0, 5e-3);

    /* Discharge: force the capacitor to exactly 0.8 V at t = 5 tau, set the
     * source to 0, and follow the independent exponential. */
    CHECK(en_rc_set_capacitor_voltage(s, 1, 0.8) == EN_RC_OK);
    EnRcCircuit d = c;
    d.elements[0].value = 0.0;
    CHECK(en_rc_edit(s, &d, EN_RC_KEEP_CHARGE) == EN_RC_OK);
    const double down_marks[] = {6.0, 7.0, 8.0, 9.0, 10.0};
    max_err = 0;
    for (size_t i = 0; i < sizeof(down_marks) / sizeof(down_marks[0]); ++i) {
        double t = down_marks[i] * tau;
        EnRcAdvance adv;
        CHECK(en_rc_advance(s, t, NULL, &adv) == EN_RC_OK);
        CHECK(adv.time == t);
        EnRcSample sm;
        CHECK(en_rc_sample(s, &sm) == EN_RC_OK);
        double expected = 0.8 * exp(-(t - 5.0 * tau) / tau);
        max_err = fmax(max_err, fabs(sm.voltage[1] - expected));
    }
    printf("RC_EVIDENCE discharge max_abs_err_v=%.3e\n", max_err);
    NEAR(max_err, 0.0, 5e-3);
    en_rc_destroy(s);
}

/* 2. Source step at a deadline, zero-length advance, right-continuity. */
static void source_step_and_zero_time(void)
{
    const double tau = 10e-3, t1 = 5e-3;
    EnRcCircuit c;
    memset(&c, 0, sizeof(c));
    c.node_count = 2;
    c.elements[0] = driver(1, 0, 0.0, 10e3);
    c.elements[1] = cap_elem(1, 0, 1e-6, 0.0);
    c.element_count = 2;
    EnRcOptions o;
    default_options(&o);
    EnRcStatus st;
    EnRcSolver *s = en_rc_create(&c, &o, &st);
    CHECK(s && st == EN_RC_OK);
    EnRcAdvance adv;
    CHECK(en_rc_advance(s, t1, NULL, &adv) == EN_RC_OK);
    CHECK(adv.time == t1);
    /* Zero-length advance: a committed event time must be re-enterable. */
    CHECK(en_rc_advance(s, t1, NULL, &adv) == EN_RC_OK);
    CHECK(adv.time == t1);
    CHECK(adv.steps_accepted == 0);
    EnRcSample sm;
    CHECK(en_rc_sample(s, &sm) == EN_RC_OK);
    NEAR(sm.voltage[1], 0.0, 1e-12);
    NEAR(sm.current[0], 0.0, 1e-15); /* Pre-change source value still committed. */
    EnRcCircuit stepped = c;
    stepped.elements[0].value = 1.0;
    CHECK(en_rc_edit(s, &stepped, EN_RC_KEEP_CHARGE) == EN_RC_OK);
    CHECK(en_rc_sample(s, &sm) == EN_RC_OK);
    NEAR(sm.voltage[1], 0.0, 1e-12); /* Charge is continuous. */
    NEAR(sm.current[0], -1.0 / 10e3, 1e-9); /* Source drives node: p->n negative. */
    double max_err = 0;
    for (double t = 6e-3; t <= 2.5e-2 + 1e-15; t += 1e-3) {
        CHECK(en_rc_advance(s, t, NULL, &adv) == EN_RC_OK);
        CHECK(adv.time == t);
        CHECK(en_rc_sample(s, &sm) == EN_RC_OK);
        double expected = 1.0 - exp(-(t - t1) / tau);
        max_err = fmax(max_err, fabs(sm.voltage[1] - expected));
    }
    printf("RC_EVIDENCE source_step max_abs_err_v=%.3e\n", max_err);
    NEAR(max_err, 0.0, 5e-3);
    en_rc_destroy(s);
}

/* 3. Switched divider: switch modeled as an explicitly edited resistor. */
static void switched_divider(void)
{
    EnRcCircuit c;
    memset(&c, 0, sizeof(c));
    c.node_count = 3;
    c.elements[0] = elem(EN_RC_VOLTAGE_SOURCE, 1, 0, 2.0);
    c.elements[1] = elem(EN_RC_RESISTOR, 1, 2, 10e3);
    c.elements[2] = elem(EN_RC_RESISTOR, 2, 0, 10e3);
    c.elements[3] = cap_elem(2, 0, 100e-9, 0.0);
    c.elements[4] = elem(EN_RC_RESISTOR, 2, 0, 1e9); /* Switch, open. */
    c.element_count = 5;
    EnRcOptions o;
    default_options(&o);
    EnRcStatus st;
    EnRcSolver *s = en_rc_create(&c, &o, &st);
    CHECK(s && st == EN_RC_OK);
    EnRcAdvance adv;
    EnRcSample sm;
    CHECK(en_rc_advance(s, 3e-3, NULL, &adv) == EN_RC_OK);
    CHECK(en_rc_sample(s, &sm) == EN_RC_OK);
    double tau_open = (10e3 / 2.0) * 100e-9;
    double expected = 1.0 + (0.0 - 1.0) * exp(-3e-3 / tau_open);
    printf("RC_EVIDENCE divider_open err_v=%.3e v=%.6f\n",
           fabs(sm.voltage[2] - expected), sm.voltage[2]);
    NEAR(sm.voltage[2], expected, 2e-3);

    EnRcCircuit closed = c;
    closed.elements[4].value = 1.0;
    CHECK(en_rc_edit(s, &closed, EN_RC_KEEP_CHARGE) == EN_RC_OK);
    CHECK(en_rc_advance(s, 6e-3, NULL, &adv) == EN_RC_OK);
    CHECK(en_rc_sample(s, &sm) == EN_RC_OK);
    double rp = 10e3 * 1.0 / (10e3 + 1.0);
    double v_final = 2.0 * rp / (10e3 + rp);
    NEAR(sm.voltage[2], v_final, 1e-6);

    EnRcCircuit reopened = c;
    CHECK(en_rc_edit(s, &reopened, EN_RC_KEEP_CHARGE) == EN_RC_OK);
    CHECK(en_rc_advance(s, 9e-3, NULL, &adv) == EN_RC_OK);
    CHECK(en_rc_sample(s, &sm) == EN_RC_OK);
    double v0 = v_final;
    expected = 1.0 + (v0 - 1.0) * exp(-3e-3 / tau_open);
    printf("RC_EVIDENCE divider_reopen err_v=%.3e\n", fabs(sm.voltage[2] - expected));
    NEAR(sm.voltage[2], expected, 2e-3);
    en_rc_destroy(s);
}

/* 4. PWM through an RC: exact deadlines every edge, recursive analytic
 * reference (each phase solved independently as a pure exponential). */
static void pwm_rc_ripple(void)
{
    const double tau = 10e-3, period = 1e-3, high = 3.3;
    const double t_high = period / 2.0;
    const int periods = 20;
    EnRcCircuit c;
    memset(&c, 0, sizeof(c));
    c.node_count = 2;
    c.elements[0] = driver(1, 0, 0.0, 10e3);
    c.elements[1] = cap_elem(1, 0, 1e-6, 0.0);
    c.element_count = 2;
    EnRcOptions o;
    default_options(&o);
    EnRcStatus st;
    EnRcSolver *s = en_rc_create(&c, &o, &st);
    CHECK(s && st == EN_RC_OK);
    EnRcCircuit hi = c;
    hi.elements[0].value = high;
    EnRcAdvance adv;
    EnRcSample sm;
    double v_ref = 0.0, max_err = 0;
    double k_hi = exp(-t_high / tau), k_lo = exp(-(period - t_high) / tau);
    for (int k = 0; k < periods; ++k) {
        double t0 = k * period;
        /* Commit the rising edge exactly at t0, then advance the phase. */
        CHECK(en_rc_edit(s, &hi, EN_RC_KEEP_CHARGE) == EN_RC_OK);
        CHECK(en_rc_advance(s, t0 + t_high, NULL, &adv) == EN_RC_OK);
        CHECK(adv.time == t0 + t_high);
        double v_rise_end = high + (v_ref - high) * k_hi;
        CHECK(en_rc_edit(s, &c, EN_RC_KEEP_CHARGE) == EN_RC_OK);
        CHECK(en_rc_advance(s, t0 + period, NULL, &adv) == EN_RC_OK);
        CHECK(adv.time == t0 + period);
        v_ref = v_rise_end * k_lo;
        CHECK(en_rc_sample(s, &sm) == EN_RC_OK);
        max_err = fmax(max_err, fabs(sm.voltage[1] - v_ref));
    }
    printf("RC_EVIDENCE pwm max_abs_err_v=%.3e v_final=%.6f ref_final=%.6f\n",
           max_err, sm.voltage[1], v_ref);
    NEAR(max_err, 0.0, 5e-3);
    /* Steady ripple window from the independent recursion. */
    CHECK(sm.voltage[1] > 1.3 && sm.voltage[1] < 1.5);
    en_rc_destroy(s);
}

/* 5. Two-time-constant stiff ladder vs an independent fixed-step
 * backward-Euler reference implemented here (different code path). */
static void two_time_constant_stiff(void)
{
    const double V = 1.0, R1 = 10e3, R2 = 10e3, Ca = 1e-9, Cb = 100e-6;
    const double marks[] = {1e-3, 1e-2, 0.1, 0.5, 1.0, 2.0};
    const size_t nmarks = sizeof(marks) / sizeof(marks[0]);
    double ref_a[8], ref_b[8];
    CHECK(nmarks <= 8);
    {
        /* Reference: fixed h BE on the 2-node ODE, h = 5e-8 s. */
        const double h = 5e-8;
        double va = 0, vb = 0, t = 0;
        double a = 1 / (R1 * Ca) + 1 / (R2 * Ca);
        double b = 1 / (R2 * Ca);
        double cc = 1 / (R2 * Cb);
        double d = 1 / (R2 * Cb);
        size_t mi = 0;
        while (mi < nmarks) {
            if (t >= marks[mi] - h / 2) {
                ref_a[mi] = va;
                ref_b[mi] = vb;
                ++mi;
                continue;
            }
            double a11 = 1 + h * a, a12 = -h * b, a21 = -h * cc, a22 = 1 + h * d;
            double det = a11 * a22 - a12 * a21;
            double r1 = va + h * V / (R1 * Ca), r2 = vb;
            va = (r1 * a22 - a12 * r2) / det;
            vb = (a11 * r2 - a21 * r1) / det;
            t += h;
        }
    }
    EnRcCircuit c;
    memset(&c, 0, sizeof(c));
    c.node_count = 4;
    c.elements[0] = elem(EN_RC_VOLTAGE_SOURCE, 1, 0, V);
    c.elements[1] = elem(EN_RC_RESISTOR, 1, 2, R1);
    c.elements[2] = cap_elem(2, 0, Ca, 0.0);
    c.elements[3] = elem(EN_RC_RESISTOR, 2, 3, R2);
    c.elements[4] = cap_elem(3, 0, Cb, 0.0);
    c.element_count = 5;
    EnRcOptions o;
    default_options(&o);
    EnRcStatus st;
    EnRcSolver *s = en_rc_create(&c, &o, &st);
    CHECK(s && st == EN_RC_OK);
    double max_diff = 0;
    EnRcAdvance adv;
    EnRcSample sm;
    for (size_t i = 0; i < nmarks; ++i) {
        CHECK(en_rc_advance(s, marks[i], NULL, &adv) == EN_RC_OK);
        CHECK(adv.time == marks[i]);
        CHECK(en_rc_sample(s, &sm) == EN_RC_OK);
        max_diff = fmax(max_diff, fabs(sm.voltage[2] - ref_a[i]));
        max_diff = fmax(max_diff, fabs(sm.voltage[3] - ref_b[i]));
    }
    printf("RC_EVIDENCE stiff max_diff_vs_reference_v=%.3e"
           " steps_last=%u rej_last=%u solves_last=%llu\n",
           max_diff, adv.steps_accepted, adv.steps_rejected,
           (unsigned long long)adv.matrix_solves);
    CHECK(max_diff < 0.05);
    NEAR(sm.voltage[3], ref_b[nmarks - 1], 0.02);
    en_rc_destroy(s);
}

/* 6. Near-zero resistance/capacitance: tau = 1e-15 s. */
static void near_zero_time_constants(void)
{
    EnRcCircuit c;
    memset(&c, 0, sizeof(c));
    c.node_count = 2;
    c.elements[0] = driver(1, 0, 1.0, 1e-3);
    c.elements[1] = cap_elem(1, 0, 1e-12, 0.0);
    c.element_count = 2;
    EnRcOptions o;
    default_options(&o);
    o.min_step_s = 1e-18;
    o.initial_step_s = 1e-18;
    o.max_step_s = 1e-12;
    o.abs_error_v = 1e-2;
    o.max_step_rejections = 80;
    EnRcStatus st;
    EnRcSolver *s = en_rc_create(&c, &o, &st);
    CHECK(s && st == EN_RC_OK);
    EnRcAdvance adv;
    EnRcSample sm;
    CHECK(en_rc_advance(s, 0.5e-15, NULL, &adv) == EN_RC_OK);
    CHECK(adv.time == 0.5e-15);
    CHECK(en_rc_sample(s, &sm) == EN_RC_OK);
    double expected = 1.0 - exp(-0.5);
    printf("RC_EVIDENCE near_zero mid_err_v=%.3e v=%.4f steps=%u rej=%u\n",
           fabs(sm.voltage[1] - expected), sm.voltage[1], adv.steps_accepted,
           adv.steps_rejected);
    NEAR(sm.voltage[1], expected, 0.05);
    CHECK(en_rc_advance(s, 1e-12, NULL, &adv) == EN_RC_OK);
    CHECK(en_rc_sample(s, &sm) == EN_RC_OK);
    NEAR(sm.voltage[1], 1.0, 1e-5);
    en_rc_destroy(s);

    /* Default options: h >> tau must still land on the asymptote. */
    default_options(&o);
    s = en_rc_create(&c, &o, &st);
    CHECK(s && st == EN_RC_OK);
    CHECK(en_rc_advance(s, 1e-9, NULL, &adv) == EN_RC_OK);
    CHECK(en_rc_sample(s, &sm) == EN_RC_OK);
    NEAR(sm.voltage[1], 1.0, 1e-6);
    printf("RC_EVIDENCE near_zero asymptote_err_v=%.3e\n",
           fabs(sm.voltage[1] - 1.0));
    en_rc_destroy(s);
}

/* 7. Step-halving: tightening the declared tolerance tightens the error. */
static void step_halving_convergence(void)
{
    const double tau = 10e-3;
    const double marks[] = {1e-2, 2e-2, 3e-2, 4e-2, 5e-2};
    const size_t nmarks = sizeof(marks) / sizeof(marks[0]);
    EnRcCircuit c;
    memset(&c, 0, sizeof(c));
    c.node_count = 2;
    c.elements[0] = driver(1, 0, 1.0, 10e3);
    c.elements[1] = cap_elem(1, 0, 1e-6, 0.0);
    c.element_count = 2;
    double prev_err = INFINITY;
    double first_err = 0;
    for (int level = 0; level < 4; ++level) {
        EnRcOptions o;
        default_options(&o);
        o.abs_error_v = 1e-6 / (double)(1 << level);
        o.rel_error = 2e-4 / (double)(1 << level);
        EnRcStatus st;
        EnRcSolver *s = en_rc_create(&c, &o, &st);
        CHECK(s && st == EN_RC_OK);
        double max_err = 0;
        EnRcAdvance adv;
        EnRcSample sm;
        for (size_t i = 0; i < nmarks; ++i) {
            CHECK(en_rc_advance(s, marks[i], NULL, &adv) == EN_RC_OK);
            CHECK(en_rc_sample(s, &sm) == EN_RC_OK);
            max_err = fmax(max_err, fabs(sm.voltage[1] - (1.0 - exp(-marks[i] / tau))));
        }
        printf("RC_EVIDENCE convergence tol=%.1e max_abs_err_v=%.3e last_err_est=%.3e\n",
               o.rel_error, max_err, adv.error_abs_v);
        if (level == 0) {
            first_err = max_err;
        }
        CHECK(max_err < prev_err); /* Monotone refinement. */
        prev_err = max_err;
        en_rc_destroy(s);
    }
    /* A first-order method under per-step LTE control refines the global
     * error monotonically (measured ~sqrt scaling, like SPICE reltol);
     * require a clear overall improvement at the tightest bound. */
    printf("RC_EVIDENCE convergence improvement_factor=%.2f\n",
           first_err / prev_err);
    CHECK(prev_err < 0.5 * first_err);
}

/* 8. Floating charged islands: absolute unknown, charge preserved. */
static void floating_charged_island(void)
{
    /* (a) Isolated capacitor island, precharged, no galvanic path at all:
     * differential charge is preserved exactly, absolute stays unknown. */
    EnRcCircuit c;
    memset(&c, 0, sizeof(c));
    c.node_count = 3;
    c.elements[0] = cap_elem(1, 2, 1e-6, 0.8);
    c.element_count = 1;
    EnRcOptions o;
    default_options(&o);
    EnRcStatus st;
    EnRcSolver *s = en_rc_create(&c, &o, &st);
    CHECK(s && st == EN_RC_OK);
    EnRcSample sm;
    CHECK(en_rc_sample(s, &sm) == EN_RC_FLOATING);
    CHECK(sm.floating[1] && sm.floating[2] && !sm.floating[0]);
    CHECK(isnan(sm.voltage[1]) && isnan(sm.voltage[2]));
    CHECK(sm.voltage[0] == 0.0);
    EnRcAdvance adv;
    CHECK(en_rc_advance(s, 1.0, NULL, &adv) == EN_RC_OK);
    CHECK(en_rc_sample(s, &sm) == EN_RC_FLOATING);
    CHECK(isnan(sm.voltage[1]));
    EnRcStatus cst;
    double vc = en_rc_capacitor_voltage(s, 0, &cst);
    CHECK(cst == EN_RC_OK);
    printf("RC_EVIDENCE floating_island vc_after_1s=%.17g\n", vc);
    CHECK(vc == 0.8); /* Charge preserved bit-exactly, no invented reference. */
    CHECK(isnan(en_rc_capacitor_charge(s, 99, &cst)) && cst == EN_RC_INVALID);

    /* Watch on a floating node must be rejected, not silently resolved. */
    EnRcWatch w;
    memset(&w, 0, sizeof(w));
    w.enabled = 1;
    w.node = 1;
    w.threshold_v = 0.4;
    w.edge = EN_RC_EDGE_ANY;
    CHECK(en_rc_advance(s, 2.0, &w, &adv) == EN_RC_INVALID);
    en_rc_destroy(s);

    /* (a2) A resistor across the floating capacitor closes a real loop: the
     * observable differential voltage decays analytically even though the
     * absolute potentials remain unknown. */
    memset(&c, 0, sizeof(c));
    c.node_count = 3;
    c.elements[0] = elem(EN_RC_RESISTOR, 1, 2, 1e3);
    c.elements[1] = cap_elem(1, 2, 1e-6, 0.8);
    c.element_count = 2;
    s = en_rc_create(&c, &o, &st);
    CHECK(s && st == EN_RC_OK);
    CHECK(en_rc_advance(s, 1e-3, NULL, &adv) == EN_RC_OK);
    vc = en_rc_capacitor_voltage(s, 1, &cst);
    CHECK(cst == EN_RC_OK);
    printf("RC_EVIDENCE floating_loop vc_1tau=%.6f expected=%.6f\n", vc,
           0.8 * exp(-1.0));
    NEAR(vc, 0.8 * exp(-1.0), 5e-3);
    CHECK(en_rc_sample(s, &sm) == EN_RC_FLOATING);
    en_rc_destroy(s);

    /* (b) Current-source integrator needs a return path: grounded RC. */
    memset(&c, 0, sizeof(c));
    c.node_count = 2;
    c.elements[0] = elem(EN_RC_RESISTOR, 1, 0, 1e6);
    c.elements[1] = cap_elem(1, 0, 1e-6, 0.0);
    c.elements[2] = elem(EN_RC_CURRENT_SOURCE, 0, 1, 1e-6);
    c.element_count = 3;
    s = en_rc_create(&c, &o, &st);
    CHECK(s && st == EN_RC_OK);
    CHECK(en_rc_advance(s, 1.0, NULL, &adv) == EN_RC_OK);
    vc = en_rc_capacitor_voltage(s, 1, &cst);
    CHECK(cst == EN_RC_OK);
    double expected = 1e-6 * 1e6 * (1.0 - exp(-1.0));
    printf("RC_EVIDENCE integrator vc=%.6f expected=%.6f\n", vc, expected);
    NEAR(vc, expected, 5e-3);
    CHECK(en_rc_sample(s, &sm) == EN_RC_OK);
    en_rc_destroy(s);

    /* (c) Net external current into an unreferenced island has no solution:
     * internal branch currents cancel in the island KCL sum and internal
     * capacitors cannot absorb common-mode charge. Rejected whether the
     * injection lands on the gauge node or any other island node. */
    memset(&c, 0, sizeof(c));
    c.node_count = 3;
    c.elements[0] = elem(EN_RC_RESISTOR, 1, 2, 1e6);
    c.elements[1] = cap_elem(1, 2, 1e-6, 0.55);
    c.elements[2] = elem(EN_RC_CURRENT_SOURCE, 0, 1, 1e-6); /* Into gauge node 1. */
    c.element_count = 3;
    CHECK(en_rc_create(&c, &o, &st) == NULL && st == EN_RC_NO_OPERATING_POINT);
    c.elements[2].n = 2; /* Same magnitude, non-gauge island node. */
    CHECK(en_rc_create(&c, &o, &st) == NULL && st == EN_RC_NO_OPERATING_POINT);
    c.elements[2].value = 1e-12; /* Below the declared imbalance tolerance. */
    s = en_rc_create(&c, &o, &st);
    CHECK(s && st == EN_RC_OK);
    CHECK(en_rc_advance(s, 1e-3, NULL, &adv) == EN_RC_OK);
    CHECK(en_rc_sample(s, &sm) == EN_RC_FLOATING);
    en_rc_destroy(s);

    /* (d) A current source with BOTH terminals inside the island is
     * balanced: allowed, and the observable differential voltage follows
     * the analytic loop solution vc(t) = I*R*(1-exp(-t/RC)). */
    memset(&c, 0, sizeof(c));
    c.node_count = 3;
    c.elements[0] = elem(EN_RC_RESISTOR, 1, 2, 1e6);
    c.elements[1] = cap_elem(1, 2, 1e-6, 0.0);
    c.elements[2] = elem(EN_RC_CURRENT_SOURCE, 2, 1, 1e-6);
    c.element_count = 3;
    s = en_rc_create(&c, &o, &st);
    CHECK(s && st == EN_RC_OK);
    CHECK(en_rc_advance(s, 1.0, NULL, &adv) == EN_RC_OK);
    CHECK(en_rc_sample(s, &sm) == EN_RC_FLOATING);
    vc = en_rc_capacitor_voltage(s, 1, &cst);
    CHECK(cst == EN_RC_OK);
    expected = 1e-6 * 1e6 * (1.0 - exp(-1.0));
    printf("RC_EVIDENCE floating_internal_source vc=%.6f expected=%.6f\n", vc,
           expected);
    NEAR(vc, expected, 5e-3);
    en_rc_destroy(s);
}

/* 9. Inconsistent algebraic constraints are rejected, never repaired. */
static void ideal_source_conflicts(void)
{
    EnRcOptions o;
    default_options(&o);
    EnRcStatus st;
    EnRcCircuit c;

    memset(&c, 0, sizeof(c));
    c.node_count = 2;
    c.elements[0] = elem(EN_RC_VOLTAGE_SOURCE, 1, 0, 1.0);
    c.elements[1] = elem(EN_RC_VOLTAGE_SOURCE, 1, 0, 2.0);
    c.element_count = 2;
    CHECK(en_rc_create(&c, &o, &st) == NULL && st == EN_RC_SINGULAR);

    memset(&c, 0, sizeof(c));
    c.node_count = 2;
    c.elements[0] = elem(EN_RC_VOLTAGE_SOURCE, 1, 0, 1.0);
    c.elements[1] = elem(EN_RC_VOLTAGE_SOURCE, 1, 0, 1.0);
    c.element_count = 2;
    CHECK(en_rc_create(&c, &o, &st) == NULL && st == EN_RC_SINGULAR);

    /* Capacitor ganged to an ideal source: instantaneous branch current is
     * indeterminate; rejected regardless of the stored voltage. */
    memset(&c, 0, sizeof(c));
    c.node_count = 2;
    c.elements[0] = elem(EN_RC_VOLTAGE_SOURCE, 1, 0, 1.0);
    c.elements[1] = cap_elem(1, 0, 1e-6, 1.0);
    c.element_count = 2;
    CHECK(en_rc_create(&c, &o, &st) == NULL && st == EN_RC_SINGULAR);

    /* Current source into a capacitor-less floating island: no observable
     * charge destination, exactly the DC kernel's missing return path. */
    memset(&c, 0, sizeof(c));
    c.node_count = 3;
    c.elements[0] = elem(EN_RC_RESISTOR, 1, 2, 1e3);
    c.elements[1] = elem(EN_RC_CURRENT_SOURCE, 0, 1, 1e-6);
    c.element_count = 2;
    CHECK(en_rc_create(&c, &o, &st) == NULL && st == EN_RC_NO_OPERATING_POINT);

    /* The finite (Thevenin) form of the same circuit is fine. */
    memset(&c, 0, sizeof(c));
    c.node_count = 2;
    c.elements[0] = driver(1, 0, 1.0, 10e3);
    c.elements[1] = cap_elem(1, 0, 1e-6, 0.0);
    c.element_count = 2;
    EnRcSolver *s = en_rc_create(&c, &o, &st);
    CHECK(s && st == EN_RC_OK);
    EnRcSample sm;
    CHECK(en_rc_sample(s, &sm) == EN_RC_OK);
    en_rc_destroy(s);
}

/* 10. Bounded numerical failure modes leave state untouched. */
static void bounded_numerical_failures(void)
{
    EnRcCircuit c;
    memset(&c, 0, sizeof(c));
    c.node_count = 2;
    c.elements[0] = driver(1, 0, 1.0, 10e3);
    c.elements[1] = cap_elem(1, 0, 1e-6, 0.0);
    c.element_count = 2;
    EnRcAdvance adv;
    EnRcSample sm;

    /* Rejection budget. */
    EnRcOptions o;
    default_options(&o);
    o.abs_error_v = 1e-15;
    o.rel_error = 0.0;
    o.max_step_rejections = 3;
    EnRcStatus st;
    EnRcSolver *s = en_rc_create(&c, &o, &st);
    CHECK(s && st == EN_RC_OK);
    CHECK(en_rc_advance(s, 1e-3, NULL, &adv) == EN_RC_REJECTED);
    CHECK(adv.time == 0.0);
    CHECK(adv.diagnostic[0] != '\0');
    CHECK(en_rc_capacitor_voltage(s, 1, &st) == 0.0);
    en_rc_destroy(s);

    /* Minimum-step floor. */
    default_options(&o);
    o.abs_error_v = 1e-15;
    o.min_step_s = 1e-4;
    o.initial_step_s = 1e-5;
    s = en_rc_create(&c, &o, &st);
    CHECK(s && st == EN_RC_OK);
    CHECK(en_rc_advance(s, 1e-3, NULL, &adv) == EN_RC_STEP_MIN);
    CHECK(adv.time == 0.0);
    en_rc_destroy(s);

    /* Work budget per advance call. */
    default_options(&o);
    o.max_matrix_solves = 5;
    s = en_rc_create(&c, &o, &st);
    CHECK(s && st == EN_RC_OK);
    CHECK(en_rc_advance(s, 2e-6, NULL, &adv) == EN_RC_WORK_EXCEEDED);
    CHECK(adv.time == 0.0);
    CHECK(adv.matrix_solves == 3);
    en_rc_destroy(s);

    /* Crossing bisection budget. */
    default_options(&o);
    o.crossing_tol_s = 1e-15;
    o.crossing_max_bisections = 5;
    s = en_rc_create(&c, &o, &st);
    CHECK(s && st == EN_RC_OK);
    EnRcWatch w;
    memset(&w, 0, sizeof(w));
    w.enabled = 1;
    w.node = 1;
    w.threshold_v = 0.3;
    w.edge = EN_RC_EDGE_RISING;
    CHECK(en_rc_advance(s, 5e-2, &w, &adv) == EN_RC_CROSSING_BUDGET);
    CHECK(adv.time == 0.0);
    CHECK(en_rc_capacitor_voltage(s, 1, &st) == 0.0);
    en_rc_destroy(s);

    /* Half-step underflow at the subnormal edge. */
    default_options(&o);
    s = en_rc_create(&c, &o, &st);
    CHECK(s && st == EN_RC_OK);
    CHECK(en_rc_advance(s, 5e-324, NULL, &adv) == EN_RC_STEP_UNDERFLOW);
    CHECK(adv.time == 0.0);
    en_rc_destroy(s);

    /* Bad arguments. */
    default_options(&o);
    s = en_rc_create(&c, &o, &st);
    CHECK(s && st == EN_RC_OK);
    CHECK(en_rc_advance(s, NAN, NULL, &adv) == EN_RC_INVALID);
    CHECK(en_rc_advance(s, -1.0, NULL, &adv) == EN_RC_INVALID);
    CHECK(en_rc_advance(s, 1e-3, NULL, &adv) == EN_RC_OK);
    CHECK(en_rc_advance(s, 0.5e-3, NULL, &adv) == EN_RC_INVALID);
    (void)sm;
    en_rc_destroy(s);
}

/* 11. Identical replay: fresh solvers and reset/re-run on one solver. */
#define RC_MAX_CALLS 96
typedef struct RcRunTrace {
    EnRcAdvance calls[RC_MAX_CALLS];
    unsigned call_count;
    EnRcSample final;
    double cap_v;
    double crossing_time;
} RcRunTrace;

static void pwm_crossing_trace(EnRcOptions *o, RcRunTrace *t)
{
    memset(t, 0, sizeof(*t));
    const double period = 1e-3, high = 3.3, t_high = period / 2.0;
    EnRcCircuit c;
    memset(&c, 0, sizeof(c));
    c.node_count = 2;
    c.elements[0] = driver(1, 0, 0.0, 10e3);
    c.elements[1] = cap_elem(1, 0, 1e-6, 0.0);
    c.element_count = 2;
    EnRcStatus st;
    EnRcSolver *s = en_rc_create(&c, o, &st);
    CHECK(s && st == EN_RC_OK);
    EnRcWatch w;
    memset(&w, 0, sizeof(w));
    w.enabled = 1;
    w.node = 1;
    w.threshold_v = 0.5;
    w.edge = EN_RC_EDGE_RISING;
    EnRcCircuit hi = c, lo = c;
    hi.elements[0].value = high;
    for (int k = 0; k < 10; ++k) {
        double t0 = k * period;
        /* Commit the rising edge at t0, then advance the high phase. */
        CHECK(en_rc_edit(s, &hi, EN_RC_KEEP_CHARGE) == EN_RC_OK);
        CHECK(t->call_count < RC_MAX_CALLS);
        EnRcAdvance *adv = &t->calls[t->call_count++];
        EnRcStatus status = en_rc_advance(s, t0 + t_high, &w, adv);
        CHECK(status == EN_RC_OK || status == EN_RC_CROSSED);
        if (status == EN_RC_CROSSED) {
            t->crossing_time = adv->crossing_time_s;
            adv = &t->calls[t->call_count++];
            CHECK(en_rc_advance(s, t0 + t_high, &w, adv) == EN_RC_OK);
        }
        /* Commit the falling edge exactly at the phase deadline. */
        CHECK(en_rc_edit(s, &lo, EN_RC_KEEP_CHARGE) == EN_RC_OK);
        adv = &t->calls[t->call_count++];
        CHECK(en_rc_advance(s, t0 + period, &w, adv) == EN_RC_OK);
    }
    CHECK(en_rc_sample(s, &t->final) == EN_RC_OK);
    EnRcStatus cst;
    t->cap_v = en_rc_capacitor_voltage(s, 1, &cst);
    CHECK(cst == EN_RC_OK);
    en_rc_destroy(s);
}

static void replay_identical(void)
{
    EnRcOptions o;
    default_options(&o);
    RcRunTrace a, b;
    pwm_crossing_trace(&o, &a);
    pwm_crossing_trace(&o, &b);
    CHECK(a.call_count == b.call_count && a.call_count > 0);
    CHECK(memcmp(&a, &b, sizeof(a)) == 0);
    /* The 0.5 V threshold fires exactly once in 10 periods (rise k=3):
     * later rises start above it and early ones never reach it. */
    CHECK(a.crossing_time > 0.003 && a.crossing_time < 0.005);
    printf("RC_EVIDENCE replay calls=%u crossing_t=%.17g identical=yes\n",
           a.call_count, a.crossing_time);

    /* Reset + rerun on the SAME solver must reproduce the advance bytes. */
    EnRcCircuit c;
    memset(&c, 0, sizeof(c));
    c.node_count = 2;
    c.elements[0] = driver(1, 0, 0.0, 10e3);
    c.elements[1] = cap_elem(1, 0, 1e-6, 0.0);
    c.element_count = 2;
    EnRcStatus st;
    EnRcSolver *s = en_rc_create(&c, &o, &st);
    CHECK(s && st == EN_RC_OK);
    EnRcAdvance adv1, adv2;
    CHECK(en_rc_advance(s, 1e-3, NULL, &adv1) == EN_RC_OK);
    CHECK(en_rc_reset(s, 0.0) == EN_RC_OK);
    CHECK(en_rc_time(s) == 0.0);
    EnRcStatus cst;
    CHECK(en_rc_capacitor_voltage(s, 1, &cst) == 0.0);
    CHECK(en_rc_advance(s, 1e-3, NULL, &adv2) == EN_RC_OK);
    CHECK(memcmp(&adv1, &adv2, sizeof(adv1)) == 0);
    en_rc_destroy(s);
}

/* 12. Crossing location vs the analytic exponential crossing. */
static void crossing_accuracy(void)
{
    const double tau = 10e-3;
    EnRcCircuit c;
    memset(&c, 0, sizeof(c));
    c.node_count = 2;
    c.elements[0] = driver(1, 0, 1.0, 10e3);
    c.elements[1] = cap_elem(1, 0, 1e-6, 0.0);
    c.element_count = 2;
    EnRcWatch w;
    memset(&w, 0, sizeof(w));
    w.enabled = 1;
    w.node = 1;
    w.threshold_v = 0.63;
    w.edge = EN_RC_EDGE_RISING;
    double t_star = -tau * log(1.0 - 0.63);
    double errors[2];
    for (int level = 0; level < 2; ++level) {
        EnRcOptions o;
        default_options(&o);
        o.rel_error = level == 0 ? 1e-3 : 1e-5;
        o.crossing_tol_s = level == 0 ? 1e-6 : 1e-9;
        EnRcStatus st;
        EnRcSolver *s = en_rc_create(&c, &o, &st);
        CHECK(s && st == EN_RC_OK);
        EnRcAdvance adv;
        CHECK(en_rc_advance(s, 0.05, &w, &adv) == EN_RC_CROSSED);
        CHECK(adv.crossed && adv.crossed_node == 1);
        CHECK(adv.time == adv.crossing_time_s);
        CHECK(adv.crossing_bracket_s <= o.crossing_tol_s);
        NEAR(adv.crossing_voltage_v, 0.63, 2e-3);
        errors[level] = fabs(adv.crossing_time_s - t_star);
        printf("RC_EVIDENCE crossing_rising rel=%.0e t_err_s=%.3e bracket=%.3e\n",
               o.rel_error, errors[level], adv.crossing_bracket_s);
        CHECK(errors[level] < 1e-3);
        /* The reported crossing voltage is the committed algebraic state:
         * sampling at the crossing time reproduces it bit-for-bit. */
        EnRcSample cms;
        CHECK(en_rc_sample(s, &cms) == EN_RC_OK);
        CHECK(cms.voltage[1] == adv.crossing_voltage_v);
        /* Resume after the digital edge: no event is lost or repeated. */
        CHECK(en_rc_advance(s, 0.05, &w, &adv) == EN_RC_OK);
        CHECK(adv.time == 0.05);
        EnRcStatus cst;
        double v = en_rc_capacitor_voltage(s, 1, &cst);
        NEAR(v, 1.0 - exp(-0.05 / tau), 2e-3);
        en_rc_destroy(s);
    }
    CHECK(errors[1] <= errors[0]); /* Tighter tolerance refines the crossing. */

    /* Falling edge on the discharge branch. */
    EnRcCircuit d = c;
    d.elements[0].value = 0.0;
    EnRcOptions o;
    default_options(&o);
    EnRcStatus st;
    EnRcSolver *s = en_rc_create(&c, &o, &st);
    CHECK(s && st == EN_RC_OK);
    EnRcAdvance adv;
    CHECK(en_rc_advance(s, tau, NULL, &adv) == EN_RC_OK);
    CHECK(en_rc_edit(s, &d, EN_RC_KEEP_CHARGE) == EN_RC_OK);
    w.edge = EN_RC_EDGE_FALLING;
    w.threshold_v = 0.37;
    double v_tau = 1.0 - exp(-1.0);
    double t_fall = tau + tau * log(v_tau / 0.37);
    CHECK(en_rc_advance(s, 0.05, &w, &adv) == EN_RC_CROSSED);
    printf("RC_EVIDENCE crossing_falling t_err_s=%.3e\n",
           fabs(adv.crossing_time_s - t_fall));
    CHECK(fabs(adv.crossing_time_s - t_fall) < 1e-3);
    en_rc_destroy(s);

    /* Budget regression: a bracket that reaches crossing_tol_s exactly on
     * the final allowed bisection (W/2^N <= tol < W/2^(N-1)) is reported,
     * not rejected as a budget failure. */
    {
        EnRcCircuit slow;
        memset(&slow, 0, sizeof(slow));
        slow.node_count = 2;
        slow.elements[0] = driver(1, 0, 1.0, 1e6);
        slow.elements[1] = cap_elem(1, 0, 1e-6, 0.0);
        slow.element_count = 2;
        EnRcOptions so;
        default_options(&so);
        so.initial_step_s = 1e-3; /* Fixed W = 1e-3 step grid. */
        so.max_step_s = 1e-3;
        so.crossing_tol_s = 6e-4; /* 5e-4 <= 6e-4 < 1e-3. */
        so.crossing_max_bisections = 1;
        EnRcStatus st2;
        EnRcSolver *s2 = en_rc_create(&slow, &so, &st2);
        CHECK(s2 && st2 == EN_RC_OK);
        EnRcWatch w2;
        memset(&w2, 0, sizeof(w2));
        w2.enabled = 1;
        w2.node = 1;
        w2.threshold_v = 0.005;
        w2.edge = EN_RC_EDGE_RISING;
        EnRcAdvance adv2;
        CHECK(en_rc_advance(s2, 0.05, &w2, &adv2) == EN_RC_CROSSED);
        CHECK(adv2.crossing_bracket_s <= so.crossing_tol_s);
        /* The reported crossing voltage IS the committed algebraic state:
         * en_rc_sample() at the crossing time reproduces it bit-for-bit. */
        EnRcSample sm2;
        CHECK(en_rc_sample(s2, &sm2) == EN_RC_OK);
        CHECK(sm2.voltage[1] == adv2.crossing_voltage_v);
        printf("RC_EVIDENCE crossing_budget_edge bracket=%.3g t=%.9g\n",
               adv2.crossing_bracket_s, adv2.crossing_time_s);
        en_rc_destroy(s2);
    }
}

/* 13. Topology edits, charge policy and reset. */
static void topology_edit_and_reset_policy(void)
{
    EnRcCircuit c;
    memset(&c, 0, sizeof(c));
    c.node_count = 2;
    c.elements[0] = driver(1, 0, 1.0, 10e3);
    c.elements[1] = cap_elem(1, 0, 1e-6, 0.0);
    c.element_count = 2;
    EnRcOptions o;
    default_options(&o);
    EnRcStatus st;
    EnRcSolver *s = en_rc_create(&c, &o, &st);
    CHECK(s && st == EN_RC_OK);
    EnRcAdvance adv;
    CHECK(en_rc_advance(s, 5e-3, NULL, &adv) == EN_RC_OK);
    EnRcStatus cst;
    double vc = en_rc_capacitor_voltage(s, 1, &cst);
    CHECK(cst == EN_RC_OK);

    /* KEEP_CHARGE: doubling C halves the observable voltage, charge invariant. */
    double q_before = en_rc_capacitor_charge(s, 1, &cst);
    CHECK(cst == EN_RC_OK);
    EnRcCircuit big = c;
    big.elements[1].value = 2e-6;
    CHECK(en_rc_edit(s, &big, EN_RC_KEEP_CHARGE) == EN_RC_OK);
    double vc2 = en_rc_capacitor_voltage(s, 1, &cst);
    CHECK(cst == EN_RC_OK);
    double q_after = en_rc_capacitor_charge(s, 1, &cst);
    CHECK(cst == EN_RC_OK);
    printf("RC_EVIDENCE keep_charge vc=%.17g vc2=%.17g ratio=%.17g dq=%.3e\n",
           vc, vc2, vc2 / vc, fabs(q_after - q_before));
    CHECK(fabs(vc2 - 0.5 * vc) <= 1e-12);
    CHECK(fabs(q_after - q_before) <= 1e-15 + 1e-9 * fabs(q_before));

    /* RESET_CHARGE returns the capacitor to its element initial voltage. */
    EnRcCircuit reset_elems = big;
    reset_elems.elements[1].initial_voltage = 0.25;
    CHECK(en_rc_edit(s, &reset_elems, EN_RC_RESET_CHARGE) == EN_RC_OK);
    CHECK(en_rc_capacitor_voltage(s, 1, &cst) == 0.25);

    /* Failed edits are atomic: bad resistance leaves everything unchanged. */
    EnRcSample before, after;
    CHECK(en_rc_sample(s, &before) == EN_RC_OK);
    EnRcCircuit bad = reset_elems;
    bad.elements[0].value = 1.0; /* Driver with value fine... */
    bad.elements[0].resistance = -1.0; /* ...resistance invalid. */
    CHECK(en_rc_edit(s, &bad, EN_RC_KEEP_CHARGE) == EN_RC_INVALID);
    CHECK(en_rc_sample(s, &after) == EN_RC_OK);
    CHECK(memcmp(&before, &after, sizeof(before)) == 0);
    CHECK(en_rc_time(s) == 5e-3);

    /* Adding a capacitor starts it at its own initial voltage. */
    EnRcCircuit extended = reset_elems;
    extended.node_count = 3;
    extended.elements[2] = elem(EN_RC_RESISTOR, 1, 2, 5e3);
    extended.elements[3] = cap_elem(2, 0, 470e-9, 0.4);
    extended.element_count = 4;
    CHECK(en_rc_edit(s, &extended, EN_RC_KEEP_CHARGE) == EN_RC_OK);
    CHECK(en_rc_capacitor_voltage(s, 3, &cst) == 0.4 && cst == EN_RC_OK);

    /* Reset restores every initial voltage and the requested time. */
    CHECK(en_rc_reset(s, 0.0) == EN_RC_OK);
    CHECK(en_rc_time(s) == 0.0);
    CHECK(en_rc_capacitor_voltage(s, 1, &cst) == 0.25);
    CHECK(en_rc_capacitor_voltage(s, 3, &cst) == 0.4);
    printf("RC_EVIDENCE topology_edit policies=keep,reset atomic=yes\n");
    en_rc_destroy(s);
}

/* 13b. Charge remap is keyed by element index across topology edits. */
static void edit_remap_by_element_index(void)
{
    /* Capacitors at element indices 0 and 2, separated by a resistor
     * element; each capacitor discharges through its own resistor with
     * tau = 1 ms. */
    EnRcCircuit c;
    memset(&c, 0, sizeof(c));
    c.node_count = 3;
    c.elements[0] = cap_elem(1, 0, 1e-6, 0.1);
    c.elements[1] = elem(EN_RC_RESISTOR, 1, 0, 1e3);
    c.elements[2] = cap_elem(2, 0, 1e-6, 0.2);
    c.elements[3] = elem(EN_RC_RESISTOR, 2, 0, 1e3);
    c.element_count = 4;
    EnRcOptions o;
    default_options(&o);
    EnRcStatus st;
    EnRcSolver *s = en_rc_create(&c, &o, &st);
    CHECK(s && st == EN_RC_OK);
    EnRcAdvance adv;
    CHECK(en_rc_advance(s, 1e-3, NULL, &adv) == EN_RC_OK); /* Exactly 1 tau. */
    EnRcStatus cst;
    double vc2 = en_rc_capacitor_voltage(s, 2, &cst);
    CHECK(cst == EN_RC_OK);
    NEAR(vc2, 0.2 * exp(-1.0), 5e-3);

    /* Remove the EARLIER capacitor (element 0): the unchanged capacitor at
     * element 2 must KEEP its charge under KEEP_CHARGE instead of being
     * silently reset to initial_voltage by ordinal-position matching. */
    EnRcCircuit removed = c;
    removed.elements[0] = elem(EN_RC_RESISTOR, 1, 0, 1e3);
    CHECK(en_rc_edit(s, &removed, EN_RC_KEEP_CHARGE) == EN_RC_OK);
    double vc2_after = en_rc_capacitor_voltage(s, 2, &cst);
    CHECK(cst == EN_RC_OK);
    printf("RC_EVIDENCE remap_index vc2=%.17g after_removal=%.17g\n", vc2,
           vc2_after);
    CHECK(vc2_after == vc2); /* Same C: bit-exact preservation. */

    /* Re-adding a capacitor at element 0 starts at its declared initial. */
    EnRcCircuit readd = removed;
    readd.elements[0] = cap_elem(1, 0, 1e-6, 0.5);
    CHECK(en_rc_edit(s, &readd, EN_RC_KEEP_CHARGE) == EN_RC_OK);
    CHECK(en_rc_capacitor_voltage(s, 0, &cst) == 0.5);
    CHECK(en_rc_capacitor_voltage(s, 2, &cst) == vc2);
    en_rc_destroy(s);
}

/* 15. en_rc_copy_state: a reusable probe continued from copied state is
 * bit-identical to the live solver's own advance. */
static void state_copy_probe_equivalence(void)
{
    EnRcCircuit c;
    memset(&c, 0, sizeof(c));
    c.node_count = 2;
    c.elements[0] = driver(1, 0, 1.0, 10e3);
    c.elements[1] = cap_elem(1, 0, 1e-6, 0.0);
    c.element_count = 2;
    EnRcOptions o;
    default_options(&o);
    EnRcStatus st;
    EnRcSolver *live = en_rc_create(&c, &o, &st);
    CHECK(live && st == EN_RC_OK);
    EnRcSolver *probe = en_rc_create(&c, &o, &st);
    CHECK(probe && st == EN_RC_OK);
    EnRcAdvance a1, p1;
    CHECK(en_rc_advance(live, 3e-3, NULL, &a1) == EN_RC_OK);
    CHECK(en_rc_advance(probe, 3e-3, NULL, &p1) == EN_RC_OK);
    CHECK(memcmp(&a1, &p1, sizeof(a1)) == 0);

    /* Diverge the probe's position, then restore live's state into it. */
    CHECK(en_rc_advance(probe, 7e-3, NULL, &p1) == EN_RC_OK);
    EnRcSample live_before, live_after;
    CHECK(en_rc_sample(live, &live_before) == EN_RC_OK);
    CHECK(en_rc_copy_state(probe, live) == EN_RC_OK);
    CHECK(en_rc_time(probe) == en_rc_time(live));
    CHECK(en_rc_sample(live, &live_after) == EN_RC_OK);
    /* The source solver is untouched by the copy. */
    CHECK(memcmp(&live_before, &live_after, sizeof(live_before)) == 0);

    /* Watched continuation from the copied state is bit-identical to the
     * live plain advance: same crossing time, same committed sample. */
    EnRcWatch w;
    memset(&w, 0, sizeof(w));
    w.enabled = 1;
    w.node = 1;
    w.threshold_v = 0.63;
    w.edge = EN_RC_EDGE_RISING;
    EnRcAdvance la, pa;
    CHECK(en_rc_advance(live, 0.05, &w, &la) == EN_RC_CROSSED);
    CHECK(en_rc_advance(probe, 0.05, &w, &pa) == EN_RC_CROSSED);
    CHECK(memcmp(&la, &pa, sizeof(la)) == 0);
    EnRcSample ls, ps;
    CHECK(en_rc_sample(live, &ls) == EN_RC_OK);
    CHECK(en_rc_sample(probe, &ps) == EN_RC_OK);
    CHECK(memcmp(&ls, &ps, sizeof(ls)) == 0);
    printf("RC_EVIDENCE state_copy crossing_t=%.17g identical=yes\n",
           la.crossing_time_s);

    /* Mismatched circuits/options are rejected; identical self-copy is a
     * validated no-op. */
    EnRcOptions off = o;
    off.rel_error = 1e-5;
    EnRcStatus st2;
    EnRcSolver *other = en_rc_create(&c, &off, &st2);
    CHECK(other && st2 == EN_RC_OK);
    CHECK(en_rc_copy_state(other, live) == EN_RC_INVALID);
    CHECK(en_rc_copy_state(NULL, live) == EN_RC_INVALID);
    CHECK(en_rc_copy_state(live, live) == EN_RC_OK);
    en_rc_destroy(other);
    en_rc_destroy(live);
    en_rc_destroy(probe);
}

/* 14. Bounds and input validation. */
static void bounds_and_limits(void)
{
    EnRcOptions o;
    default_options(&o);
    EnRcStatus st;
    EnRcCircuit c;

    memset(&c, 0, sizeof(c));
    c.node_count = EN_RC_MAX_NODES + 1;
    CHECK(en_rc_create(&c, &o, &st) == NULL && st == EN_RC_INVALID);

    memset(&c, 0, sizeof(c));
    c.node_count = 2;
    c.element_count = EN_RC_MAX_ELEMENTS + 1;
    CHECK(en_rc_create(&c, &o, &st) == NULL && st == EN_RC_INVALID);

    memset(&c, 0, sizeof(c));
    c.node_count = 2;
    c.elements[0] = elem(EN_RC_RESISTOR, 1, 5, 1e3); /* Node out of range. */
    c.element_count = 1;
    CHECK(en_rc_create(&c, &o, &st) == NULL && st == EN_RC_INVALID);

    memset(&c, 0, sizeof(c));
    c.node_count = 2;
    c.elements[0] = elem(EN_RC_RESISTOR, 1, 1, 1e3); /* Self loop. */
    c.element_count = 1;
    CHECK(en_rc_create(&c, &o, &st) == NULL && st == EN_RC_INVALID);

    memset(&c, 0, sizeof(c));
    c.node_count = 2;
    c.elements[0] = elem(EN_RC_RESISTOR, 1, 0, 0.0);
    c.element_count = 1;
    CHECK(en_rc_create(&c, &o, &st) == NULL && st == EN_RC_INVALID);

    memset(&c, 0, sizeof(c));
    c.node_count = 2;
    c.elements[0] = cap_elem(1, 0, -1e-6, 0.0);
    c.element_count = 1;
    CHECK(en_rc_create(&c, &o, &st) == NULL && st == EN_RC_INVALID);

    memset(&c, 0, sizeof(c));
    c.node_count = 2;
    c.elements[0] = cap_elem(1, 0, 1e-6, NAN);
    c.element_count = 1;
    CHECK(en_rc_create(&c, &o, &st) == NULL && st == EN_RC_INVALID);

    memset(&c, 0, sizeof(c));
    c.node_count = 2;
    c.elements[0] = driver(1, 0, 1.0, 0.0);
    c.element_count = 1;
    CHECK(en_rc_create(&c, &o, &st) == NULL && st == EN_RC_INVALID);

    /* Options validation. */
    memset(&c, 0, sizeof(c));
    c.node_count = 2;
    c.elements[0] = driver(1, 0, 1.0, 10e3);
    c.elements[1] = cap_elem(1, 0, 1e-6, 0.0);
    c.element_count = 2;
    EnRcOptions bad = o;
    bad.abs_error_v = 0.0;
    CHECK(en_rc_create(&c, &bad, &st) == NULL && st == EN_RC_INVALID);
    bad = o;
    bad.min_step_s = 2e-3;
    bad.max_step_s = 1e-3;
    CHECK(en_rc_create(&c, &bad, &st) == NULL && st == EN_RC_INVALID);
    bad = o;
    bad.initial_step_s = 2e-3;
    CHECK(en_rc_create(&c, &bad, &st) == NULL && st == EN_RC_INVALID);

    /* Lifetime statistics are published and consistent. */
    EnRcSolver *s = en_rc_create(&c, &o, &st);
    CHECK(s && st == EN_RC_OK);
    EnRcAdvance adv;
    CHECK(en_rc_advance(s, 1e-3, NULL, &adv) == EN_RC_OK);
    EnRcStats stats;
    en_rc_stats(s, &stats);
    CHECK(stats.matrix_solves_total >= adv.matrix_solves);
    CHECK(stats.steps_accepted_total >= adv.steps_accepted);
    CHECK(stats.step_max_s_total <= o.max_step_s);
    printf("RC_EVIDENCE stats solves=%llu accepted=%llu step_min=%.3e step_max=%.3e\n",
           (unsigned long long)stats.matrix_solves_total,
           (unsigned long long)stats.steps_accepted_total, stats.step_min_s_total,
           stats.step_max_s_total);
    en_rc_destroy(s);
}

int main(void)
{
    charge_discharge_analytic();
    source_step_and_zero_time();
    switched_divider();
    pwm_rc_ripple();
    two_time_constant_stiff();
    near_zero_time_constants();
    step_halving_convergence();
    floating_charged_island();
    ideal_source_conflicts();
    bounded_numerical_failures();
    replay_identical();
    crossing_accuracy();
    topology_edit_and_reset_policy();
    edit_remap_by_element_index();
    state_copy_probe_equivalence();
    bounds_and_limits();
    printf("RC_EVIDENCE total_checks=%u\n", assertions);
    printf("net-rc: all %u checks passed\n", assertions);
    return 0;
}
