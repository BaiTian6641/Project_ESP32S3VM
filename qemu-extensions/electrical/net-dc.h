/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ESP32S3VM_NET_DC_H
#define ESP32S3VM_NET_DC_H

#include <stddef.h>

/* Bounded linear DC primitive. Node 0 is an EXPLICIT caller-supplied reference.
 * No ground, power, leakage, pull-up, or GPIO impedance is inferred from wiring.
 * The project adapter must map stable terminal/net IDs to these local indices.
 * This library does not implement a QEMU pad, timing, capacitance, or ADC. */
#define EN_DC_MAX_NODES 64
#define EN_DC_MAX_ELEMENTS 128

typedef enum EnDcKind {
    EN_DC_RESISTOR,       /* value: resistance in ohms */
    EN_DC_CURRENT_SOURCE, /* value: signed amperes flowing p -> n */
    EN_DC_VOLTAGE_SOURCE, /* value: voltage p minus n; ideal, current solved */
    EN_DC_DRIVER          /* value: Thevenin voltage p minus n, resistance > 0 */
} EnDcKind;

typedef struct EnDcElement {
    EnDcKind kind;
    unsigned p, n;
    double value;
    double resistance; /* Used only for EN_DC_DRIVER. */
} EnDcElement;

typedef struct EnDcCircuit {
    size_t node_count; /* Includes the explicit reference node, at least 1. */
    size_t element_count;
    EnDcElement elements[EN_DC_MAX_ELEMENTS];
} EnDcCircuit;

typedef enum EnDcStatus {
    EN_DC_OK,
    EN_DC_FLOATING,       /* Relative solution exists; absolute voltages unknown. */
    EN_DC_INVALID,
    EN_DC_SINGULAR,       /* Includes conflicting/redundant ideal-source loops. */
    EN_DC_NO_OPERATING_POINT, /* Nonzero net DC current into a floating island. */
    EN_DC_NUMERICAL,
    EN_DC_ALLOCATION_FAILED
} EnDcStatus;

typedef struct EnDcResult {
    EnDcStatus status;
    size_t bad_element; /* SIZE_MAX when the failure has no one element owner. */
    size_t node_count, element_count;
    double voltage[EN_DC_MAX_NODES]; /* NaN on floating nodes or any failure. */
    unsigned char floating[EN_DC_MAX_NODES];
    double current[EN_DC_MAX_ELEMENTS]; /* Signed p -> n, incl. ideal supplies. */
    double max_kcl_residual_a;
    double max_source_residual_v;
    char diagnostic[160];
} EnDcResult;

/* Row-equilibrated dense MNA, deterministic partial pivoting. Limits and default
 * tolerances are part of docs/contracts/electrical-dc.md. Floating nodes never
 * become zero-voltage outputs merely because an internal gauge was selected. */
EnDcStatus en_dc_solve(const EnDcCircuit *circuit, EnDcResult *result);

/* Reusable bounded matrix workspace. Source-only changes reuse the existing
 * row-equilibrated LU factors; every call still checks floating-island current
 * balance and every original KCL/source equation. No allocations on solve. */
typedef struct EnDcWorkspace EnDcWorkspace;
EnDcWorkspace *en_dc_workspace_create(void);
void en_dc_workspace_destroy(EnDcWorkspace *workspace);
EnDcStatus en_dc_workspace_solve(EnDcWorkspace *workspace,
                                const EnDcCircuit *circuit, EnDcResult *result);
unsigned long long en_dc_workspace_factorizations(const EnDcWorkspace *workspace);

typedef enum EnPadLevel {
    EN_PAD_LOW,
    EN_PAD_HIGH,
    EN_PAD_INDETERMINATE,
    EN_PAD_FLOATING,
    EN_PAD_INVALID
} EnPadLevel;

/* Caller supplies thresholds for the actual pad power domain/profile. Out of
 * rail voltages and the threshold gap remain explicit, not coerced to bits. */
EnPadLevel en_dc_classify_pad(double voltage, int floating, double vdd,
                             double low_fraction, double high_fraction);

#endif
