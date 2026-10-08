/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ESP32S3VM_NET_ADAPTER_H
#define ESP32S3VM_NET_ADAPTER_H

#include <stddef.h>
#include <limits.h>

#include "net-dc.h"
#include "net-rc.h"

/* Strict v3 project-graph -> linear DC primitive adapter (H-NET-01).
 *
 * The input ABI below mirrors the connectivity projection of a v3 project
 * document (docs/contracts/project.schema.json): components with typed
 * parameters, terminals with stable opaque IDs, nets with endpoint lists.
 * It is a plain Qt-free C ABI; the caller keeps ownership of all pointed-to
 * storage for the duration of the en_adpt_solve call. Geometry is NOT part of
 * this ABI and never contributes connectivity.
 *
 * Strictness contract: every construct the DC kernel cannot represent
 * (capacitors, generic devices, closed switches without an explicit
 * on_resistance, analog quantities without a defined primitive) fails with an
 * explicit status and a diagnostic naming the offending ID. Nothing is
 * silently dropped and no ground, supply, pull or leakage is inferred.
 * Floating absolute voltages stay NaN; the solver's failure statuses pass
 * through unchanged. Empty nets have no node; one-endpoint nets and unwired
 * terminals remain distinct physical nodes, never hidden wires to ground.
 *
 * ABI version: bump EN_ADPT_ABI_VERSION whenever a struct below changes size,
 * field order or meaning. Callers must pass abi_version == EN_ADPT_ABI_VERSION. */
#define EN_ADPT_ABI_VERSION 2u

/* Matches the v3 id grammar: 1..64 chars of [A-Za-z0-9_.:/-], first char
 * alphanumeric, NUL-terminated, no embedded NUL (schema-level transport rule;
 * enforced here because endpoint resolution and diagnostics depend on it). */
#define EN_ADPT_ID_MAX 64

/* Input counts above these fail with EN_ADPT_EXCEEDS_LIMITS. They bound the
 * adapter's own internal tables; the kernel's EN_DC_MAX_NODES (64) and
 * EN_DC_MAX_ELEMENTS (128) bounds still apply to the projection itself. */
#define EN_ADPT_MAX_COMPONENTS 256
#define EN_ADPT_MAX_NETS 256
#define EN_ADPT_MAX_TERMINALS 512

#define EN_ADPT_DIAGNOSTIC_MAX 256

typedef struct EnAdptId {
    char text[EN_ADPT_ID_MAX + 1];
} EnAdptId;

/* Numeric payload plus the v3 unit dimension. State units carry their meaning
 * in the unit discriminant; `value` is ignored for EN_ADPT_UNIT_STATE_OPEN and
 * EN_ADPT_UNIT_STATE_CLOSED. SI scale factors (m, k, M) are applied here, so
 * the kernel always receives volts, amperes and ohms. */
typedef enum EnAdptUnit {
    EN_ADPT_UNIT_V,
    EN_ADPT_UNIT_MV,
    EN_ADPT_UNIT_A,
    EN_ADPT_UNIT_MA,
    EN_ADPT_UNIT_OHM,
    EN_ADPT_UNIT_KOHM,
    EN_ADPT_UNIT_MOHM,
    EN_ADPT_UNIT_RATIO,      /* 0..1, e.g. potentiometer position */
    EN_ADPT_UNIT_PERCENT,    /* 0..100 */
    EN_ADPT_UNIT_STATE_OPEN,
    EN_ADPT_UNIT_STATE_CLOSED,
    EN_ADPT_UNIT_F,
    EN_ADPT_UNIT_UF,
    EN_ADPT_UNIT_NF,
    EN_ADPT_UNIT_PF,
    EN_ADPT_UNIT_COUNT
} EnAdptUnit;

typedef struct EnAdptQuantity {
    double value;
    EnAdptUnit unit;
} EnAdptQuantity;

typedef struct EnAdptParameter {
    const char *name; /* Must be non-NULL; exact case-sensitive match. */
    EnAdptQuantity quantity;
} EnAdptParameter;

