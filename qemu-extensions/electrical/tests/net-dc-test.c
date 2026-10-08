/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "net-dc.h"
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

static void add(EnDcCircuit *c, EnDcKind kind, unsigned p, unsigned n,
                double value, double resistance)
{
    CHECK(c->element_count < EN_DC_MAX_ELEMENTS);
    c->elements[c->element_count++] = (EnDcElement){kind, p, n, value, resistance};
}

static void divider_and_source_currents(void)
{
    EnDcCircuit c = {.node_count = 3};
    EnDcResult r;
    add(&c, EN_DC_VOLTAGE_SOURCE, 1, 0, 3.3, 0);
    add(&c, EN_DC_RESISTOR, 1, 2, 10000, 0);
    add(&c, EN_DC_RESISTOR, 2, 0, 10000, 0);
    CHECK(en_dc_solve(&c, &r) == EN_DC_OK);
    NEAR(r.voltage[2], 1.65, 1e-12);
    NEAR(r.current[0], -0.000165, 1e-15);
    NEAR(r.current[1], 0.000165, 1e-15);
    NEAR(r.current[2], 0.000165, 1e-15);
    CHECK(r.max_kcl_residual_a < 1e-15);
    CHECK(en_dc_classify_pad(r.voltage[2], 0, 3.3, 0.25, 0.75) == EN_PAD_INDETERMINATE);
    /* Unequal divider, independent analytic ratio. */
    c.elements[2].value = 20000;
    CHECK(en_dc_solve(&c, &r) == EN_DC_OK);
    NEAR(r.voltage[2], 2.2, 1e-12);
}

static void loaded_driver_and_contention(void)
{
    EnDcCircuit c = {.node_count = 2};
    EnDcResult r;
    add(&c, EN_DC_DRIVER, 1, 0, 3.3, 40);
    add(&c, EN_DC_RESISTOR, 1, 0, 1000, 0);
    CHECK(en_dc_solve(&c, &r) == EN_DC_OK);
    NEAR(r.voltage[1], 3.3 * 1000 / 1040, 1e-12);
    NEAR(r.current[0], -3.3 / 1040, 1e-15);
    CHECK(en_dc_classify_pad(r.voltage[1], 0, 3.3, 0.25, 0.75) == EN_PAD_HIGH);
    /* Equal-strength opposing drivers: finite voltage AND actual currents. */
    c.elements[0].resistance = 20;
    c.elements[1] = (EnDcElement){EN_DC_DRIVER, 1, 0, 0, 20};
    CHECK(en_dc_solve(&c, &r) == EN_DC_OK);
    NEAR(r.voltage[1], 1.65, 1e-12);
    NEAR(r.current[0], -0.0825, 1e-14);
    NEAR(r.current[1], 0.0825, 1e-14);
    CHECK(en_dc_classify_pad(r.voltage[1], 0, 3.3, 0.25, 0.75) == EN_PAD_INDETERMINATE);
}

static void open_drain_pull_and_release(void)
{
    EnDcCircuit c = {.node_count = 2};
    EnDcResult r;
    /* Explicit 45 kOhm nominal pull model; no hidden pull is inserted. */
    add(&c, EN_DC_DRIVER, 1, 0, 3.3, 45000);
    add(&c, EN_DC_DRIVER, 1, 0, 0, 20);
    CHECK(en_dc_solve(&c, &r) == EN_DC_OK);
    NEAR(r.voltage[1], 3.3 * 20 / 45020, 1e-12);
    CHECK(en_dc_classify_pad(r.voltage[1], 0, 3.3, 0.25, 0.75) == EN_PAD_LOW);
    --c.element_count; /* Open drain HIGH removes the low driver. */
    CHECK(en_dc_solve(&c, &r) == EN_DC_OK);
    NEAR(r.voltage[1], 3.3, 1e-12);
    CHECK(en_dc_classify_pad(r.voltage[1], 0, 3.3, 0.25, 0.75) == EN_PAD_HIGH);
    --c.element_count; /* No pull or drive at all. */
    CHECK(en_dc_solve(&c, &r) == EN_DC_FLOATING);
    CHECK(isnan(r.voltage[1]) && r.floating[1]);
    CHECK(en_dc_classify_pad(r.voltage[1], r.floating[1], 3.3, 0.25, 0.75) == EN_PAD_FLOATING);
}

