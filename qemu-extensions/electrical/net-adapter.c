/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "net-adapter.h"

#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Internal projection state. Every table is statically bounded; nothing is
 * derived from geometry because the ABI carries no geometry at all. */
#define MAX_GROUPS (EN_ADPT_MAX_NETS + EN_ADPT_MAX_TERMINALS)
typedef struct Adapter {
    const EnAdptGraph *graph;
    EnAdptProjection *projection;
    int allow_rc;
    const EnAdptModelBinding *models;
    size_t model_count;
    size_t terminal_total;
    size_t terminal_offset[EN_ADPT_MAX_COMPONENTS + 1];
    const EnAdptTerminal *flat_terminal[EN_ADPT_MAX_TERMINALS];
    /* During endpoint resolution, -1 means no declared net. Afterwards each
     * unwired terminal owns a private group; it never connects to another pin. */
    int net_of_terminal[EN_ADPT_MAX_TERMINALS];
    int driver_return_flat[EN_ADPT_MAX_TERMINALS]; /* -1: no driver. */
    size_t group_count;
    const char *group_id[MAX_GROUPS];
    unsigned net_parent[MAX_GROUPS];
    unsigned net_node[MAX_GROUPS];
    unsigned char net_is_ground[MAX_GROUPS];
    unsigned char net_is_reference[MAX_GROUPS];
    unsigned char group_used[MAX_GROUPS];
    unsigned char root_is_ground[MAX_GROUPS];
    unsigned node_of_root[MAX_GROUPS]; /* 0: unassigned; nodes start at 1. */
    EnDcCircuit circuit;
    size_t prim_component[EN_DC_MAX_ELEMENTS];
    size_t prim_terminal[EN_DC_MAX_ELEMENTS];
} Adapter;

static void store_offender(char *dst, const char *src)
{
    size_t out = 0;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    for (size_t i = 0; src[i] != '\0' && out < EN_ADPT_ID_MAX; ++i) {
        dst[out++] = isprint((unsigned char)src[i]) ? src[i] : '?';
    }
    dst[out] = '\0';
}

#if defined(__GNUC__) || defined(__clang__)
static EnAdptStatus fail(EnAdptResult *result, EnAdptStatus status,
                         size_t component, size_t net, const char *offender,
                         const char *format, ...)
    __attribute__((format(printf, 6, 7)));
#endif

static EnAdptStatus fail(EnAdptResult *result, EnAdptStatus status,
                         size_t component, size_t net, const char *offender,
                         const char *format, ...)
{
    va_list args;
    result->status = status;
    result->bad_component = component;
    result->bad_net = net;
    store_offender(result->offender, offender);
    va_start(args, format);
    vsnprintf(result->diagnostic, sizeof(result->diagnostic), format, args);
    va_end(args);
    /* No stale successful outputs: outputs were prefilled NaN at entry and
     * only real solutions overwrite them. */
    for (size_t i = 0; i < EN_ADPT_MAX_NETS; ++i) {
        result->nets[i].voltage_v = NAN;
    }
    for (size_t i = 0; i < EN_ADPT_MAX_TERMINALS; ++i) {
        result->terminals[i].voltage_v = NAN;
    }
    for (size_t i = 0; i < EN_DC_MAX_ELEMENTS; ++i) {
        result->primitives[i].current_a = NAN;
    }
    return status;
}

/* Exact v3 id grammar: 1..64 chars, first alphanumeric, then
 * [A-Za-z0-9_.:/-]. An unterminated fixed-size array fails because the scan
 * reaches the EN_ADPT_ID_MAX boundary without a NUL. */
static int id_valid(const char *id)
{
    if (!id || id[0] == '\0') {
        return 0;
    }
    for (size_t i = 0; i <= EN_ADPT_ID_MAX; ++i) {
        char c = id[i];
        if (c == '\0') {
            return i > 0;
        }
        {
            int alphanumeric = (c >= 'A' && c <= 'Z') ||
                               (c >= 'a' && c <= 'z') ||
                               (c >= '0' && c <= '9');
            int punctuation = c == '_' || c == '.' || c == ':' || c == '/' ||
                              c == '-';
            if (i == 0 ? !alphanumeric : !(alphanumeric || punctuation)) {
                return 0;
            }
        }
    }
    return 0;
}

static unsigned net_find(const unsigned *parent, unsigned net)
{
    while (parent[net] != net) {
        net = parent[net];
    }
    return net;
}

/* Stable lowest-index root, mirroring the kernel's gauge determinism. */
static void net_unite(unsigned *parent, unsigned a, unsigned b)
{
    a = net_find(parent, a);
    b = net_find(parent, b);
    if (a < b) {
        parent[b] = a;
    } else {
        parent[a] = b;
    }
}

static const char *role_or_empty(const EnAdptTerminal *terminal)
{
    return terminal->role ? terminal->role : "";
}

static const EnAdptQuantity *find_param(const EnAdptComponent *component,
                                        const char *name)
{
    for (size_t i = 0; i < component->parameter_count; ++i) {
        const EnAdptParameter *parameter = &component->parameters[i];
        if (parameter->name && strcmp(parameter->name, name) == 0) {
            return &parameter->quantity;
        }
    }
    return NULL;
}

static int resistance_ohms(const EnAdptQuantity *quantity, double *out)
{
    double scale = quantity->unit == EN_ADPT_UNIT_OHM ? 1.0 :
                   quantity->unit == EN_ADPT_UNIT_KOHM ? 1e3 :
                   quantity->unit == EN_ADPT_UNIT_MOHM ? 1e6 : 0.0;
    if (scale == 0.0 || !isfinite(quantity->value)) {
        return 0;
    }
    *out = quantity->value * scale;
    return 1;
}

static int voltage_volts(const EnAdptQuantity *quantity, double *out)
{
    double scale = quantity->unit == EN_ADPT_UNIT_V ? 1.0 :
                   quantity->unit == EN_ADPT_UNIT_MV ? 1e-3 : 0.0;
    if (scale == 0.0 || !isfinite(quantity->value)) {
        return 0;
    }
    *out = quantity->value * scale;
    return 1;
}

static int current_amps(const EnAdptQuantity *quantity, double *out)
{
    double scale = quantity->unit == EN_ADPT_UNIT_A ? 1.0 :
                   quantity->unit == EN_ADPT_UNIT_MA ? 1e-3 : 0.0;
    if (scale == 0.0 || !isfinite(quantity->value)) {
        return 0;
    }
    *out = quantity->value * scale;
    return 1;
}

static int position_ratio(const EnAdptQuantity *quantity, double *out)
{
    if (quantity->unit == EN_ADPT_UNIT_RATIO) {
        if (!isfinite(quantity->value) ||
            quantity->value < 0.0 || quantity->value > 1.0) {
            return 0;
        }
        *out = quantity->value;
        return 1;
    }
    if (quantity->unit == EN_ADPT_UNIT_PERCENT) {
        if (!isfinite(quantity->value) ||
            quantity->value < 0.0 || quantity->value > 100.0) {
            return 0;
        }
        *out = quantity->value / 100.0;
        return 1;
    }
    return 0;
}

/* A finite resistance the kernel can invert without overflow. */
static int usable_resistance(double ohms)
{
    return isfinite(ohms) && ohms > 0.0 && isfinite(1.0 / ohms);
}

static size_t role_index(const EnAdptComponent *component, const char *role)
{
    for (size_t i = 0; i < component->terminal_count; ++i) {
        if (strcmp(role_or_empty(&component->terminals[i]), role) == 0) {
            return i;
        }
    }
    return SIZE_MAX;
}

static int roles_match(const EnAdptComponent *component,
                       const char *const *roles, size_t count)
{
    if (component->terminal_count != count) {
        return 0;
    }
    for (size_t r = 0; r < count; ++r) {
        size_t matches = 0;
        for (size_t i = 0; i < component->terminal_count; ++i) {
            if (strcmp(role_or_empty(&component->terminals[i]), roles[r]) == 0) {
                ++matches;
            }
        }
        if (matches != 1) {
            return 0;
        }
    }
    return 1;
}

static const char *kind_name(EnAdptKind kind)
{
    switch (kind) {
    case EN_ADPT_KIND_MCU: return "mcu";
    case EN_ADPT_KIND_DEVICE: return "device";
    case EN_ADPT_KIND_RESISTOR: return "resistor";
    case EN_ADPT_KIND_CAPACITOR: return "capacitor";
    case EN_ADPT_KIND_VOLTAGE_SOURCE: return "voltage-source";
    case EN_ADPT_KIND_CURRENT_SOURCE: return "current-source";
    case EN_ADPT_KIND_POTENTIOMETER: return "potentiometer";
    case EN_ADPT_KIND_SWITCH: return "switch";
    case EN_ADPT_KIND_GROUND: return "ground";
    default: return "unknown";
    }
}

