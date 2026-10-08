/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "net-dc.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EN_PIVOT_TOL 1e-12
#define EN_REL_TOL 1e-9
#define EN_CURRENT_ABS_TOL 1e-12
#define EN_VOLTAGE_ABS_TOL 1e-10

static EnDcStatus fail(EnDcResult *result, EnDcStatus status,
                       size_t element, const char *message)
{
    result->status = status;
    result->bad_element = element;
    snprintf(result->diagnostic, sizeof(result->diagnostic), "%s", message);
    /* Discard partial outputs. An earlier successful solve cannot leak through. */
    for (size_t i = 0; i < EN_DC_MAX_NODES; ++i) {
        result->voltage[i] = NAN;
    }
    for (size_t i = 0; i < EN_DC_MAX_ELEMENTS; ++i) {
        result->current[i] = NAN;
    }
    return status;
}

static unsigned root(unsigned *parent, unsigned node)
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

#define EN_DC_MAX_DIM (EN_DC_MAX_NODES - 1 + EN_DC_MAX_ELEMENTS)
struct EnDcWorkspace {
    EnDcCircuit matrix_key;
    int factored;
    size_t dimension;
    unsigned long long factorizations;
    double storage[2 * EN_DC_MAX_DIM * EN_DC_MAX_DIM + 4 * EN_DC_MAX_DIM];
    double row_scale[EN_DC_MAX_DIM];
    size_t pivot[EN_DC_MAX_DIM];
};

EnDcWorkspace *en_dc_workspace_create(void)
{
    return calloc(1, sizeof(EnDcWorkspace));
}

void en_dc_workspace_destroy(EnDcWorkspace *workspace)
{
    free(workspace);
}

unsigned long long en_dc_workspace_factorizations(const EnDcWorkspace *workspace)
{
    return workspace ? workspace->factorizations : 0;
}

static int same_matrix(const EnDcCircuit *a, const EnDcCircuit *b)
{
    if (a->node_count != b->node_count) {
        return 0;
    }
    size_t ai = 0, bi = 0;
    for (;;) {
        /* Ideal current sources stamp only RHS. Their count, endpoints and
         * values do not change the factorization or conductive islands. */
        while (ai < a->element_count && a->elements[ai].kind == EN_DC_CURRENT_SOURCE) {
            ++ai;
        }
        while (bi < b->element_count && b->elements[bi].kind == EN_DC_CURRENT_SOURCE) {
            ++bi;
        }
        if (ai == a->element_count || bi == b->element_count) {
            return ai == a->element_count && bi == b->element_count;
        }
        const EnDcElement *x = &a->elements[ai++], *y = &b->elements[bi++];
        if (x->kind != y->kind || x->p != y->p || x->n != y->n ||
            (x->kind == EN_DC_RESISTOR && x->value != y->value) ||
            (x->kind == EN_DC_DRIVER && x->resistance != y->resistance)) {
            return 0;
        }
    }
}

static EnDcStatus factor_matrix(EnDcWorkspace *w, double *a, size_t dim)
{
    ++w->factorizations;
    w->factored = 0;
    for (size_t row = 0; row < dim; ++row) {
        double scale = 0;
        for (size_t col = 0; col < dim; ++col) {
            if (!isfinite(a[row * dim + col])) {
                return EN_DC_NUMERICAL;
            }
            scale = fmax(scale, fabs(a[row * dim + col]));
        }
        if (scale == 0) {
            return EN_DC_SINGULAR;
        }
        w->row_scale[row] = scale;
        for (size_t col = 0; col < dim; ++col) {
            a[row * dim + col] /= scale;
        }
    }
    for (size_t col = 0; col < dim; ++col) {
        size_t pivot = col;
        for (size_t row = col + 1; row < dim; ++row) {
            if (fabs(a[row * dim + col]) > fabs(a[pivot * dim + col])) {
                pivot = row;
            }
        }
        w->pivot[col] = pivot;
        if (!isfinite(a[pivot * dim + col])) {
            return EN_DC_NUMERICAL;
        }
        if (fabs(a[pivot * dim + col]) <= EN_PIVOT_TOL) {
            return EN_DC_SINGULAR;
        }
        if (pivot != col) {
            /* Earlier L columns stay in elimination order. RHS operations
             * replay these swaps and eliminations in that same order. */
            for (size_t k = col; k < dim; ++k) {
                double value = a[col * dim + k];
                a[col * dim + k] = a[pivot * dim + k];
                a[pivot * dim + k] = value;
            }
        }
        for (size_t row = col + 1; row < dim; ++row) {
            double multiplier = a[row * dim + col] / a[col * dim + col];
            a[row * dim + col] = multiplier;
            for (size_t k = col + 1; k < dim; ++k) {
                a[row * dim + k] -= multiplier * a[col * dim + k];
            }
        }
    }
    w->factored = 1;
    return EN_DC_OK;
}