static void floating_relative_solution_and_current_balance(void)
{
    EnDcCircuit c = {.node_count = 4};
    EnDcResult r;
    add(&c, EN_DC_VOLTAGE_SOURCE, 2, 3, 5, 0);
    add(&c, EN_DC_RESISTOR, 2, 3, 500, 0);
    CHECK(en_dc_solve(&c, &r) == EN_DC_FLOATING);
    CHECK(isnan(r.voltage[1]) && isnan(r.voltage[2]) && isnan(r.voltage[3]));
    NEAR(r.current[0], -0.01, 1e-14);
    NEAR(r.current[1], 0.01, 1e-14);
    add(&c, EN_DC_CURRENT_SOURCE, 2, 0, 0.001, 0);
    CHECK(en_dc_solve(&c, &r) == EN_DC_NO_OPERATING_POINT);
    CHECK(isnan(r.voltage[0]) && isnan(r.current[0]));
    /* Explicit connection to reference supplies a return path. */
    add(&c, EN_DC_RESISTOR, 3, 0, 1000, 0);
    CHECK(en_dc_solve(&c, &r) == EN_DC_FLOATING); /* Node 1 still isolated. */
    NEAR(r.voltage[3], -1, 1e-12);
    NEAR(r.voltage[2], 4, 1e-12);
}

static void current_polarity_and_bridge(void)
{
    EnDcCircuit c = {.node_count = 2};
    EnDcResult r;
    add(&c, EN_DC_CURRENT_SOURCE, 0, 1, 0.002, 0);
    add(&c, EN_DC_RESISTOR, 1, 0, 1000, 0);
    CHECK(en_dc_solve(&c, &r) == EN_DC_OK);
    NEAR(r.voltage[1], 2, 1e-12);
    c.elements[0].value = -0.002;
    CHECK(en_dc_solve(&c, &r) == EN_DC_OK);
    NEAR(r.voltage[1], -2, 1e-12);
    CHECK(en_dc_classify_pad(r.voltage[1], 0, 3.3, 0.25, 0.75) == EN_PAD_INVALID);
    memset(&c, 0, sizeof(c));
    c.node_count = 4;
    add(&c, EN_DC_VOLTAGE_SOURCE, 1, 0, 6, 0);
    add(&c, EN_DC_RESISTOR, 1, 2, 1000, 0);
    add(&c, EN_DC_RESISTOR, 2, 0, 2000, 0);
    add(&c, EN_DC_RESISTOR, 1, 3, 2000, 0);
    add(&c, EN_DC_RESISTOR, 3, 0, 1000, 0);
    add(&c, EN_DC_RESISTOR, 2, 3, 1000, 0);
    CHECK(en_dc_solve(&c, &r) == EN_DC_OK);
    NEAR(r.voltage[2], 24.0 / 7, 1e-12);
    NEAR(r.voltage[3], 18.0 / 7, 1e-12);
    NEAR(r.current[5], 6.0 / 7000, 1e-15);
}

static void large_internal_current_cannot_hide_external_imbalance(void)
{
    EnDcCircuit c = {.node_count = 3};
    EnDcResult r;
    add(&c, EN_DC_VOLTAGE_SOURCE, 1, 2, 3.3, 0);
    add(&c, EN_DC_RESISTOR, 1, 2, 1e-6, 0);
    add(&c, EN_DC_CURRENT_SOURCE, 0, 1, 0.001, 0);
    CHECK(en_dc_solve(&c, &r) == EN_DC_NO_OPERATING_POINT);
    CHECK(isnan(r.voltage[1]) && isnan(r.current[0]));
    add(&c, EN_DC_CURRENT_SOURCE, 2, 0, 0.001, 0);
    CHECK(en_dc_solve(&c, &r) == EN_DC_FLOATING);
    CHECK(isnan(r.voltage[1]) && isnan(r.voltage[2]));
    NEAR(r.current[1], 3300000, 1e-6);
    NEAR(r.current[2], 0.001, 1e-15);
    NEAR(r.current[3], 0.001, 1e-15);
    c.element_count = 2;
    c.elements[0].value = 1e308;
    c.elements[1].value = 1;
    CHECK(en_dc_solve(&c, &r) == EN_DC_NUMERICAL);
    CHECK(isnan(r.voltage[1]) && isnan(r.current[0]));
}