typedef enum EnAdptTerminalDomain {
    EN_ADPT_DOMAIN_DIGITAL,
    EN_ADPT_DOMAIN_ANALOG,
    EN_ADPT_DOMAIN_POWER,
    EN_ADPT_DOMAIN_GROUND,
    EN_ADPT_DOMAIN_PASSIVE,
    EN_ADPT_DOMAIN_UNSPECIFIED
} EnAdptTerminalDomain;

typedef enum EnAdptTerminalDirection {
    EN_ADPT_DIRECTION_INPUT,
    EN_ADPT_DIRECTION_OUTPUT,
    EN_ADPT_DIRECTION_INOUT,
    EN_ADPT_DIRECTION_PASSIVE,
    EN_ADPT_DIRECTION_UNSPECIFIED
} EnAdptTerminalDirection;

/* Explicit GPIO-style driver state (Thevenin source). Never inferred: the
 * caller states voltage, impedance and the return rail terminal. An
 * open-drain release is expressed as EN_ADPT_DRIVE_NONE — the adapter then
 * inserts no high driver, per the electrical-dc contract. There is no
 * implicit pull, supply or leakage anywhere in this ABI. */
typedef enum EnAdptDrive {
    EN_ADPT_DRIVE_NONE = 0,
    EN_ADPT_DRIVE_PUSH_PULL,
    EN_ADPT_DRIVE_OPEN_DRAIN /* Active low only; the low side is explicit. */
} EnAdptDrive;

typedef struct EnAdptDriverState {
    EnAdptDrive drive;
    double voltage_v;      /* Thevenin voltage: pad minus return terminal. */
    double impedance_ohm;  /* Explicit output impedance, finite and > 0. */
    const char *return_id; /* Opaque ID of the rail terminal (typically the
                            * MCU's own VDD/GND pin); required when
                            * drive != EN_ADPT_DRIVE_NONE. */
} EnAdptDriverState;

typedef struct EnAdptTerminal {
    EnAdptId id;
    const char *role; /* v3 role text: "a"/"b"/"p"/"n"/"w"/"ref" or free-form. */
    EnAdptTerminalDomain domain;
    EnAdptTerminalDirection direction;
    EnAdptDriverState driver; /* Only meaningful on MCU components. */
} EnAdptTerminal;

/* Mirrors the v3 `kind` enum. The DC projection supports MCU, RESISTOR,
 * VOLTAGE_SOURCE, CURRENT_SOURCE, POTENTIOMETER, SWITCH and GROUND;
 * CAPACITOR and DEVICE have no DC primitive and fail explicitly. */
typedef enum EnAdptKind {
    EN_ADPT_KIND_MCU,
    EN_ADPT_KIND_DEVICE,
    EN_ADPT_KIND_RESISTOR,
    EN_ADPT_KIND_CAPACITOR,
    EN_ADPT_KIND_VOLTAGE_SOURCE,
    EN_ADPT_KIND_CURRENT_SOURCE,
    EN_ADPT_KIND_POTENTIOMETER,
    EN_ADPT_KIND_SWITCH,
    EN_ADPT_KIND_GROUND
} EnAdptKind;

typedef struct EnAdptComponent {
    EnAdptId id;
    EnAdptKind kind;
    const EnAdptTerminal *terminals; /* Borrowed; terminal_count entries. */
    size_t terminal_count;
    const EnAdptParameter *parameters; /* Borrowed; parameter_count entries. */
    size_t parameter_count;
} EnAdptComponent;

typedef struct EnAdptNet {
    EnAdptId id;
    const EnAdptId *endpoints; /* Borrowed; each must be a globally unique
                                * terminal ID of an existing component. */
    size_t endpoint_count;
} EnAdptNet;

typedef struct EnAdptGraph {
    unsigned abi_version; /* Must equal EN_ADPT_ABI_VERSION. */
    const EnAdptComponent *components; /* Borrowed; component_count entries. */
    size_t component_count;
    const EnAdptNet *nets; /* Borrowed; net_count entries. */
    size_t net_count;
} EnAdptGraph;