static EnDcStatus solve_factored(EnDcWorkspace *w, const double *a,
                                double *b, double *x, size_t dim)
{
    for (size_t row = 0; row < dim; ++row) {
        b[row] /= w->row_scale[row];
        if (!isfinite(b[row])) {
            return EN_DC_NUMERICAL;
        }
    }
    for (size_t col = 0; col < dim; ++col) {
        size_t pivot = w->pivot[col];
        double value = b[col];
        b[col] = b[pivot];
        b[pivot] = value;
        for (size_t row = col + 1; row < dim; ++row) {
            b[row] -= a[row * dim + col] * b[col];
        }
    }
    for (size_t row = dim; row-- > 0;) {
        double value = b[row];
        for (size_t col = row + 1; col < dim; ++col) {
            value -= a[row * dim + col] * x[col];
        }
        x[row] = value / a[row * dim + row];
        if (!isfinite(x[row])) {
            return EN_DC_NUMERICAL;
        }
    }
    return EN_DC_OK;
}

EnDcStatus en_dc_workspace_solve(EnDcWorkspace *workspace,
                                const EnDcCircuit *circuit, EnDcResult *result)
{
    unsigned parent[EN_DC_MAX_NODES];
    int source_index[EN_DC_MAX_ELEMENTS];
    size_t source_count = 0;
    if (!result) {
        return EN_DC_INVALID;
    }
    memset(result, 0, sizeof(*result));
    fail(result, EN_DC_INVALID, SIZE_MAX, "Invalid circuit bounds or reference");
    if (!workspace || !circuit || circuit->node_count < 1 ||
        circuit->node_count > EN_DC_MAX_NODES ||
        circuit->element_count > EN_DC_MAX_ELEMENTS) {
        return result->status;
    }
    result->node_count = circuit->node_count;
    result->element_count = circuit->element_count;
    for (size_t i = 0; i < circuit->node_count; ++i) {
        parent[i] = (unsigned)i;
    }
    for (size_t i = 0; i < circuit->element_count; ++i) {
        const EnDcElement *element = &circuit->elements[i];
        source_index[i] = -1;
        if (element->p >= circuit->node_count || element->n >= circuit->node_count ||
            element->p == element->n || !isfinite(element->value)) {
            return fail(result, EN_DC_INVALID, i, "Invalid endpoints or nonfinite value");
        }
        switch (element->kind) {
        case EN_DC_RESISTOR:
        case EN_DC_DRIVER: {
            double resistance = element->kind == EN_DC_RESISTOR ? element->value : element->resistance;
            if (!isfinite(resistance) || resistance <= 0 || !isfinite(1 / resistance)) {
                return fail(result, EN_DC_INVALID, i, "Resistance must be finite and positive");
            }
            unite(parent, element->p, element->n);
            break;
        }
        case EN_DC_VOLTAGE_SOURCE:
            source_index[i] = (int)source_count++;
            unite(parent, element->p, element->n);
            break;
        case EN_DC_CURRENT_SOURCE:
            /* An ideal current source provides no absolute-voltage reference. */
            break;
        default:
            return fail(result, EN_DC_INVALID, i, "Unsupported element kind");
        }
    }
    int any_floating = 0;
    for (size_t i = 0; i < circuit->node_count; ++i) {
        result->floating[i] = root(parent, (unsigned)i) != root(parent, 0);
        any_floating |= result->floating[i];
    }
    /* An island's external DC injection must balance independently of its
     * internal circulating currents. Huge internal source currents must never
     * inflate the residual budget and hide an impossible floating operating
     * point. Extended precision keeps this bounded input summation accurate. */
    long double external[EN_DC_MAX_NODES] = {0};
    long double external_scale[EN_DC_MAX_NODES] = {0};
    for (size_t i = 0; i < circuit->element_count; ++i) {
        const EnDcElement *element = &circuit->elements[i];
        if (element->kind == EN_DC_CURRENT_SOURCE) {
            unsigned p = root(parent, element->p), n = root(parent, element->n);
            if (p != n) {
                external[p] -= (long double)element->value;
                external[n] += (long double)element->value;
                external_scale[p] += fabsl((long double)element->value);
                external_scale[n] += fabsl((long double)element->value);
            }
        }
    }
    for (size_t i = 1; i < circuit->node_count; ++i) {
        if (result->floating[i] && root(parent, (unsigned)i) == i) {
            if (!isfinite(external[i]) || !isfinite(external_scale[i])) {
                return fail(result, EN_DC_NUMERICAL, SIZE_MAX, "External island-current sum overflowed");
            }
            if (fabsl(external[i]) > EN_CURRENT_ABS_TOL + EN_REL_TOL * external_scale[i]) {
                return fail(result, EN_DC_NO_OPERATING_POINT, SIZE_MAX,
                            "Nonzero external DC current into floating island");
            }
        }
    }
    const size_t nodes = circuit->node_count - 1;
    const size_t dim = nodes + source_count;
    if (dim == 0) {
        result->voltage[0] = 0;
        result->status = EN_DC_OK;
        snprintf(result->diagnostic, sizeof(result->diagnostic), "Explicit reference only");
        return result->status;
    }
    /* Fixed workspace; factors survive source-only changes. */
    double *original = workspace->storage;
    double *a = original + dim * dim;
    double *rhs = a + dim * dim;
    double *b = rhs + dim;
    double *x = b + dim;
    int reuse = workspace->factored && workspace->dimension == dim &&
                same_matrix(&workspace->matrix_key, circuit);
    if (!reuse) {
        memset(original, 0, dim * dim * sizeof(double));
    }
    memset(rhs, 0, 3 * dim * sizeof(double));
    for (size_t i = 0; i < circuit->element_count; ++i) {
        const EnDcElement *element = &circuit->elements[i];
        unsigned p = element->p, n = element->n;
        switch (element->kind) {
        case EN_DC_RESISTOR:
            if (!reuse) {
                stamp_conductance(original, dim, p, n, 1 / element->value);
            }
            break;
        case EN_DC_DRIVER:
            if (!reuse) {
                stamp_conductance(original, dim, p, n, 1 / element->resistance);
            }
            stamp_rhs(rhs, p, n, element->value / element->resistance);
            break;
        case EN_DC_CURRENT_SOURCE:
            stamp_rhs(rhs, p, n, -element->value);
            break;
        case EN_DC_VOLTAGE_SOURCE: {
            size_t row = nodes + (size_t)source_index[i];
            if (!reuse && p) {
                original[(p - 1) * dim + row] += 1;
                original[row * dim + p - 1] += 1;
            }
            if (!reuse && n) {
                original[(n - 1) * dim + row] -= 1;
                original[row * dim + n - 1] -= 1;
            }
            rhs[row] = element->value;
            break;
        }
        }
    }
    if (!reuse) {
        memcpy(a, original, dim * dim * sizeof(double));
    }
    memcpy(b, rhs, dim * sizeof(double));
    /* One internal gauge per floating conductive island. Preserve ORIGINAL
     * equations for the post-solve current-balance check (no fabricated ground). */
    for (size_t i = 1; i < circuit->node_count; ++i) {
        if (result->floating[i] && root(parent, (unsigned)i) == i) {
            if (!reuse) {
                memset(a + (i - 1) * dim, 0, dim * sizeof(double));
                a[(i - 1) * dim + i - 1] = 1;
            }
            b[i - 1] = 0;
        }
    }
    EnDcStatus status = EN_DC_OK;
    if (!reuse) {
        status = factor_matrix(workspace, a, dim);
        if (status == EN_DC_OK) {
            workspace->matrix_key = *circuit;
            workspace->dimension = dim;
        }
    }
    if (status == EN_DC_OK) {
        status = solve_factored(workspace, a, b, x, dim);
    }
    if (status != EN_DC_OK) {
        return fail(result, status, SIZE_MAX,
                    status == EN_DC_SINGULAR ? "Singular or ill-conditioned ideal-source/topology system" :
                                             "Nonfinite matrix or numerical solution");
    }
    for (size_t row = 0; row < dim; ++row) {
        double value = 0, scale = fabs(rhs[row]);
        for (size_t col = 0; col < dim; ++col) {
            double contribution = original[row * dim + col] * x[col];
            value += contribution;
            scale += fabs(contribution);
        }
        double residual = fabs(value - rhs[row]);
        double absolute = row < nodes ? EN_CURRENT_ABS_TOL : EN_VOLTAGE_ABS_TOL;
        if (row < nodes) {
            result->max_kcl_residual_a = fmax(result->max_kcl_residual_a, residual);
        } else {
            result->max_source_residual_v = fmax(result->max_source_residual_v, residual);
        }
        if (!isfinite(residual) || !isfinite(scale) || residual > absolute + EN_REL_TOL * scale) {
            /* Physical island-current balance passed independently already.
             * Overflow/convergence must not be mislabeled as a missing return. */
            return fail(result, EN_DC_NUMERICAL, SIZE_MAX,
                        "Original equation residual was nonfinite or exceeded tolerance");
        }
    }
    for (size_t i = 0; i < circuit->element_count; ++i) {
        const EnDcElement *element = &circuit->elements[i];
        double vp = element->p ? x[element->p - 1] : 0;
        double vn = element->n ? x[element->n - 1] : 0;
        switch (element->kind) {
        case EN_DC_RESISTOR:
            result->current[i] = (vp - vn) / element->value;
            break;
        case EN_DC_DRIVER:
            result->current[i] = (vp - vn - element->value) / element->resistance;
            break;
        case EN_DC_CURRENT_SOURCE:
            result->current[i] = element->value;
            break;
        case EN_DC_VOLTAGE_SOURCE:
            result->current[i] = x[nodes + (size_t)source_index[i]];
            break;
        }
        if (!isfinite(result->current[i])) {
            return fail(result, EN_DC_NUMERICAL, i, "Nonfinite branch current");
        }
    }
    result->voltage[0] = 0;
    for (size_t i = 1; i < circuit->node_count; ++i) {
        result->voltage[i] = result->floating[i] ? NAN : x[i - 1];
    }
    result->status = any_floating ? EN_DC_FLOATING : EN_DC_OK;
    result->bad_element = SIZE_MAX;
    snprintf(result->diagnostic, sizeof(result->diagnostic), "%s",
             any_floating ? "Floating absolute voltages are unknown; relative currents solved" :
                            "Linear DC solution satisfies original equations");
    return result->status;
}