/* Returns the offending parameter name (possibly a nameless parameter
 * reported as ""), or NULL when every parameter is on the allow-list. */
static const char *unknown_parameter(const EnAdptComponent *component,
                                     const char *const *allowed,
                                     size_t allowed_count)
{
    for (size_t i = 0; i < component->parameter_count; ++i) {
        const EnAdptParameter *parameter = &component->parameters[i];
        int known = 0;
        if (!parameter->name) {
            return "";
        }
        for (size_t a = 0; a < allowed_count; ++a) {
            if (strcmp(parameter->name, allowed[a]) == 0) {
                known = 1;
                break;
            }
        }
        if (!known) {
            return parameter->name;
        }
    }
    return NULL;
}

static const char *dc_kind_name(EnDcKind kind)
{
    switch (kind) {
    case EN_DC_RESISTOR: return "resistor";
    case EN_DC_CURRENT_SOURCE: return "current source";
    case EN_DC_VOLTAGE_SOURCE: return "voltage source";
    case EN_DC_DRIVER: return "driver";
    default: return "unknown";
    }
}

static EnAdptStatus add_primitive(Adapter *adapter, EnAdptResult *result,
                                  size_t component_index, size_t terminal_index,
                                  EnDcKind kind, unsigned p, unsigned n,
                                  double value, double resistance)
{
    EnDcElement *element;
    const EnAdptComponent *component = &adapter->graph->components[component_index];
    if (adapter->circuit.element_count >= EN_DC_MAX_ELEMENTS) {
        return fail(result, EN_ADPT_EXCEEDS_LIMITS, component_index, SIZE_MAX,
                    component->id.text,
                    "Component '%s' pushes the projection past the %d-primitive "
                    "limit", component->id.text, EN_DC_MAX_ELEMENTS);
    }
    if (p == n) {
        size_t flat = adapter->terminal_offset[component_index] + terminal_index;
        const char *net_id = flat < adapter->terminal_total &&
                                     adapter->net_of_terminal[flat] >= 0
                                 ? adapter->group_id[adapter->net_of_terminal[flat]]
                                 : "(none)";
        return fail(result, EN_ADPT_UNSUPPORTED, component_index, SIZE_MAX,
                    component->id.text,
                    "Component '%s' has a %s primitive with both terminals on "
                    "net '%s'; a DC element shorted onto one net is not representable",
                    component->id.text, dc_kind_name(kind), net_id);
    }
    element = &adapter->circuit.elements[adapter->circuit.element_count];
    element->kind = kind;
    element->p = p;
    element->n = n;
    element->value = value;
    element->resistance = resistance;
    adapter->prim_component[adapter->circuit.element_count] = component_index;
    adapter->prim_terminal[adapter->circuit.element_count] = terminal_index;
    ++adapter->circuit.element_count;
    if (adapter->projection) {
        EnRcElement *rc = &adapter->projection->circuit.elements[
            adapter->circuit.element_count - 1];
        static const EnRcKind kinds[] = {
            EN_RC_RESISTOR, EN_RC_CURRENT_SOURCE, EN_RC_VOLTAGE_SOURCE, EN_RC_DRIVER
        };
        rc->kind = kinds[kind];
        rc->p = p;
        rc->n = n;
        rc->value = value;
        rc->resistance = resistance;
    }
    return EN_ADPT_OK;
}

static int net_of(Adapter *adapter, size_t component_index, size_t terminal_index)
{
    return adapter->net_of_terminal[adapter->terminal_offset[component_index] +
                                    terminal_index];
}


/* Resolved local solver node of a component terminal (post-merge). */
static unsigned node_of(Adapter *adapter, size_t component_index,
                        size_t terminal_index)
{
    return adapter->net_node[net_of(adapter, component_index, terminal_index)];
}

typedef struct ModelRole {
    const char *name;
    EnAdptTerminalDirection direction;
    int open_drain;
} ModelRole;

typedef struct ModelSpec {
    const char *type;
    const ModelRole *roles;
    size_t role_count;
    unsigned data_width;
    EnAdptTerminalDirection data_direction;
} ModelSpec;

static const ModelRole i2c_roles[] = {
    {"sda", EN_ADPT_DIRECTION_INOUT, 1}, {"scl", EN_ADPT_DIRECTION_INOUT, 1},
    {"vdd", EN_ADPT_DIRECTION_INPUT, 0}, {"gnd", EN_ADPT_DIRECTION_INPUT, 0}
};
static const ModelRole spi_roles[] = {
    {"mosi", EN_ADPT_DIRECTION_INPUT, 0}, {"miso", EN_ADPT_DIRECTION_OUTPUT, 0},
    {"sclk", EN_ADPT_DIRECTION_INPUT, 0}, {"cs", EN_ADPT_DIRECTION_INPUT, 0},
    {"vdd", EN_ADPT_DIRECTION_INPUT, 0}, {"gnd", EN_ADPT_DIRECTION_INPUT, 0}
};
static const ModelRole i2s_roles[] = {
    {"bclk", EN_ADPT_DIRECTION_INOUT, 0}, {"ws", EN_ADPT_DIRECTION_INOUT, 0},
    {"din", EN_ADPT_DIRECTION_INPUT, 0}, {"dout", EN_ADPT_DIRECTION_OUTPUT, 0},
    {"vdd", EN_ADPT_DIRECTION_INPUT, 0}, {"gnd", EN_ADPT_DIRECTION_INPUT, 0}
};
static const ModelRole ws2812_roles[] = {
    {"din", EN_ADPT_DIRECTION_INPUT, 0},
    {"vdd", EN_ADPT_DIRECTION_INPUT, 0}, {"gnd", EN_ADPT_DIRECTION_INPUT, 0}
};
static const ModelRole nec_roles[] = {
    {"out", EN_ADPT_DIRECTION_OUTPUT, 1},
    {"vdd", EN_ADPT_DIRECTION_INPUT, 0}, {"gnd", EN_ADPT_DIRECTION_INPUT, 0}
};
static const ModelRole i80_roles[] = {
    {"wr", EN_ADPT_DIRECTION_INPUT, 0}, {"dc", EN_ADPT_DIRECTION_INPUT, 0},
    {"cs", EN_ADPT_DIRECTION_INPUT, 0}, {"reset", EN_ADPT_DIRECTION_INPUT, 0},
    {"vdd", EN_ADPT_DIRECTION_INPUT, 0}, {"gnd", EN_ADPT_DIRECTION_INPUT, 0}
};
static const ModelRole rgb_roles[] = {
    {"pclk", EN_ADPT_DIRECTION_INPUT, 0}, {"hsync", EN_ADPT_DIRECTION_INPUT, 0},
    {"vsync", EN_ADPT_DIRECTION_INPUT, 0}, {"de", EN_ADPT_DIRECTION_INPUT, 0},
    {"reset", EN_ADPT_DIRECTION_INPUT, 0},
    {"vdd", EN_ADPT_DIRECTION_INPUT, 0}, {"gnd", EN_ADPT_DIRECTION_INPUT, 0}
};
static const ModelRole camera_roles[] = {
    {"sda", EN_ADPT_DIRECTION_INOUT, 1}, {"scl", EN_ADPT_DIRECTION_INOUT, 1},
    {"xclk", EN_ADPT_DIRECTION_INPUT, 0}, {"reset", EN_ADPT_DIRECTION_INPUT, 0},
    {"pwdn", EN_ADPT_DIRECTION_INPUT, 0}, {"pclk", EN_ADPT_DIRECTION_OUTPUT, 0},
    {"href", EN_ADPT_DIRECTION_OUTPUT, 0}, {"vsync", EN_ADPT_DIRECTION_OUTPUT, 0},
    {"vdd", EN_ADPT_DIRECTION_INPUT, 0}, {"gnd", EN_ADPT_DIRECTION_INPUT, 0}
};

#define MODEL_SPEC(type, roles, width, direction) \
    {type, roles, sizeof(roles) / sizeof((roles)[0]), width, direction}