/* Adapter statuses. The first two are successful solve outcomes; the next
 * group are strict adapter rejections; the final group pass the kernel's
 * solver statuses through unchanged. */
typedef enum EnAdptStatus {
    EN_ADPT_OK = 0,
    EN_ADPT_FLOATING, /* Solved; some absolute voltages are unknown (NaN). */

    /* Strict adapter rejections (diagnostic names the offending ID). */
    EN_ADPT_INVALID_ABI,      /* NULL argument or abi_version mismatch. */
    EN_ADPT_INVALID_ID,       /* ID grammar violation or unterminated ID. */
    EN_ADPT_DUPLICATE_ID,     /* Repeated component/net/terminal ID. */
    EN_ADPT_DUPLICATE_ENDPOINT, /* Same terminal twice within one net. */
    EN_ADPT_UNKNOWN_ENDPOINT, /* Endpoint/return ID is not an existing terminal. */
    EN_ADPT_TERMINAL_CONFLICT, /* Terminal on two nets; wrong terminal
                                * count/roles for the component kind. */
    EN_ADPT_INVALID_PARAMETER, /* Missing, ill-typed, nonfinite or
                                * out-of-range parameter. */
    EN_ADPT_UNSUPPORTED,      /* Construct with no DC primitive or an element
                                * shorted onto a single net. */
    EN_ADPT_EXCEEDS_LIMITS,   /* Projection exceeds the adapter or kernel bounds. */

    /* Kernel solver outcomes, unmodified semantics. */
    EN_ADPT_SINGULAR,          /* Conflicting/redundant ideal sources etc. */
    EN_ADPT_NO_OPERATING_POINT, /* Net DC current into a floating island. */
    EN_ADPT_NUMERICAL,         /* Nonfinite/ill-conditioned numerics. */
    EN_ADPT_ALLOCATION_FAILED,
    EN_ADPT_SOLVER_INVALID     /* Kernel rejection of an already validated
                                * projection; report as an adapter bug. */
} EnAdptStatus;

typedef struct EnAdptNetResult {
    unsigned node;      /* Local solver node (0 = reference); UINT_MAX for empty nets. */
    unsigned char is_reference; /* Net merged into the declared reference. */
    unsigned char floating;     /* Island without absolute reference. */
    double voltage_v;           /* NaN when floating, unattached, or any failure. */
} EnAdptNetResult;

typedef struct EnAdptTerminalResult {
    double voltage_v;       /* Terminal potential; no net may still be source-connected. */
    unsigned char floating;
    unsigned char on_net;   /* 0 for terminals no valid net references. */
} EnAdptTerminalResult;

/* One projected kernel element with its branch current and the input
 * component (and, where meaningful, terminal) it came from. */
typedef struct EnAdptPrimitiveResult {
    size_t component_index; /* Index into the input component array. */
    size_t terminal_index;  /* Index within that component's terminal array of
                             * the terminal owning the branch: resistors/switches
                             * use the "a" terminal, sources the "p" terminal,
                             * potentiometer segments the segment start
                             * ("a" then "w"), MCU drivers the driven pad.
                             * SIZE_MAX when not applicable. */
    double current_a;       /* Signed p -> n branch current. */
} EnAdptPrimitiveResult;

typedef struct EnAdptResult {
    EnAdptStatus status;
    size_t bad_component; /* SIZE_MAX when no single component is at fault. */
    size_t bad_net;       /* SIZE_MAX when no single net is at fault. */
    char offender[EN_ADPT_ID_MAX + 1]; /* Offending ID/name, "" if none.
                                        * Non-printable bytes become '?'. */
    char diagnostic[EN_ADPT_DIAGNOSTIC_MAX]; /* Truncation allowed. */

    size_t net_count;      /* Valid entries in nets[] (== input net_count). */
    size_t terminal_count; /* Valid entries in terminals[], in input order:
                            * component i's terminal j is at the running
                            * offset of components before i plus j. */
    EnAdptNetResult nets[EN_ADPT_MAX_NETS];
    EnAdptTerminalResult terminals[EN_ADPT_MAX_TERMINALS];

    size_t node_count;     /* Projection size handed to the kernel. */
    size_t element_count;
    double max_kcl_residual_a;
    double max_source_residual_v;

    size_t primitive_count;
    EnAdptPrimitiveResult primitives[EN_DC_MAX_ELEMENTS];
} EnAdptResult;