EnDcStatus en_dc_solve(const EnDcCircuit *circuit, EnDcResult *result)
{
    EnDcWorkspace *workspace = en_dc_workspace_create();
    if (!workspace) {
        if (!result) {
            return EN_DC_INVALID;
        }
        memset(result, 0, sizeof(*result));
        return fail(result, EN_DC_ALLOCATION_FAILED, SIZE_MAX,
                    "Matrix workspace allocation failed");
    }
    EnDcStatus status = en_dc_workspace_solve(workspace, circuit, result);
    en_dc_workspace_destroy(workspace);
    return status;
}

EnPadLevel en_dc_classify_pad(double voltage, int floating, double vdd,
                             double low_fraction, double high_fraction)
{
    if (!isfinite(vdd) || vdd <= 0 || !isfinite(low_fraction) || !isfinite(high_fraction) ||
        low_fraction < 0 || high_fraction > 1 || low_fraction >= high_fraction) {
        return EN_PAD_INVALID;
    }
    if (floating) {
        return EN_PAD_FLOATING;
    }
    if (!isfinite(voltage) || voltage < 0 || voltage > vdd) {
        return EN_PAD_INVALID;
    }
    if (voltage <= low_fraction * vdd) {
        return EN_PAD_LOW;
    }
    if (voltage >= high_fraction * vdd) {
        return EN_PAD_HIGH;
    }
    return EN_PAD_INDETERMINATE;
}