static const ModelSpec model_specs[] = {
    [EN_ADPT_MODEL_SHT21] = MODEL_SPEC("sht21", i2c_roles, 0, EN_ADPT_DIRECTION_INPUT),
    [EN_ADPT_MODEL_24C02] = MODEL_SPEC("24c02", i2c_roles, 0, EN_ADPT_DIRECTION_INPUT),
    [EN_ADPT_MODEL_SPI_NOR_1M] = MODEL_SPEC("spi-nor-1m", spi_roles, 0, EN_ADPT_DIRECTION_INPUT),
    [EN_ADPT_MODEL_I2S_SAMPLE_PEER] = MODEL_SPEC("i2s-sample-peer", i2s_roles, 0, EN_ADPT_DIRECTION_INPUT),
    [EN_ADPT_MODEL_I2C_SCRIPTED_MASTER] = MODEL_SPEC("i2c-scripted-master", i2c_roles, 0, EN_ADPT_DIRECTION_INPUT),
    [EN_ADPT_MODEL_WS2812_FUNCTIONAL_3V3] = MODEL_SPEC("ws2812-functional-3v3", ws2812_roles, 0, EN_ADPT_DIRECTION_INPUT),
    [EN_ADPT_MODEL_NEC_ENVELOPE_SOURCE] = MODEL_SPEC("nec-envelope-source", nec_roles, 0, EN_ADPT_DIRECTION_INPUT),
    [EN_ADPT_MODEL_ST7789_I80] = MODEL_SPEC("st7789-i80", i80_roles, 16, EN_ADPT_DIRECTION_INPUT),
    [EN_ADPT_MODEL_RGB_PANEL] = MODEL_SPEC("rgb-panel", rgb_roles, 16, EN_ADPT_DIRECTION_INPUT),
    [EN_ADPT_MODEL_OV2640_DVP] = MODEL_SPEC("ov2640-dvp", camera_roles, 8, EN_ADPT_DIRECTION_OUTPUT)
};
#undef MODEL_SPEC

static const ModelSpec *model_spec(EnAdptNativeModel model)
{
    return (unsigned)model < sizeof(model_specs) / sizeof(model_specs[0]) ?
           &model_specs[model] : NULL;
}

int en_adpt_model_from_type(const char *type, EnAdptNativeModel *model)
{
    if (!type || !model) {
        return 0;
    }
    for (size_t i = 0; i < sizeof(model_specs) / sizeof(model_specs[0]); ++i) {
        if (!strcmp(type, model_specs[i].type)) {
            *model = (EnAdptNativeModel)i;
            return 1;
        }
    }
    return 0;
}

/* Exact decimal spelling only: d0..d15, never d00 or d16. */
static int model_data_role(const ModelSpec *spec, const char *role)
{
    if (!role || role[0] != 'd' || !spec->data_width) {
        return 0;
    }
    unsigned index;
    if (role[1] >= '0' && role[1] <= '9' && !role[2]) {
        index = role[1] - '0';
    } else if (role[1] == '1' && role[2] >= '0' && role[2] <= '5' && !role[3]) {
        index = 10 + role[2] - '0';
    } else {
        return 0;
    }
    return index < spec->data_width;
}

int en_adpt_model_terminal_driver(EnAdptNativeModel model, const char *role,
                                  int *open_drain)
{
    const ModelSpec *spec = model_spec(model);
    if (!spec || !role || !open_drain) {
        return 0;
    }
    if (model_data_role(spec, role)) {
        if (spec->data_direction != EN_ADPT_DIRECTION_OUTPUT) {
            return 0;
        }
        *open_drain = 0;
        return 1;
    }
    for (size_t i = 0; i < spec->role_count; ++i) {
        const ModelRole *r = &spec->roles[i];
        if (!strcmp(role, r->name) &&
            (r->direction == EN_ADPT_DIRECTION_OUTPUT ||
             r->direction == EN_ADPT_DIRECTION_INOUT)) {
            *open_drain = r->open_drain;
            return 1;
        }
    }
    return 0;
}

static int model_count_parameter(const EnAdptComponent *c, const char *name,
                                 double minimum, double maximum)
{
    const EnAdptQuantity *q = find_param(c, name);
    return q && q->unit == EN_ADPT_UNIT_COUNT && isfinite(q->value) &&
           q->value == floor(q->value) && q->value >= minimum &&
           q->value <= maximum;
}

static EnAdptStatus validate_model(Adapter *adapter, EnAdptResult *result,
                                  size_t ci)
{
    const EnAdptComponent *c = &adapter->graph->components[ci];
    const EnAdptModelBinding *binding = NULL;
    for (size_t i = 0; i < adapter->model_count; ++i) {
        if (adapter->models[i].component_index == ci) {
            binding = &adapter->models[i];
            break;
        }
    }
    const ModelSpec *spec = binding ? model_spec(binding->model) : NULL;
    if (!spec) {
        return fail(result, EN_ADPT_UNSUPPORTED, ci, SIZE_MAX, c->id.text,
                    "Device '%s' has no registered native electrical model", c->id.text);
    }
    if (c->terminal_count != spec->role_count + spec->data_width) {
        return fail(result, EN_ADPT_TERMINAL_CONFLICT, ci, SIZE_MAX, c->id.text,
                    "Registered %s model '%s' has an incorrect terminal count", spec->type, c->id.text);
    }
    uint32_t data_roles = 0, named_roles = 0;
    for (size_t i = 0; i < c->terminal_count; ++i) {
        const EnAdptTerminal *t = &c->terminals[i];
        EnAdptTerminalDomain domain = EN_ADPT_DOMAIN_DIGITAL;
        EnAdptTerminalDirection direction;
        int clock = 0;
        if (model_data_role(spec, t->role)) {
            unsigned index = t->role[2] ? 10 + t->role[2] - '0' : t->role[1] - '0';
            if (data_roles & (UINT32_C(1) << index)) {
                return fail(result, EN_ADPT_TERMINAL_CONFLICT, ci, SIZE_MAX, t->id.text,
                            "Registered model '%s' duplicates role '%s'", c->id.text, t->role);
            }
            data_roles |= UINT32_C(1) << index;
            direction = spec->data_direction;
        } else {
            size_t ri;
            for (ri = 0; ri < spec->role_count; ++ri) {
                if (!strcmp(t->role, spec->roles[ri].name)) {
                    break;
                }
            }
            if (ri == spec->role_count || (named_roles & (UINT32_C(1) << ri))) {
                return fail(result, EN_ADPT_TERMINAL_CONFLICT, ci, SIZE_MAX, t->id.text,
                            "Registered model '%s' has unknown/duplicate role '%s'", c->id.text, t->role);
            }
            named_roles |= UINT32_C(1) << ri;
            direction = spec->roles[ri].direction;
            domain = !strcmp(t->role, "vdd") ? EN_ADPT_DOMAIN_POWER :
                     !strcmp(t->role, "gnd") ? EN_ADPT_DOMAIN_GROUND : EN_ADPT_DOMAIN_DIGITAL;
            clock = binding->model == EN_ADPT_MODEL_I2S_SAMPLE_PEER && ri < 2;
        }
        int direction_valid = clock ?
            t->direction == EN_ADPT_DIRECTION_INPUT ||
            t->direction == EN_ADPT_DIRECTION_OUTPUT ||
            t->direction == EN_ADPT_DIRECTION_INOUT : t->direction == direction;
        if (t->domain != domain || !direction_valid) {
            return fail(result, EN_ADPT_TERMINAL_CONFLICT, ci, SIZE_MAX, t->id.text,
                        "Registered model terminal '%s' has invalid domain/direction", t->id.text);
        }
    }
    int valid;
    if (binding->model == EN_ADPT_MODEL_SHT21 || binding->model == EN_ADPT_MODEL_24C02) {
        valid = c->parameter_count == 1 && model_count_parameter(c, "address", 1, 127);
    } else if (binding->model == EN_ADPT_MODEL_WS2812_FUNCTIONAL_3V3) {
        valid = c->parameter_count == 1 && model_count_parameter(c, "led_count", 1, 256);
    } else if (binding->model == EN_ADPT_MODEL_NEC_ENVELOPE_SOURCE) {
        valid = c->parameter_count == 3 &&
                model_count_parameter(c, "address", 0, 255) &&
                model_count_parameter(c, "command", 0, 255) &&
                model_count_parameter(c, "period_ms", 100, 60000);
    } else {
        /* Complex attributes belong to mandatory native service-factory
         * preflight, never to an untyped electrical parameter fallback. */
        valid = c->parameter_count == 0;
    }
    if (!valid) {
        return fail(result, EN_ADPT_INVALID_PARAMETER, ci, SIZE_MAX, c->id.text,
                    "Registered %s model '%s' has invalid exact count parameters", spec->type, c->id.text);
    }
    return EN_ADPT_OK;
}