/* Validate the v3 connectivity projection, project it onto net-dc primitives,
 * solve, and map node voltages / branch currents back to terminal and net
 * IDs. Deterministic: identical inputs in the same build produce identical
 * result bytes. On any status other than EN_ADPT_OK / EN_ADPT_FLOATING every
 * voltage and current output is NaN (no stale successful data leaks through)
 * and result->diagnostic names the problem. Only nets[0..net_count) and
 * terminals[0..terminal_count) are written. */
EnAdptStatus en_adpt_solve(const EnAdptGraph *graph, EnAdptResult *result);

/* Validated topology projection, without solving or allocating. ABI2 input
 * structs are unchanged. Native consumers use the node tables to add actual
 * firmware drivers without repeating ID resolution. RC mode alone accepts
 * capacitors; it requires an explicit initial_voltage reset policy quantity.
 * Unwired, undriven MCU terminals map to UINT_MAX without consuming node
 * capacity. Native consumers may allocate a private intrinsic pad node when
 * an actual on-die driver or pull becomes active; that never creates wiring. */
typedef struct EnAdptProjection {
    EnRcCircuit circuit;
    unsigned terminal_node[EN_ADPT_MAX_TERMINALS];
    unsigned net_node[EN_ADPT_MAX_NETS];
    size_t primitive_component[EN_RC_MAX_ELEMENTS];
    size_t primitive_terminal[EN_RC_MAX_ELEMENTS];
} EnAdptProjection;
EnAdptStatus en_adpt_project(const EnAdptGraph *graph, int allow_rc,
                            EnAdptProjection *projection, EnAdptResult *result);

/* Optional, explicit native device registry. Legacy ABI2 input structs and
 * en_adpt_solve/project remain strict: DEVICE is unsupported without a named
 * binding. Only exact registered models declare high-Z idle signals and zero
 * static rail load. Native services request finite rail-referenced branches;
 * every power/signal connection remains explicit. Complex service attributes
 * require native factory preflight before graph Apply. */
typedef enum EnAdptNativeModel {
    EN_ADPT_MODEL_SHT21,
    EN_ADPT_MODEL_24C02,
    EN_ADPT_MODEL_SPI_NOR_1M,
    EN_ADPT_MODEL_I2S_SAMPLE_PEER,
    EN_ADPT_MODEL_I2C_SCRIPTED_MASTER,
    EN_ADPT_MODEL_WS2812_FUNCTIONAL_3V3,
    EN_ADPT_MODEL_NEC_ENVELOPE_SOURCE,
    EN_ADPT_MODEL_ST7789_I80,
    EN_ADPT_MODEL_RGB_PANEL,
    EN_ADPT_MODEL_OV2640_DVP
} EnAdptNativeModel;
typedef struct EnAdptModelBinding {
    size_t component_index;
    EnAdptNativeModel model;
} EnAdptModelBinding;
/* Exact registered type and output-role descriptors; no DEVICE fallback.
 * open_drain is populated only when the role is a registered output. */
int en_adpt_model_from_type(const char *type, EnAdptNativeModel *model);
int en_adpt_model_terminal_driver(EnAdptNativeModel model, const char *role,
                                  int *open_drain);
EnAdptStatus en_adpt_project_models(const EnAdptGraph *graph, int allow_rc,
                                   const EnAdptModelBinding *models, size_t count,
                                   EnAdptProjection *projection,
                                   EnAdptResult *result);

#endif