static void singular_invalid_and_clean_failure(void)
{
    EnDcCircuit c = {.node_count = 2};
    EnDcResult r;
    add(&c, EN_DC_VOLTAGE_SOURCE, 1, 0, 3.3, 0);
    CHECK(en_dc_solve(&c, &r) == EN_DC_OK);
    add(&c, EN_DC_VOLTAGE_SOURCE, 1, 0, 5, 0);
    CHECK(en_dc_solve(&c, &r) == EN_DC_SINGULAR);
    CHECK(isnan(r.voltage[1]) && isnan(r.current[0]));
    c.elements[1].value = 3.3;
    CHECK(en_dc_solve(&c, &r) == EN_DC_SINGULAR); /* Ambiguous supply currents. */
    c.elements[1] = (EnDcElement){EN_DC_RESISTOR, 1, 0, 0, 0};
    CHECK(en_dc_solve(&c, &r) == EN_DC_INVALID && r.bad_element == 1);
    c.elements[1].value = NAN;
    CHECK(en_dc_solve(&c, &r) == EN_DC_INVALID);
    c.elements[1] = (EnDcElement){EN_DC_DRIVER, 1, 0, 3.3, -20};
    CHECK(en_dc_solve(&c, &r) == EN_DC_INVALID);
    c.elements[1].resistance = INFINITY;
    CHECK(en_dc_solve(&c, &r) == EN_DC_INVALID);
    c.elements[1] = (EnDcElement){EN_DC_RESISTOR, 2, 0, 1000, 0};
    CHECK(en_dc_solve(&c, &r) == EN_DC_INVALID);
    c.elements[1] = (EnDcElement){(EnDcKind)99, 1, 0, 1, 0};
    CHECK(en_dc_solve(&c, &r) == EN_DC_INVALID);
    c.node_count = EN_DC_MAX_NODES + 1;
    CHECK(en_dc_solve(&c, &r) == EN_DC_INVALID);
    CHECK(en_dc_solve(NULL, &r) == EN_DC_INVALID);
    CHECK(en_dc_solve(&c, NULL) == EN_DC_INVALID);
    CHECK(en_dc_classify_pad(0, 0, NAN, 0.25, 0.75) == EN_PAD_INVALID);
    CHECK(en_dc_classify_pad(1, 0, 3.3, 0.75, 0.25) == EN_PAD_INVALID);
    CHECK(en_dc_classify_pad(4, 0, 3.3, 0.25, 0.75) == EN_PAD_INVALID);
    CHECK(en_dc_classify_pad(0.825, 0, 3.3, 0.25, 0.75) == EN_PAD_LOW);
    CHECK(en_dc_classify_pad(2.475, 0, 3.3, 0.25, 0.75) == EN_PAD_HIGH);
}

static void bounded_network_and_repeatability(void)
{
    EnDcCircuit c = {.node_count = EN_DC_MAX_NODES};
    EnDcResult r, again;
    add(&c, EN_DC_VOLTAGE_SOURCE, 1, 0, 3.3, 0);
    for (unsigned i = 2; i < EN_DC_MAX_NODES; ++i) {
        add(&c, EN_DC_RESISTOR, 1, i, 10000, 0);
        add(&c, EN_DC_RESISTOR, i, 0, 10000, 0);
    }
    while (c.element_count < EN_DC_MAX_ELEMENTS) {
        add(&c, EN_DC_RESISTOR, 2, 3, 10000, 0);
    }
    CHECK(en_dc_solve(&c, &r) == EN_DC_OK);
    for (unsigned i = 2; i < EN_DC_MAX_NODES; ++i) {
        NEAR(r.voltage[i], 1.65, 1e-12);
    }
    for (unsigned repeat = 0; repeat < 100; ++repeat) {
        CHECK(en_dc_solve(&c, &again) == EN_DC_OK);
        CHECK(memcmp(&r, &again, sizeof(r)) == 0);
    }
    /* Reorder elements: topology identity and analytic answers stay equivalent. */
    for (size_t i = 0; i < c.element_count / 2; ++i) {
        EnDcElement temporary = c.elements[i];
        c.elements[i] = c.elements[c.element_count - 1 - i];
        c.elements[c.element_count - 1 - i] = temporary;
    }
    CHECK(en_dc_solve(&c, &again) == EN_DC_OK);
    for (unsigned i = 0; i < EN_DC_MAX_NODES; ++i) {
        NEAR(again.voltage[i], r.voltage[i], 1e-12);
    }
}

int main(void)
{
    divider_and_source_currents();
    loaded_driver_and_contention();
    open_drain_pull_and_release();
    floating_relative_solution_and_current_balance();
    current_polarity_and_bridge();
    large_internal_current_cannot_hide_external_imbalance();
    singular_invalid_and_clean_failure();
    bounded_network_and_repeatability();
    printf("8 analytic/failure scenarios passed (%u checks); no native GPIO/ADC claim\n", assertions);
    return 0;
}