/* Phase 5: structural, parameter and driver validation. Also marks ground
 * nets and applies exact-endpoint potentiometer wiper merges. */
static EnAdptStatus validate_component(Adapter *adapter, EnAdptResult *result,
                                       size_t component_index)
{
    static const char *const passive[] = {"a", "b"};
    static const char *const polar[] = {"p", "n"};
    static const char *const pot[] = {"a", "w", "b"};
    static const char *const ref_only[] = {"ref"};
    const EnAdptComponent *component =
        &adapter->graph->components[component_index];
    const char *unknown = NULL;

    switch (component->kind) {
    case EN_ADPT_KIND_CAPACITOR:
        if (!adapter->allow_rc) {
            return fail(result, EN_ADPT_UNSUPPORTED, component_index, SIZE_MAX,
                        component->id.text, "Capacitor '%s' requires RC projection",
                        component->id.text);
        }
        if (!roles_match(component, polar, 2)) {
            return fail(result, EN_ADPT_TERMINAL_CONFLICT, component_index,
                        SIZE_MAX, component->id.text,
                        "Capacitor '%s' requires p,n terminals", component->id.text);
        }
        break;
    case EN_ADPT_KIND_DEVICE: {
        EnAdptStatus status = validate_model(adapter, result, component_index);
        if (status != EN_ADPT_OK) {
            return status;
        }
        break;
    }
    case EN_ADPT_KIND_RESISTOR:
    case EN_ADPT_KIND_SWITCH:
        if (!roles_match(component, passive, 2)) {
            return fail(result, EN_ADPT_TERMINAL_CONFLICT, component_index,
                        SIZE_MAX, component->id.text,
                        "Component '%s' (%s) needs exactly one 'a' and one 'b' "
                        "terminal", component->id.text,
                        kind_name(component->kind));
        }
        break;
    case EN_ADPT_KIND_VOLTAGE_SOURCE:
    case EN_ADPT_KIND_CURRENT_SOURCE:
        if (!roles_match(component, polar, 2)) {
            return fail(result, EN_ADPT_TERMINAL_CONFLICT, component_index,
                        SIZE_MAX, component->id.text,
                        "Component '%s' (%s) needs exactly one 'p' and one 'n' "
                        "terminal", component->id.text,
                        kind_name(component->kind));
        }
        break;
    case EN_ADPT_KIND_POTENTIOMETER:
        if (!roles_match(component, pot, 3)) {
            return fail(result, EN_ADPT_TERMINAL_CONFLICT, component_index,
                        SIZE_MAX, component->id.text,
                        "Component '%s' (potentiometer) needs exactly one 'a', "
                        "'w' and 'b' terminal", component->id.text);
        }
        break;
    case EN_ADPT_KIND_GROUND:
        if (!roles_match(component, ref_only, 1)) {
            return fail(result, EN_ADPT_TERMINAL_CONFLICT, component_index,
                        SIZE_MAX, component->id.text,
                        "Component '%s' (ground) needs exactly one 'ref' terminal",
                        component->id.text);
        }
        break;
    case EN_ADPT_KIND_MCU:
        break;
    default:
        return fail(result, EN_ADPT_UNSUPPORTED, component_index, SIZE_MAX,
                    component->id.text, "Component '%s' has an unknown kind",
                    component->id.text);
    }

    switch (component->kind) {
    case EN_ADPT_KIND_CAPACITOR: {
        static const char *const allowed[] = {"capacitance", "initial_voltage"};
        const EnAdptQuantity *q = find_param(component, "capacitance");
        const EnAdptQuantity *initial = find_param(component, "initial_voltage");
        double scale = !q ? 0 : q->unit == EN_ADPT_UNIT_F ? 1 :
                       q->unit == EN_ADPT_UNIT_UF ? 1e-6 :
                       q->unit == EN_ADPT_UNIT_NF ? 1e-9 :
                       q->unit == EN_ADPT_UNIT_PF ? 1e-12 : 0;
        double volts;
        unknown = unknown_parameter(component, allowed, 2);
        if (unknown || !q || scale == 0 || !isfinite(q->value * scale) ||
            q->value * scale <= 0 || !initial ||
            !voltage_volts(initial, &volts)) {
            return fail(result, EN_ADPT_INVALID_PARAMETER, component_index,
                        SIZE_MAX, component->id.text,
                        "Capacitor '%s' requires positive F/uF/nF/pF capacitance "
                        "and explicit finite initial_voltage V/mV", component->id.text);
        }
        break;
    }
    case EN_ADPT_KIND_RESISTOR: {
        static const char *const allowed[] = {"resistance"};
        const EnAdptQuantity *resistance;
        double ohms;
        unknown = unknown_parameter(component, allowed, 1);
        if (unknown) {
            return fail(result, EN_ADPT_INVALID_PARAMETER, component_index,
                        SIZE_MAX, unknown,
                        "Resistor '%s' has parameter '%s' with no defined DC "
                        "primitive; unknown quantities are never ignored",
                        component->id.text, unknown);
        }
        resistance = find_param(component, "resistance");
        if (!resistance || !resistance_ohms(resistance, &ohms) ||
            !usable_resistance(ohms)) {
            return fail(result, EN_ADPT_INVALID_PARAMETER, component_index,
                        SIZE_MAX, "resistance",
                        "Resistor '%s' needs a finite positive resistance in "
                        "ohm/kohm/Mohm", component->id.text);
        }
        break;
    }
    case EN_ADPT_KIND_POTENTIOMETER: {
        static const char *const allowed[] = {"resistance", "position"};
        const EnAdptQuantity *resistance, *position;
        double ohms, ratio;
        unknown = unknown_parameter(component, allowed, 2);
        if (unknown) {
            return fail(result, EN_ADPT_INVALID_PARAMETER, component_index,
                        SIZE_MAX, unknown,
                        "Potentiometer '%s' has parameter '%s' with no defined "
                        "DC primitive", component->id.text, unknown);
        }
        resistance = find_param(component, "resistance");
        position = find_param(component, "position");
        if (!resistance || !resistance_ohms(resistance, &ohms) ||
            !usable_resistance(ohms)) {
            return fail(result, EN_ADPT_INVALID_PARAMETER, component_index,
                        SIZE_MAX, "resistance",
                        "Potentiometer '%s' needs a finite positive resistance "
                        "in ohm/kohm/Mohm", component->id.text);
        }
        if (!position || !position_ratio(position, &ratio)) {
            return fail(result, EN_ADPT_INVALID_PARAMETER, component_index,
                        SIZE_MAX, "position",
                        "Potentiometer '%s' needs a position of ratio 0..1 or "
                        "percent 0..100", component->id.text);
        }
        if (ratio > 0.0 && ratio < 1.0 &&
            (!usable_resistance(ohms * ratio) ||
             !usable_resistance(ohms * (1.0 - ratio)))) {
            return fail(result, EN_ADPT_INVALID_PARAMETER, component_index,
                        SIZE_MAX, "position",
                        "Potentiometer '%s' position makes a segment resistance "
                        "unrepresentable", component->id.text);
        }
        /* Exact endpoints merge the wiper net (explicit zero-ohm node merge,
         * per the electrical-dc contract's zero-ohm rule). */
        if (ratio == 0.0) {
            net_unite(adapter->net_parent,
                      (unsigned)net_of(adapter, component_index,
                                       role_index(component, "a")),
                      (unsigned)net_of(adapter, component_index,
                                       role_index(component, "w")));
        } else if (ratio == 1.0) {
            net_unite(adapter->net_parent,
                      (unsigned)net_of(adapter, component_index,
                                       role_index(component, "w")),
                      (unsigned)net_of(adapter, component_index,
                                       role_index(component, "b")));
        }
        break;
    }
    case EN_ADPT_KIND_SWITCH: {
        static const char *const allowed[] = {"state", "on_resistance",
                                              "off_resistance"};
        const EnAdptQuantity *state, *on_resistance, *off_resistance;
        double ohms;
        unknown = unknown_parameter(component, allowed, 3);
        if (unknown) {
            return fail(result, EN_ADPT_INVALID_PARAMETER, component_index,
                        SIZE_MAX, unknown,
                        "Switch '%s' has parameter '%s' with no defined DC "
                        "primitive", component->id.text, unknown);
        }
        state = find_param(component, "state");
        if (!state || (state->unit != EN_ADPT_UNIT_STATE_OPEN &&
                       state->unit != EN_ADPT_UNIT_STATE_CLOSED)) {
            return fail(result, EN_ADPT_INVALID_PARAMETER, component_index,
                        SIZE_MAX, "state",
                        "Switch '%s' needs a state parameter of unit 'state' "
                        "(open/closed)", component->id.text);
        }
        on_resistance = find_param(component, "on_resistance");
        off_resistance = find_param(component, "off_resistance");
        if (on_resistance &&
            (!resistance_ohms(on_resistance, &ohms) ||
             !usable_resistance(ohms))) {
            return fail(result, EN_ADPT_INVALID_PARAMETER, component_index,
                        SIZE_MAX, component->id.text,
                        "Switch '%s' on_resistance must be finite and positive",
                        component->id.text);
        }
        if (off_resistance &&
            (!resistance_ohms(off_resistance, &ohms) ||
             !usable_resistance(ohms))) {
            return fail(result, EN_ADPT_INVALID_PARAMETER, component_index,
                        SIZE_MAX, component->id.text,
                        "Switch '%s' off_resistance must be finite and positive",
                        component->id.text);
        }
        if (state->unit == EN_ADPT_UNIT_STATE_CLOSED && !on_resistance) {
            return fail(result, EN_ADPT_UNSUPPORTED, component_index, SIZE_MAX,
                        component->id.text,
                        "Closed switch '%s' has no explicit on_resistance; no "
                        "zero-ohm or default resistance is invented",
                        component->id.text);
        }
        break;
    }
    case EN_ADPT_KIND_VOLTAGE_SOURCE:
    case EN_ADPT_KIND_CURRENT_SOURCE: {
        static const char *const allowed_voltage[] = {"voltage",
                                                      "series_resistance"};
        static const char *const allowed_current[] = {"current",
                                                      "series_resistance"};
        const char *const *allowed =
            component->kind == EN_ADPT_KIND_VOLTAGE_SOURCE ? allowed_voltage
                                                           : allowed_current;
        const EnAdptQuantity *main_quantity, *series;
        double scaled, ohms;
        int ok;
        unknown = unknown_parameter(component, allowed, 2);
        if (unknown) {
            return fail(result, EN_ADPT_INVALID_PARAMETER, component_index,
                        SIZE_MAX, unknown,
                        "%s '%s' has parameter '%s' with no defined DC primitive",
                        kind_name(component->kind), component->id.text, unknown);
        }
        if (component->kind == EN_ADPT_KIND_VOLTAGE_SOURCE) {
            main_quantity = find_param(component, "voltage");
            ok = main_quantity && voltage_volts(main_quantity, &scaled);
        } else {
            main_quantity = find_param(component, "current");
            ok = main_quantity && current_amps(main_quantity, &scaled);
        }
        if (!ok) {
            return fail(result, EN_ADPT_INVALID_PARAMETER, component_index,
                        SIZE_MAX,
                        component->kind == EN_ADPT_KIND_VOLTAGE_SOURCE
                            ? "voltage"
                            : "current",
                        "%s '%s' needs a finite %s parameter in %s",
                        kind_name(component->kind), component->id.text,
                        component->kind == EN_ADPT_KIND_VOLTAGE_SOURCE
                            ? "voltage"
                            : "current",
                        component->kind == EN_ADPT_KIND_VOLTAGE_SOURCE ? "V/mV"
                                                                       : "A/mA");
        }
        series = find_param(component, "series_resistance");
        if (series && (!resistance_ohms(series, &ohms) ||
                       !usable_resistance(ohms))) {
            return fail(result, EN_ADPT_INVALID_PARAMETER, component_index,
                        SIZE_MAX, "series_resistance",
                        "Source '%s' series_resistance must be finite and "
                        "positive (omit it for an ideal source)",
                        component->id.text);
        }
        break;
    }
    case EN_ADPT_KIND_GROUND: {
        size_t ref = role_index(component, "ref");
        size_t flat = adapter->terminal_offset[component_index] + ref;
        if (component->parameter_count > 0) {
            return fail(result, EN_ADPT_INVALID_PARAMETER, component_index,
                        SIZE_MAX,
                        component->parameters[0].name
                            ? component->parameters[0].name
                            : component->id.text,
                        "Ground '%s' carries parameters; ground has no DC "
                        "parameters, only its reference terminal",
                        component->id.text);
        }
        adapter->net_is_ground[adapter->net_of_terminal[flat]] = 1;
        break;
    }
    case EN_ADPT_KIND_MCU:
        if (component->parameter_count > 0) {
            return fail(result, EN_ADPT_INVALID_PARAMETER, component_index,
                        SIZE_MAX,
                        component->parameters[0].name
                            ? component->parameters[0].name
                            : component->id.text,
                        "MCU '%s' has component-level parameter '%s' with no "
                        "defined DC primitive; drive state belongs on terminals",
                        component->id.text,
                        component->parameters[0].name
                            ? component->parameters[0].name
                            : "(nameless)");
        }
        break;
    default:
        break;
    }

    /* Driver state: only MCU pads may drive, and only explicitly. */
    for (size_t t = 0; t < component->terminal_count; ++t) {
        const EnAdptTerminal *terminal = &component->terminals[t];
        size_t flat = adapter->terminal_offset[component_index] + t;
        int found = -1;
        if (terminal->driver.drive == EN_ADPT_DRIVE_NONE) {
            continue;
        }
        if (component->kind != EN_ADPT_KIND_MCU) {
            return fail(result, EN_ADPT_UNSUPPORTED, component_index, SIZE_MAX,
                        component->id.text,
                        "Terminal '%s' on %s '%s' carries a driver state; only "
                        "MCU pads may drive", terminal->id.text,
                        kind_name(component->kind), component->id.text);
        }
        if (terminal->direction == EN_ADPT_DIRECTION_INPUT) {
            return fail(result, EN_ADPT_INVALID_PARAMETER, component_index,
                        SIZE_MAX, terminal->id.text,
                        "Input terminal '%s' cannot carry a driver state",
                        terminal->id.text);
        }
        if (!isfinite(terminal->driver.voltage_v) ||
            !usable_resistance(terminal->driver.impedance_ohm)) {
            return fail(result, EN_ADPT_INVALID_PARAMETER, component_index,
                        SIZE_MAX, terminal->id.text,
                        "Driver on terminal '%s' needs a finite Thevenin "
                        "voltage and a finite positive impedance",
                        terminal->id.text);
        }
        if (!terminal->driver.return_id) {
            return fail(result, EN_ADPT_INVALID_PARAMETER, component_index,
                        SIZE_MAX, terminal->id.text,
                        "Driver on terminal '%s' has no explicit return "
                        "terminal; no ground or supply is inferred",
                        terminal->id.text);
        }
        if (!id_valid(terminal->driver.return_id)) {
            return fail(result, EN_ADPT_INVALID_ID, component_index, SIZE_MAX,
                        terminal->driver.return_id,
                        "Driver return ID '%s' violates the v3 id grammar",
                        terminal->driver.return_id);
        }
        for (size_t s = 0; s < adapter->terminal_total; ++s) {
            if (strcmp(adapter->flat_terminal[s]->id.text,
                       terminal->driver.return_id) == 0) {
                found = (int)s;
                break;
            }
        }
        if (found < 0) {
            return fail(result, EN_ADPT_UNKNOWN_ENDPOINT, component_index,
                        SIZE_MAX, terminal->driver.return_id,
                        "Driver on terminal '%s' names return terminal '%s' "
                        "which no component has", terminal->id.text,
                        terminal->driver.return_id);
        }
        adapter->driver_return_flat[flat] = found;
    }
    return EN_ADPT_OK;
}

/* Phase 7: project onto kernel primitives using resolved node indices. */
static EnAdptStatus build_component(Adapter *adapter, EnAdptResult *result,
                                    size_t component_index)
{
    const EnAdptComponent *component =
        &adapter->graph->components[component_index];
    EnAdptStatus status = EN_ADPT_OK;

    switch (component->kind) {
    case EN_ADPT_KIND_CAPACITOR: {
        const EnAdptQuantity *q = find_param(component, "capacitance");
        double scale = q->unit == EN_ADPT_UNIT_F ? 1 :
                       q->unit == EN_ADPT_UNIT_UF ? 1e-6 :
                       q->unit == EN_ADPT_UNIT_NF ? 1e-9 : 1e-12;
        size_t p = role_index(component, "p"), n = role_index(component, "n");
        /* Common bound/endpoints/ownership checks; this slot is used only
         * by the RC projection and is never handed to the DC solver. */
        status = add_primitive(adapter, result, component_index, p,
                               EN_DC_RESISTOR, node_of(adapter, component_index, p),
                               node_of(adapter, component_index, n), 1, 0);
        if (status == EN_ADPT_OK) {
            EnRcElement *rc = &adapter->projection->circuit.elements[
                adapter->circuit.element_count - 1];
            rc->kind = EN_RC_CAPACITOR;
            rc->value = q->value * scale;
            (void)voltage_volts(find_param(component, "initial_voltage"),
                                &rc->initial_voltage);
        }
        break;
    }
    case EN_ADPT_KIND_RESISTOR: {
        double ohms = 0.0;
        (void)resistance_ohms(find_param(component, "resistance"), &ohms);
        status = add_primitive(adapter, result, component_index,
                               role_index(component, "a"), EN_DC_RESISTOR,
                               (unsigned)node_of(adapter, component_index,
                                                 role_index(component, "a")),
                               (unsigned)node_of(adapter, component_index,
                                                 role_index(component, "b")),
                               ohms, 0.0);
        break;
    }
    case EN_ADPT_KIND_POTENTIOMETER: {
        double ohms = 0.0, ratio = 0.0;
        size_t a = role_index(component, "a");
        size_t w = role_index(component, "w");
        size_t b = role_index(component, "b");
        unsigned na = (unsigned)node_of(adapter, component_index, a);
        unsigned nw = (unsigned)node_of(adapter, component_index, w);
        unsigned nb = (unsigned)node_of(adapter, component_index, b);
        (void)resistance_ohms(find_param(component, "resistance"), &ohms);
        (void)position_ratio(find_param(component, "position"), &ratio);
        if (ratio == 0.0) {
            /* na == nw after the validation-phase merge. */
            status = add_primitive(adapter, result, component_index, a,
                                   EN_DC_RESISTOR, na, nb, ohms, 0.0);
        } else if (ratio == 1.0) {
            status = add_primitive(adapter, result, component_index, a,
                                   EN_DC_RESISTOR, na, nw, ohms, 0.0);
        } else {
            status = add_primitive(adapter, result, component_index, a,
                                   EN_DC_RESISTOR, na, nw, ohms * ratio, 0.0);
            if (status == EN_ADPT_OK) {
                status = add_primitive(adapter, result, component_index, w,
                                       EN_DC_RESISTOR, nw, nb,
                                       ohms * (1.0 - ratio), 0.0);
            }
        }
        break;
    }
    case EN_ADPT_KIND_SWITCH: {
        const EnAdptQuantity *state = find_param(component, "state");
        size_t a = role_index(component, "a");
        size_t b = role_index(component, "b");
        if (state->unit == EN_ADPT_UNIT_STATE_CLOSED) {
            unsigned na = node_of(adapter, component_index, a);
            unsigned nb = node_of(adapter, component_index, b);
            double ohms = 0.0;
            (void)resistance_ohms(find_param(component, "on_resistance"), &ohms);
            status = add_primitive(adapter, result, component_index, a,
                                   EN_DC_RESISTOR, na, nb, ohms, 0.0);
        } else {
            const EnAdptQuantity *off_resistance =
                find_param(component, "off_resistance");
            double ohms = 0.0;
            /* An open switch is no connection unless an explicit finite
             * off_resistance models the capacitive/leakage path. */
            if (off_resistance) {
                unsigned na = node_of(adapter, component_index, a);
                unsigned nb = node_of(adapter, component_index, b);
                (void)resistance_ohms(off_resistance, &ohms);
                status = add_primitive(adapter, result, component_index, a,
                                       EN_DC_RESISTOR, na, nb, ohms, 0.0);
            }
        }
        break;
    }
    case EN_ADPT_KIND_VOLTAGE_SOURCE: {
        double volts = 0.0, ohms = 0.0;
        const EnAdptQuantity *series = find_param(component, "series_resistance");
        size_t p = role_index(component, "p");
        size_t n = role_index(component, "n");
        unsigned np = (unsigned)node_of(adapter, component_index, p);
        unsigned nn = (unsigned)node_of(adapter, component_index, n);
        (void)voltage_volts(find_param(component, "voltage"), &volts);
        if (series) {
            (void)resistance_ohms(series, &ohms);
            /* Thevenin form: the series resistance is the explicit driver
             * impedance, not an extra ideal constraint. */
            status = add_primitive(adapter, result, component_index, p,
                                   EN_DC_DRIVER, np, nn, volts, ohms);
        } else {
            status = add_primitive(adapter, result, component_index, p,
                                   EN_DC_VOLTAGE_SOURCE, np, nn, volts, 0.0);
        }
        break;
    }
    case EN_ADPT_KIND_CURRENT_SOURCE: {
        double amps = 0.0, ohms = 0.0;
        const EnAdptQuantity *series = find_param(component, "series_resistance");
        size_t p = role_index(component, "p");
        size_t n = role_index(component, "n");
        unsigned np = (unsigned)node_of(adapter, component_index, p);
        unsigned nn = (unsigned)node_of(adapter, component_index, n);
        (void)current_amps(find_param(component, "current"), &amps);
        status = add_primitive(adapter, result, component_index, p,
                               EN_DC_CURRENT_SOURCE, np, nn, amps, 0.0);
        if (status == EN_ADPT_OK && series) {
            (void)resistance_ohms(series, &ohms);
            /* Norton form: explicit shunt resistor in parallel with the ideal
             * source. */
            status = add_primitive(adapter, result, component_index, p,
                                   EN_DC_RESISTOR, np, nn, ohms, 0.0);
        }
        break;
    }
    case EN_ADPT_KIND_MCU:
        for (size_t t = 0; t < component->terminal_count; ++t) {
            const EnAdptTerminal *terminal = &component->terminals[t];
            size_t flat = adapter->terminal_offset[component_index] + t;
            if (terminal->driver.drive == EN_ADPT_DRIVE_NONE) {
                continue;
            }
            status = add_primitive(
                adapter, result, component_index, t, EN_DC_DRIVER,
                adapter->net_node[adapter->net_of_terminal[flat]],
                adapter->net_node[adapter
                                      ->net_of_terminal[adapter->driver_return_flat[flat]]],
                terminal->driver.voltage_v, terminal->driver.impedance_ohm);
            if (status != EN_ADPT_OK) {
                break;
            }
        }
        break;
    case EN_ADPT_KIND_GROUND:
    case EN_ADPT_KIND_DEVICE:
    default:
        /* Grounds merge nets (phase 5/6); capacitors and devices never reach
         * the build phase. */
        break;
    }
    return status;
}

static EnAdptStatus adapter_run(const EnAdptGraph *graph, EnAdptResult *result,
                               EnAdptProjection *projection, int allow_rc,
                               const EnAdptModelBinding *models, size_t model_count)
{
    Adapter adapter;
    EnDcResult kernel;
    size_t next_node = 1;
    EnAdptStatus status;

    if (!result) {
        return EN_ADPT_INVALID_ABI;
    }
    memset(result, 0, sizeof(*result));
    result->bad_component = SIZE_MAX;
    result->bad_net = SIZE_MAX;
    for (size_t i = 0; i < EN_ADPT_MAX_NETS; ++i) {
        result->nets[i].voltage_v = NAN;
    }
    for (size_t i = 0; i < EN_ADPT_MAX_TERMINALS; ++i) {
        result->terminals[i].voltage_v = NAN;
    }
    for (size_t i = 0; i < EN_DC_MAX_ELEMENTS; ++i) {
        result->primitives[i].current_a = NAN;
    }
    if (!graph || graph->abi_version != EN_ADPT_ABI_VERSION) {
        return fail(result, EN_ADPT_INVALID_ABI, SIZE_MAX, SIZE_MAX, "",
                    "Graph ABI mismatch: expected abi_version %u with non-NULL "
                    "graph", EN_ADPT_ABI_VERSION);
    }
    memset(&adapter, 0, sizeof(adapter));
    adapter.graph = graph;
    adapter.projection = projection;
    adapter.allow_rc = allow_rc;
    adapter.models = models;
    adapter.model_count = model_count;
    if (projection) {
        memset(projection, 0, sizeof(*projection));
    }
    for (size_t i = 0; i < EN_ADPT_MAX_TERMINALS; ++i) {
        adapter.net_of_terminal[i] = -1;
        adapter.driver_return_flat[i] = -1;
    }
    for (size_t ni = 0; ni < MAX_GROUPS; ++ni) {
        adapter.net_parent[ni] = (unsigned)ni;
    }

    /* Phase 1: input bounds. */
    if (graph->component_count > EN_ADPT_MAX_COMPONENTS) {
        return fail(result, EN_ADPT_EXCEEDS_LIMITS, SIZE_MAX, SIZE_MAX, "",
                    "%zu components exceed the adapter bound of %d",
                    graph->component_count, EN_ADPT_MAX_COMPONENTS);
    }
    if (graph->net_count > EN_ADPT_MAX_NETS) {
        return fail(result, EN_ADPT_EXCEEDS_LIMITS, SIZE_MAX, SIZE_MAX, "",
                    "%zu nets exceed the adapter bound of %d", graph->net_count,
                    EN_ADPT_MAX_NETS);
    }
    if (graph->component_count > 0 && !graph->components) {
        return fail(result, EN_ADPT_INVALID_ABI, SIZE_MAX, SIZE_MAX, "",
                    "Component array is NULL");
    }
    if (graph->net_count > 0 && !graph->nets) {
        return fail(result, EN_ADPT_INVALID_ABI, SIZE_MAX, SIZE_MAX, "",
                    "Net array is NULL");
    }
    if (model_count > EN_ADPT_MAX_COMPONENTS || (model_count && !models)) {
        return fail(result, EN_ADPT_INVALID_ABI, SIZE_MAX, SIZE_MAX, "",
                    "Invalid native model registry bounds");
    }
    for (size_t i = 0; i < model_count; ++i) {
        if (models[i].component_index >= graph->component_count ||
            !graph->components ||
            graph->components[models[i].component_index].kind != EN_ADPT_KIND_DEVICE) {
            return fail(result, EN_ADPT_INVALID_ABI, SIZE_MAX, SIZE_MAX, "",
                        "Native model binding does not name an existing DEVICE");
        }
        for (size_t j = 0; j < i; ++j) {
            if (models[i].component_index == models[j].component_index) {
                return fail(result, EN_ADPT_DUPLICATE_ID, models[i].component_index,
                            SIZE_MAX, "", "Duplicate native model binding");
            }
        }
    }

    /* Phase 2: ID grammar and global uniqueness. */
    for (size_t ci = 0; ci < graph->component_count; ++ci) {
        const EnAdptComponent *component = &graph->components[ci];
        if (!id_valid(component->id.text)) {
            return fail(result, EN_ADPT_INVALID_ID, ci, SIZE_MAX,
                        component->id.text,
                        "Component ID '%s' violates the v3 id grammar",
                        component->id.text);
        }
        for (size_t cj = 0; cj < ci; ++cj) {
            if (strcmp(graph->components[cj].id.text, component->id.text) == 0) {
                return fail(result, EN_ADPT_DUPLICATE_ID, ci, SIZE_MAX,
                            component->id.text,
                            "Component ID '%s' is declared twice",
                            component->id.text);
            }
        }
        if (component->terminal_count > 0 && !component->terminals) {
            return fail(result, EN_ADPT_INVALID_ABI, ci, SIZE_MAX,
                        component->id.text,
                        "Component '%s' terminal array is NULL",
                        component->id.text);
        }
        if (component->parameter_count > 0 && !component->parameters) {
            return fail(result, EN_ADPT_INVALID_ABI, ci, SIZE_MAX,
                        component->id.text,
                        "Component '%s' parameter array is NULL",
                        component->id.text);
        }
        if (component->terminal_count >
            EN_ADPT_MAX_TERMINALS - adapter.terminal_total) {
            return fail(result, EN_ADPT_EXCEEDS_LIMITS, ci, SIZE_MAX,
                        component->id.text,
                        "Component '%s' pushes terminals past the %d bound",
                        component->id.text, EN_ADPT_MAX_TERMINALS);
        }
        adapter.terminal_offset[ci] = adapter.terminal_total;
        for (size_t t = 0; t < component->terminal_count; ++t) {
            const EnAdptTerminal *terminal = &component->terminals[t];
            size_t flat = adapter.terminal_total;
            if (!id_valid(terminal->id.text)) {
                return fail(result, EN_ADPT_INVALID_ID, ci, SIZE_MAX,
                            terminal->id.text,
                            "Terminal ID '%s' on component '%s' violates the v3 "
                            "id grammar", terminal->id.text, component->id.text);
            }
            for (size_t s = 0; s < flat; ++s) {
                if (strcmp(adapter.flat_terminal[s]->id.text,
                           terminal->id.text) == 0) {
                    return fail(result, EN_ADPT_DUPLICATE_ID, ci, SIZE_MAX,
                                terminal->id.text,
                                "Terminal ID '%s' is declared twice (globally "
                                "unique terminal identity is required for "
                                "endpoint resolution)", terminal->id.text);
                }
            }
            adapter.flat_terminal[flat] = terminal;
            adapter.terminal_total = flat + 1;
        }
    }
    adapter.terminal_offset[graph->component_count] = adapter.terminal_total;
    result->terminal_count = adapter.terminal_total;
    result->net_count = graph->net_count;

    for (size_t ni = 0; ni < graph->net_count; ++ni) {
        const EnAdptNet *net = &graph->nets[ni];
        if (!id_valid(net->id.text)) {
            return fail(result, EN_ADPT_INVALID_ID, SIZE_MAX, ni, net->id.text,
                        "Net ID '%s' violates the v3 id grammar", net->id.text);
        }
        for (size_t nj = 0; nj < ni; ++nj) {
            if (strcmp(graph->nets[nj].id.text, net->id.text) == 0) {
                return fail(result, EN_ADPT_DUPLICATE_ID, SIZE_MAX, ni,
                            net->id.text, "Net ID '%s' is declared twice",
                            net->id.text);
            }
        }
        if (net->endpoint_count > 0 && !net->endpoints) {
            return fail(result, EN_ADPT_INVALID_ABI, SIZE_MAX, ni, net->id.text,
                        "Net '%s' endpoint array is NULL", net->id.text);
        }
    }

    /* Phase 3: endpoint resolution. Unknown IDs, duplicates within a net and
     * one terminal shared by two nets all fail explicitly. */
    for (size_t ni = 0; ni < graph->net_count; ++ni) {
        const EnAdptNet *net = &graph->nets[ni];
        for (size_t e = 0; e < net->endpoint_count; ++e) {
            const char *endpoint = net->endpoints[e].text;
            int found = -1;
            if (!id_valid(endpoint)) {
                return fail(result, EN_ADPT_INVALID_ID, SIZE_MAX, ni, endpoint,
                            "Net '%s' endpoint '%s' violates the v3 id grammar",
                            net->id.text, endpoint);
            }
            for (size_t prior = 0; prior < e; ++prior) {
                if (strcmp(net->endpoints[prior].text, endpoint) == 0) {
                    return fail(result, EN_ADPT_DUPLICATE_ENDPOINT, SIZE_MAX,
                                ni, endpoint,
                                "Net '%s' lists endpoint '%s' twice",
                                net->id.text, endpoint);
                }
            }
            for (size_t s = 0; s < adapter.terminal_total; ++s) {
                if (strcmp(adapter.flat_terminal[s]->id.text, endpoint) == 0) {
                    found = (int)s;
                    break;
                }
            }
            if (found < 0) {
                return fail(result, EN_ADPT_UNKNOWN_ENDPOINT, SIZE_MAX, ni,
                            endpoint,
                            "Net '%s' endpoint '%s' resolves to no actual "
                            "terminal of any existing component",
                            net->id.text, endpoint);
            }
            if (adapter.net_of_terminal[found] >= 0) {
                return fail(result, EN_ADPT_TERMINAL_CONFLICT, SIZE_MAX, ni,
                            endpoint,
                            "Terminal '%s' appears on net '%s' and also on net "
                            "'%s'; one net per terminal is required",
                            endpoint,
                            graph->nets[adapter.net_of_terminal[found]].id.text,
                            net->id.text);
            }
            adapter.net_of_terminal[found] = (int)ni;
        }
    }

    /* Incomplete v3 wiring is a physical state, not a graph-wide error.
     * Every unwired terminal is distinct; no hidden wire or rail is added.
     * An empty named net has no physical node and consumes no solver capacity. */
    adapter.group_count = graph->net_count;
    for (size_t ni = 0; ni < graph->net_count; ++ni) {
        adapter.group_id[ni] = graph->nets[ni].id.text;
        adapter.group_used[ni] = graph->nets[ni].endpoint_count > 0;
    }
    for (size_t s = 0; s < adapter.terminal_total; ++s) {
        if (adapter.net_of_terminal[s] < 0) {
            size_t group = adapter.group_count++;
            adapter.net_of_terminal[s] = (int)group;
            adapter.group_id[group] = adapter.flat_terminal[s]->id.text;
        }
    }

    /* Phase 4: strict per-component validation (also unions exact-endpoint
     * potentiometer wipers and marks ground nets). */
    for (size_t ci = 0; ci < graph->component_count; ++ci) {
        status = validate_component(&adapter, result, ci);
        if (status != EN_ADPT_OK) {
            return status;
        }
    }

    /* Unwired input pads and open switches contribute no equations; report
     * them unknown without spending the bounded solver's node capacity. */
    for (size_t ci = 0; ci < graph->component_count; ++ci) {
        const EnAdptComponent *component = &graph->components[ci];
        int contributes = component->kind != EN_ADPT_KIND_MCU;
        if (component->kind == EN_ADPT_KIND_SWITCH &&
            find_param(component, "state")->unit == EN_ADPT_UNIT_STATE_OPEN &&
            !find_param(component, "off_resistance")) {
            contributes = 0;
        }
        for (size_t t = 0; t < component->terminal_count; ++t) {
            size_t flat = adapter.terminal_offset[ci] + t;
            if (contributes || component->terminals[t].driver.drive != EN_ADPT_DRIVE_NONE) {
                adapter.group_used[adapter.net_of_terminal[flat]] = 1;
            }
            if (adapter.driver_return_flat[flat] >= 0) {
                int ret = adapter.driver_return_flat[flat];
                adapter.group_used[adapter.net_of_terminal[ret]] = 1;
            }
        }
    }

    /* Phase 5: deterministic node assignment. Ground-flagged net groups
     * become the declared reference node 0; every other group receives the
     * next free node in first-appearance order. With no ground at all node 0
     * stays unused and every island solves floating (no fabricated ground).
     * (The parent table was initialized before validation because exact-
     * endpoint potentiometer wiper merges union during phase 4.) */
    for (size_t ni = 0; ni < adapter.group_count; ++ni) {
        if (adapter.net_is_ground[ni]) {
            adapter.root_is_ground[net_find(adapter.net_parent, (unsigned)ni)] = 1;
        }
    }
    for (size_t ni = 0; ni < adapter.group_count; ++ni) {
        unsigned root;
        if (!adapter.group_used[ni]) {
            adapter.net_node[ni] = UINT_MAX;
            continue;
        }
        root = net_find(adapter.net_parent, (unsigned)ni);
        if (adapter.root_is_ground[root]) {
            adapter.net_node[ni] = 0;
            adapter.net_is_reference[ni] = 1;
        } else {
            if (adapter.node_of_root[root] == 0) {
                if (next_node >= EN_DC_MAX_NODES) {
                    return fail(result, EN_ADPT_EXCEEDS_LIMITS, SIZE_MAX,
                                ni < graph->net_count ? ni : SIZE_MAX,
                                adapter.group_id[ni],
                                "Projection needs more than %d non-reference "
                                "nodes", EN_DC_MAX_NODES - 1);
                }
                adapter.node_of_root[root] = (unsigned)next_node++;
            }
            adapter.net_node[ni] = adapter.node_of_root[root];
        }
    }

    /* Phase 6: project to primitives. */
    adapter.circuit.node_count = (size_t)next_node;
    for (size_t ci = 0; ci < graph->component_count; ++ci) {
        status = build_component(&adapter, result, ci);
        if (status != EN_ADPT_OK) {
            return status;
        }
    }
    result->node_count = adapter.circuit.node_count;
    result->element_count = adapter.circuit.element_count;
    result->primitive_count = adapter.circuit.element_count;
    if (projection) {
        projection->circuit.node_count = adapter.circuit.node_count;
        projection->circuit.element_count = adapter.circuit.element_count;
        for (size_t i = 0; i < graph->net_count; ++i) {
            projection->net_node[i] = adapter.net_node[i];
        }
        for (size_t i = 0; i < adapter.terminal_total; ++i) {
            projection->terminal_node[i] =
                adapter.net_node[adapter.net_of_terminal[i]];
        }
        for (size_t i = 0; i < adapter.circuit.element_count; ++i) {
            projection->primitive_component[i] = adapter.prim_component[i];
            projection->primitive_terminal[i] = adapter.prim_terminal[i];
        }
        result->status = EN_ADPT_OK;
        snprintf(result->diagnostic, sizeof(result->diagnostic),
                 "Validated v3 %s topology projection", allow_rc ? "RC" : "DC");
        return EN_ADPT_OK;
    }

    /* Phase 7: solve and map back. Kernel statuses map explicitly; the enum
     * value spaces are unrelated. */
    {
        EnDcStatus kernel_status = en_dc_solve(&adapter.circuit, &kernel);
        switch (kernel_status) {
        case EN_DC_OK:
            status = EN_ADPT_OK;
            break;
        case EN_DC_FLOATING:
            status = EN_ADPT_FLOATING;
            break;
        case EN_DC_INVALID:
            status = EN_ADPT_SOLVER_INVALID;
            break;
        case EN_DC_SINGULAR:
            status = EN_ADPT_SINGULAR;
            break;
        case EN_DC_NO_OPERATING_POINT:
            status = EN_ADPT_NO_OPERATING_POINT;
            break;
        case EN_DC_NUMERICAL:
            status = EN_ADPT_NUMERICAL;
            break;
        case EN_DC_ALLOCATION_FAILED:
            status = EN_ADPT_ALLOCATION_FAILED;
            break;
        default:
            status = EN_ADPT_SOLVER_INVALID;
            break;
        }
    }
    if (status != EN_ADPT_OK && status != EN_ADPT_FLOATING) {
        size_t component = SIZE_MAX;
        const char *offender = "";
        if (kernel.bad_element != SIZE_MAX &&
            kernel.bad_element < adapter.circuit.element_count) {
            component = adapter.prim_component[kernel.bad_element];
            offender = graph->components[component].id.text;
        }
        return fail(result, status, component, SIZE_MAX, offender,
                    "Kernel rejected the projection: %s",
                    kernel.diagnostic[0] ? kernel.diagnostic : "(no detail)");
    }
    for (size_t ni = 0; ni < graph->net_count; ++ni) {
        EnAdptNetResult *out = &result->nets[ni];
        out->node = adapter.net_node[ni];
        out->is_reference = adapter.net_is_reference[ni];
        if (out->node == UINT_MAX) {
            out->floating = 1;
            out->voltage_v = NAN;
            status = EN_ADPT_FLOATING;
        } else {
            out->floating = kernel.floating[out->node];
            out->voltage_v = kernel.voltage[out->node];
        }
    }
    for (size_t s = 0; s < adapter.terminal_total; ++s) {
        EnAdptTerminalResult *out = &result->terminals[s];
        int group = adapter.net_of_terminal[s];
        unsigned node = adapter.net_node[group];
        out->on_net = (size_t)group < graph->net_count;
        if (node == UINT_MAX) {
            out->voltage_v = NAN;
            out->floating = 1;
            status = EN_ADPT_FLOATING;
        } else {
            out->voltage_v = kernel.voltage[node];
            out->floating = kernel.floating[node];
        }
    }
    for (size_t e = 0; e < adapter.circuit.element_count; ++e) {
        EnAdptPrimitiveResult *out = &result->primitives[e];
        out->component_index = adapter.prim_component[e];
        out->terminal_index = adapter.prim_terminal[e];
        out->current_a = kernel.current[e];
    }
    result->max_kcl_residual_a = kernel.max_kcl_residual_a;
    result->max_source_residual_v = kernel.max_source_residual_v;
    result->status = status;
    snprintf(result->diagnostic, sizeof(result->diagnostic),
             "v3 graph solved: %zu nets on %zu nodes, %zu primitives; kernel: %.150s",
             graph->net_count, adapter.circuit.node_count,
             adapter.circuit.element_count,
             kernel.diagnostic[0] ? kernel.diagnostic : "ok");
    return status;
}

EnAdptStatus en_adpt_solve(const EnAdptGraph *graph, EnAdptResult *result)
{
    return adapter_run(graph, result, NULL, 0, NULL, 0);
}

EnAdptStatus en_adpt_project(const EnAdptGraph *graph, int allow_rc,
                            EnAdptProjection *projection, EnAdptResult *result)
{
    if (!projection) {
        return EN_ADPT_INVALID_ABI;
    }
    return adapter_run(graph, result, projection, allow_rc, NULL, 0);
}

EnAdptStatus en_adpt_project_models(const EnAdptGraph *graph, int allow_rc,
                                   const EnAdptModelBinding *models, size_t count,
                                   EnAdptProjection *projection, EnAdptResult *result)
{
    if (!projection) {
        return EN_ADPT_INVALID_ABI;
    }
    return adapter_run(graph, result, projection, allow_rc, models, count);
}
