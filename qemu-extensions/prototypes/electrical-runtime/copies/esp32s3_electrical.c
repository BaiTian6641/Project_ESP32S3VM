/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include <math.h>
#include "qemu/cutils.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qemu/main-loop.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "qapi/qmp/qdict.h"
#include "qapi/qmp/qlist.h"
#include "qapi/qmp/qjson.h"
#include "qapi/qmp/qnum.h"
#include "sysemu/runstate.h"
#include "hw/misc/esp32s3_adc_provider.h"
#include "hw/misc/esp32s3_rtc_cntl.h"
#include "hw/misc/esp32s3_electrical.h"
#include "esp32s3_electrical_internal.h"
#include "esp32s3_electrical_observers.h"
#include "esp32s3_project.h"

#define DRIVER_OHM 40.0
#define PULL_OHM 45000.0
#define DELTA_LIMIT 16
#define RC_HORIZON_NS 100000

typedef struct NativeMatrixDrive {
    bool present, oe, level, open_drain;
} NativeMatrixDrive;

typedef struct NativeFactory {
    ESP32S3ElectricalPreflight preflight;
    ESP32S3ElectricalActivate activate;
    void *opaque;
} NativeFactory;

struct ESP32S3ElectricalState {
    DeviceState parent_obj;
    ESP32S3GPIOState *gpio;
    ESP32S3RtcIoState *rtc;
    Esp32S3Project *project;
    EnDcWorkspace *dc;
    EnRcSolver *rc, *probe;
    EnRcCircuit circuit;
    EnRcSample sample;
    QEMUTimer *timer;
    QEMUBH *pause_bh;
    uint64_t generation;
    uint64_t source_generation;
    int64_t sample_ns;
    bool settling, dirty, failed;
    bool drive_owned[49];
    unsigned pad_node[49];
    NativeMatrixDrive matrix[256];
    ESP32S3ElectricalObserverSet observers;
    NativeFactory factories[10];
    unsigned batch_depth;
    bool notifying;
    char diagnostic[256];
};

static void publish_pads(ESP32S3ElectricalState *s);
static void settle_sources(ESP32S3ElectricalState *s);
static void publish_frame(ESP32S3ElectricalState *s);

static bool rc_ok(EnRcStatus status)
{
    return status == EN_RC_OK || status == EN_RC_FLOATING || status == EN_RC_CROSSED;
}

static void pause_dependency(void *opaque)
{
    ESP32S3ElectricalState *s = opaque;
    error_report("native electrical dependency: %s", s->diagnostic);
    vm_stop(RUN_STATE_PAUSED);
}

static void failure(ESP32S3ElectricalState *s, const char *diagnostic)
{
    s->failed = true;
    pstrcpy(s->diagnostic, sizeof(s->diagnostic), diagnostic);
    timer_del(s->timer);
    s->sample_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->sample.max_kcl_residual_a = NAN;
    s->sample.max_source_residual_v = NAN;
    for (size_t n = 0; n < EN_RC_MAX_NODES; ++n) {
        s->sample.voltage[n] = NAN;
        s->sample.floating[n] = true;
    }
    for (size_t i = 0; i < EN_RC_MAX_ELEMENTS; ++i) {
        s->sample.current[i] = NAN;
    }
    s->sample.status = EN_RC_NUMERICAL;
    esp32s3_electrical_models_update_power(s->project, &s->sample, s->sample_ns);
    qemu_bh_schedule(s->pause_bh);
    publish_pads(s);
}

static bool add_branch(EnRcCircuit *c, unsigned p, unsigned n, double resistance,
                       Error **errp)
{
    if (p == n) {
        /* A finite driver attached to its own rail has zero branch voltage,
         * no equation and no current; no zero-ohm connection is inserted. */
        return true;
    }
    if (c->element_count == EN_RC_MAX_ELEMENTS) {
        error_setg(errp, "Firmware drive/pulls exceed %d total electrical primitives", EN_RC_MAX_ELEMENTS);
        return false;
    }
    c->elements[c->element_count++] = (EnRcElement){
        .kind = EN_RC_RESISTOR, .p = p, .n = n, .value = resistance
    };
    return true;
}

static unsigned uart_iomux_output(unsigned pad, unsigned function)
{
    /* ESP32-S3 uart_pins.h and io_mux_reg.h: these direct outputs select the
     * same controller sources as the matrix, but do not use GPIO_ENABLE or
     * FUNC_OUT_SEL. UART2 has no direct IO_MUX pins. */
    switch (pad) {
    case 15: return function == 2 ? 13 : 256; /* U0RTS */
    case 17: return function == 2 ? 15 : 256; /* U1TXD */
    case 19: return function == 2 ? 16 : 256; /* U1RTS */
    case 43: return function == 0 ? 12 : 256; /* U0TXD */
    default: return 256;
    }
}

static bool build_drives(ESP32S3ElectricalState *s, Esp32S3Project *p,
                         EnRcCircuit *c, bool *owned, unsigned *pad_nodes,
                         Error **errp)
{
    if (!s->gpio->rtc_cntl || s->rtc->rtc_cntl != s->gpio->rtc_cntl) {
        error_setg(errp, "Native RTC GPIO mode provider is unavailable");
        return false;
    }
    if (esp32s3_gpio_sleep_active(s->gpio)) {
        error_setg(errp, "Actual RTC GPIO sleep request is unavailable in the electrical profile");
        return false;
    }
    if (esp32s3_rtc_cntl_dig_pad_hold(s->gpio->rtc_cntl) ||
        esp32s3_rtc_cntl_rtc_pad_hold(s->gpio->rtc_cntl)) {
        error_setg(errp, "Actual RTC GPIO hold request is unavailable in the electrical profile");
        return false;
    }
    *c = p->projection.circuit;
    unsigned vdd = p->projection.terminal_node[p->vdd_terminal];
    unsigned gnd = p->projection.terminal_node[p->gnd_terminal];
    memset(owned, 0, 49 * sizeof(bool));
    for (unsigned pad = 0; pad < 49; ++pad) {
        pad_nodes[pad] = UINT_MAX;
        if (pad >= 22 && pad <= 25) {
            continue;
        }
        int flat = p->pad_terminal[pad];
        ESP32S3GpioDriveSnapshot digital;
        ESP32S3RtcIoDriveSnapshot rtc = {0};
        esp32s3_gpio_get_drive_snapshot(s->gpio, pad, &digital);
        if (pad < 22) {
            esp32s3_rtc_io_get_drive_snapshot(s->rtc, pad, &rtc);
        }
        bool oe, level, drain, up, down;
        if (rtc.mux_sel) {
            oe = rtc.out_oe;
            level = rtc.out_bit;
            drain = rtc.open_drain;
            up = rtc.rue;
            down = rtc.rde;
            if (rtc.hold) {
                error_setg(errp, "GPIO%u RTC hold/sleep electrical behavior is unavailable", pad);
                return false;
            }
        } else {
            if (digital.hold || digital.analog_owned) {
                error_setg(errp, "GPIO%u hold/sleep/dedicated analog ownership is unavailable", pad);
                return false;
            }
            /* Only real native controller sources can drive non-GPIO
             * matrix selectors. Resolved pad levels are never fed back. */
            oe = false;
            level = digital.out_level ^ digital.out_inv;
            drain = digital.open_drain;
            if (digital.matrix_gpio) {
                if (digital.out_sel == 256) {
                    oe = digital.out_oe ^ digital.oen_inv;
                } else if (digital.out_sel < 256 && s->matrix[digital.out_sel].present) {
                    NativeMatrixDrive *source = &s->matrix[digital.out_sel];
                    bool signal_oe = source->oe &&
                        (!source->open_drain || !source->level);
                    oe = (digital.oen_from_signal ? signal_oe : digital.out_oe) ^ digital.oen_inv;
                    level = source->level ^ digital.out_inv;
                } else if (digital.out_oe) {
                    error_setg(errp, "GPIO%u matrix signal %u has no native drive provider", pad, digital.out_sel);
                    return false;
                }
            } else {
                unsigned signal = uart_iomux_output(pad, digital.mcu_sel);
                if (signal < 256 && s->matrix[signal].present) {
                    NativeMatrixDrive *source = &s->matrix[signal];
                    oe = source->oe && (!source->open_drain || !source->level);
                    level = source->level;
                }
            }
            up = digital.pull_up;
            down = digital.pull_down;
        }
        unsigned node = flat < 0 ? UINT_MAX : p->projection.terminal_node[flat];
        bool active_drive = oe && (!drain || !level);
        if (node == UINT_MAX && (active_drive || up || down)) {
            if (c->node_count == EN_RC_MAX_NODES) {
                error_setg(errp, "Physical GPIO%u intrinsic node exceeds the %u-node limit",
                           pad, EN_RC_MAX_NODES);
                return false;
            }
            node = c->node_count++;
        }
        pad_nodes[pad] = node;
        owned[pad] = oe;
        if (node == UINT_MAX) {
            continue;
        }
        if (oe && (!drain || !level) &&
            !add_branch(c, node, level && !drain ? vdd : gnd, DRIVER_OHM, errp)) {
            return false;
        }
        if ((up && !add_branch(c, node, vdd, PULL_OHM, errp)) ||
            (down && !add_branch(c, node, gnd, PULL_OHM, errp))) {
            return false;
        }
    }
    for (size_t i = 0; i < p->device_count; ++i) {
        Esp32S3ProjectDevice *d = &p->devices[i];
        unsigned ground = p->projection.terminal_node[d->gnd_terminal];
        unsigned supply = p->projection.terminal_node[d->vdd_terminal];
        for (size_t j = 0; j < d->drive_count; ++j) {
            Esp32S3ProjectDrive *drive = &d->drives[j];
            if (!drive->oe) {
                continue;
            }
            unsigned destination = drive->open_drain || !drive->level ? ground : supply;
            if (!add_branch(c, p->projection.terminal_node[drive->flat_terminal],
                            destination, DRIVER_OHM, errp)) {
                return false;
            }
        }
    }
    return true;
}

static bool circuit_equal(const EnRcCircuit *a, const EnRcCircuit *b)
{
    if (a->node_count != b->node_count || a->element_count != b->element_count) {
        return false;
    }
    for (size_t i = 0; i < a->element_count; ++i) {
        const EnRcElement *x = &a->elements[i], *y = &b->elements[i];
        if (x->kind != y->kind || x->p != y->p || x->n != y->n ||
            x->value != y->value || x->resistance != y->resistance ||
            x->initial_voltage != y->initial_voltage) {
            return false;
        }
    }
    return true;
}

static EnDcStatus solve_dc(ESP32S3ElectricalState *s, const EnRcCircuit *c,
                           EnRcSample *sample)
{
    EnDcCircuit dc = {.node_count = c->node_count, .element_count = c->element_count};
    static const EnDcKind kinds[] = {EN_DC_RESISTOR, EN_DC_RESISTOR,
                                    EN_DC_VOLTAGE_SOURCE, EN_DC_CURRENT_SOURCE, EN_DC_DRIVER};
    for (size_t i = 0; i < c->element_count; ++i) {
        const EnRcElement *e = &c->elements[i];
        if (e->kind == EN_RC_CAPACITOR) {
            return EN_DC_INVALID;
        }
        dc.elements[i] = (EnDcElement){kinds[e->kind], e->p, e->n, e->value, e->resistance};
    }
    EnDcResult out;
    EnDcStatus status = en_dc_workspace_solve(s->dc, &dc, &out);
    memset(sample, 0, sizeof(*sample));
    sample->status = status == EN_DC_OK ? EN_RC_OK : status == EN_DC_FLOATING ? EN_RC_FLOATING : EN_RC_NUMERICAL;
    memcpy(sample->voltage, out.voltage, sizeof(out.voltage));
    memcpy(sample->floating, out.floating, sizeof(out.floating));
    memcpy(sample->current, out.current, sizeof(out.current));
    sample->max_kcl_residual_a = out.max_kcl_residual_a;
    sample->max_source_residual_v = out.max_source_residual_v;
    pstrcpy(sample->diagnostic, sizeof(sample->diagnostic), out.diagnostic);
    return status;
}

static bool rails_declared(Esp32S3Project *p, Error **errp)
{
    bool vdd_net = false, gnd_net = false;
    for (size_t i = 0; i < p->graph.net_count; ++i) {
        for (size_t j = 0; j < p->nets[i].endpoint_count; ++j) {
            const char *id = p->nets[i].endpoints[j].text;
            vdd_net |= !strcmp(id, p->terminals[p->vdd_terminal].id.text);
            gnd_net |= !strcmp(id, p->terminals[p->gnd_terminal].id.text);
        }
    }
    if (!vdd_net || !gnd_net || p->projection.terminal_node[p->gnd_terminal] != 0) {
        error_setg(errp, "MCU vdd/gnd must be explicitly wired, with gnd connected to declared project ground");
        return false;
    }
    if (p->rc) {
        /* Fixed voltage thresholds can schedule exact crossings only when
         * the MCU rails are an explicit ideal DC reference pair. */
        unsigned rail = p->projection.terminal_node[p->vdd_terminal];
        bool supplied = false;
        for (size_t i = 0; i < p->projection.circuit.element_count; ++i) {
            EnRcElement *e = &p->projection.circuit.elements[i];
            if (e->kind == EN_RC_VOLTAGE_SOURCE &&
                ((e->p == rail && e->n == 0 && e->value > 0) ||
                 (e->n == rail && e->p == 0 && e->value < 0))) {
                supplied = true;
            }
        }
        if (!supplied) {
            error_setg(errp, "RC threshold scheduling requires explicit fixed ideal MCU vdd-to-gnd supply");
            return false;
        }
    }
    return true;
}

static void publish_pads(ESP32S3ElectricalState *s)
{
    if (!s->project) {
        return;
    }
    Esp32S3Project *p = s->project;
    double rail = s->sample.voltage[p->projection.terminal_node[p->vdd_terminal]];
    esp32s3_gpio_set_solver_owned(s->gpio, true);
    esp32s3_rtc_io_set_solver_owned(s->rtc, true);
    double ground = s->sample.voltage[p->projection.terminal_node[p->gnd_terminal]];
    for (unsigned pad = 0; pad < 49; ++pad) {
        if (pad >= 22 && pad <= 25) {
            continue;
        }
        unsigned node = s->pad_node[pad];
        double voltage = node == UINT_MAX ? NAN : s->sample.voltage[node];
        EnPadLevel level = en_dc_classify_pad(voltage - ground,
                                             node == UINT_MAX || s->sample.floating[node],
                                             rail - ground, 0.25, 0.75);
        bool valid = !s->failed && (level == EN_PAD_LOW || level == EN_PAD_HIGH);
        esp32s3_gpio_drive_net(s->gpio, pad, valid, level == EN_PAD_HIGH, voltage);
        if (pad < 22) {
            esp32s3_rtc_io_drive_net(s->rtc, pad, valid, level == EN_PAD_HIGH, voltage);
        }
    }
}

static void publish_frame(ESP32S3ElectricalState *s)
{
    publish_pads(s);
    if (s->notifying) {
        return;
    }
    s->notifying = true;
    if (!esp32s3_electrical_observers_publish(&s->observers, s->sample_ns)) {
        failure(s, "Native electrical observer dispatch rejected reentrance");
    }
    s->notifying = false;
}

static bool advance_now(ESP32S3ElectricalState *s, int64_t ns)
{
    if (!s->project || s->failed) {
        return !s->failed;
    }
    if (!s->rc || ns == s->sample_ns) {
        return true;
    }
    EnRcAdvance advance;
    EnRcStatus status = en_rc_advance(s->rc, ns * 1e-9, NULL, &advance);
    if (!rc_ok(status) || !rc_ok(en_rc_sample(s->rc, &s->sample))) {
        failure(s, !rc_ok(status) ? advance.diagnostic : s->sample.diagnostic);
        return false;
    }
    s->sample_ns = ns;
    esp32s3_electrical_models_update_power(s->project, &s->sample, ns);
    return true;
}

static bool restore_probe(ESP32S3ElectricalState *s)
{
    return rc_ok(en_rc_copy_state(s->probe, s->rc));
}

static void schedule_rc(ESP32S3ElectricalState *s)
{
    timer_del(s->timer);
    if (!s->project || !s->rc || s->failed) {
        return;
    }
    bool changing_charge = false;
    for (size_t i = 0; i < s->circuit.element_count; ++i) {
        if (s->circuit.elements[i].kind == EN_RC_CAPACITOR &&
            s->sample.current[i] != 0) {
            changing_charge = true;
            break;
        }
    }
    if (!changing_charge) {
        /* With static sources and exactly zero capacitor currents the
         * linear state is stationary; a source event or sample wakes it. */
        return;
    }
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int64_t deadline = now + RC_HORIZON_NS;
    Esp32S3Project *p = s->project;
    unsigned rail_node = p->projection.terminal_node[p->vdd_terminal];
    double rail = s->sample.voltage[rail_node];
    uint64_t watched_nodes = 0;
    for (unsigned pad = 0; pad < 49; ++pad) {
        unsigned node = s->pad_node[pad];
        if (node == UINT_MAX) {
            continue;
        }
        if (s->sample.floating[node] || !isfinite(rail) || rail <= 0) {
            continue;
        }
        ESP32S3GpioDriveSnapshot digital;
        ESP32S3RtcIoDriveSnapshot rtc = {0};
        esp32s3_gpio_get_drive_snapshot(s->gpio, pad, &digital);
        if (pad < 22) {
            esp32s3_rtc_io_get_drive_snapshot(s->rtc, pad, &rtc);
        }
        if (!(rtc.mux_sel ? rtc.fun_ie : digital.ie)) {
            continue;
        }
        if (node == 0 || node == rail_node ||
            (watched_nodes & (UINT64_C(1) << node))) {
            continue;
        }
        watched_nodes |= UINT64_C(1) << node;
        for (unsigned threshold = 0; threshold < 2; ++threshold) {
            double threshold_v = rail * (threshold ? 0.75 : 0.25);
            if (fabs(s->sample.voltage[node] - threshold_v) <=
                1e-6 + 1e-5 * fabs(threshold_v)) {
                /* The published numerical error band is also the crossing
                 * chatter guard; a reset probe must not re-fire a threshold
                 * while the live voltage is still indistinguishable from it. */
                continue;
            }
            if (!restore_probe(s)) {
                failure(s, "RC crossing probe could not restore declared charge state");
                return;
            }
            EnRcWatch watch = {.enabled = true, .node = node,
                              .threshold_v = threshold_v,
                              .edge = EN_RC_EDGE_ANY};
            EnRcAdvance result;
            EnRcStatus status = en_rc_advance(s->probe, deadline * 1e-9, &watch, &result);
            if (!rc_ok(status)) {
                failure(s, result.diagnostic);
                return;
            }
            if (status == EN_RC_CROSSED) {
                int64_t crossing = (int64_t)ceil(result.crossing_time_s * 1e9);
                deadline = MIN(deadline, MAX(now + 1, crossing));
            }
        }
    }
    timer_mod(s->timer, deadline);
}

static void settle_sources(ESP32S3ElectricalState *s)
{
    s->dirty = true;
    if (s->settling || s->batch_depth || !s->project) {
        return;
    }
    s->settling = true;
    unsigned delta;
    for (delta = 0; s->dirty && delta < DELTA_LIMIT; ++delta) {
        s->dirty = false;
        bool recovering = s->failed;
        s->failed = false;
        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        if (!advance_now(s, now)) {
            break;
        }
        EnRcCircuit next;
        bool owned[49];
        unsigned pad_nodes[49];
        Error *err = NULL;
        if (!build_drives(s, s->project, &next, owned, pad_nodes, &err)) {
            failure(s, error_get_pretty(err));
            error_free(err);
            break;
        }
        bool changed = !circuit_equal(&next, &s->circuit);
        if (changed || recovering) {
            if (s->rc) {
                if ((changed &&
                     (!rc_ok(en_rc_edit(s->rc, &next, EN_RC_KEEP_CHARGE)) ||
                      !rc_ok(en_rc_edit(s->probe, &next, EN_RC_RESET_CHARGE)))) ||
                    !rc_ok(en_rc_sample(s->rc, &s->sample))) {
                    failure(s, "RC firmware source transition failed bounded atomic settlement");
                    break;
                }
            } else {
                EnDcStatus status = solve_dc(s, &next, &s->sample);
                if (status != EN_DC_OK && status != EN_DC_FLOATING) {
                    failure(s, s->sample.diagnostic);
                    break;
                }
            }
            if (changed) {
                s->circuit = next;
                ++s->source_generation;
            }
        }
        memcpy(s->drive_owned, owned, sizeof(owned));
        memcpy(s->pad_node, pad_nodes, sizeof(pad_nodes));
        s->sample_ns = now;
        esp32s3_electrical_models_update_power(s->project, &s->sample, now);
        publish_frame(s);
    }
    if (s->dirty && delta == DELTA_LIMIT) {
        failure(s, "Native electrical oscillation: 16 reentrant delta cycles exhausted");
    }
    if (s->failed) {
        publish_frame(s);
    }
    s->settling = false;
    schedule_rc(s);
}

static void drive_changed(void *opaque)
{
    settle_sources(opaque);
}


static void timer_sample(void *opaque)
{
    ESP32S3ElectricalState *s = opaque;
    s->settling = true;
    advance_now(s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    publish_frame(s);
    s->settling = false;
    if (s->dirty) {
        settle_sources(s);
    } else {
        schedule_rc(s);
    }
}

static const char *capacitor_terminal(const EnAdptComponent *c, const char *role)
{
    for (size_t i = 0; i < c->terminal_count; ++i) {
        if (!strcmp(c->terminals[i].role, role)) {
            return c->terminals[i].id.text;
        }
    }
    return "";
}

static bool preserve_charge(ESP32S3ElectricalState *s, Esp32S3Project *p,
                            EnRcSolver *candidate, int64_t now,
                            bool graph_changed, Error **errp)
{
    if (!s->rc || (graph_changed && p->charge_policy != EN_RC_KEEP_CHARGE)) {
        return true;
    }
    /* Evaluate old charge at the Apply timestamp on the reusable probe.
     * A rejected graph must not mutate the current solver/time/charge. */
    EnRcAdvance advance;
    if (!restore_probe(s) ||
        !rc_ok(en_rc_advance(s->probe, now * 1e-9, NULL, &advance))) {
        error_setg(errp, "Old RC charge cannot advance to Apply timestamp");
        return false;
    }
    for (size_t ni = 0; ni < p->projection.circuit.element_count; ++ni) {
        if (p->projection.circuit.elements[ni].kind != EN_RC_CAPACITOR) {
            continue;
        }
        const EnAdptComponent *new_cap = &p->components[p->projection.primitive_component[ni]];
        for (size_t oi = 0; oi < s->project->projection.circuit.element_count; ++oi) {
            const EnAdptComponent *old_cap =
                &s->project->components[s->project->projection.primitive_component[oi]];
            if (s->project->projection.circuit.elements[oi].kind != EN_RC_CAPACITOR ||
                strcmp(new_cap->id.text, old_cap->id.text) ||
                strcmp(capacitor_terminal(new_cap, "p"), capacitor_terminal(old_cap, "p")) ||
                strcmp(capacitor_terminal(new_cap, "n"), capacitor_terminal(old_cap, "n"))) {
                continue;
            }
            EnRcStatus status;
            double charge = en_rc_capacitor_charge(s->probe, oi, &status);
            if (!rc_ok(status) ||
                !rc_ok(en_rc_set_capacitor_charge(candidate, ni, charge))) {
                error_setg(errp, "RC stable capacitor '%s' charge remap failed", new_cap->id.text);
                return false;
            }
            break;
        }
    }
    return true;
}

static bool preflight_models(ESP32S3ElectricalState *s, Esp32S3Project *p,
                             Error **errp)
{
    GString *identity = g_string_new(p->identity);
    for (size_t i = 0; i < p->device_count; ++i) {
        Esp32S3ProjectDevice *model = &p->devices[i];
        unsigned kind = model->model;
        NativeFactory *factory = kind < G_N_ELEMENTS(s->factories) ?
                                 &s->factories[kind] : NULL;
        if (kind >= EN_ADPT_MODEL_I2S_SAMPLE_PEER &&
            (!factory || !factory->preflight)) {
            error_setg(errp, "Registered model '%s' requires its native service factory",
                       p->components[model->component_index].id.text);
            g_string_free(identity, true);
            return false;
        }
        if (!factory || !factory->preflight) {
            continue;
        }
        QDict *component = p->component_documents[model->component_index];
        char *key = NULL;
        if (!factory->preflight(factory->opaque, component, &key, errp)) {
            g_free(key);
            g_string_free(identity, true);
            return false;
        }
        if (!key || !*key || strlen(key) > 4096) {
            error_setg(errp, "Native factory returned an invalid canonical identity");
            g_free(key);
            g_string_free(identity, true);
            return false;
        }
        model->factory_identity = key;
        g_string_append_printf(identity, "factory:%s,%zu:%s;",
                               p->components[model->component_index].id.text,
                               strlen(key), key);
        if (kind == EN_ADPT_MODEL_I2S_SAMPLE_PEER) {
            QDict *attributes = qobject_to(QDict, qdict_get(component, "attributes"));
            QDict *config = attributes ?
                qobject_to(QDict, qdict_get(attributes, "native_i2s_peer")) : NULL;
            model->master_clock_allowed = config &&
                !g_strcmp0(qdict_get_try_str(config, "role"), "master");
        }
    }
    g_free(p->identity);
    p->identity = g_string_free(identity, false);
    return true;
}

static char *get_project(Object *obj, Error **errp)
{
    ESP32S3ElectricalState *s = ESP32S3_ELECTRICAL(obj);
    if (!s->project) {
        return g_strdup("");
    }
    GString *json = qobject_to_json(s->project->document);
    return g_string_free(json, false);
}

static void set_project(Object *obj, const char *json, Error **errp)
{
    ESP32S3ElectricalState *s = ESP32S3_ELECTRICAL(obj);
    if (!s->gpio || !s->rtc || s->settling || s->batch_depth ||
        (!runstate_check(RUN_STATE_PAUSED) && !runstate_check(RUN_STATE_PRELAUNCH))) {
        error_setg(errp, "Electrical Apply requires a paused/initializing quiescent VM boundary");
        return;
    }
    Esp32S3Project *p = esp32s3_project_parse(json, errp);
    if (!p) {
        return;
    }
    if (!rails_declared(p, errp)) {
        esp32s3_project_free(p);
        return;
    }
    if (!preflight_models(s, p, errp)) {
        esp32s3_project_free(p);
        return;
    }
    esp32s3_electrical_models_transfer(s->project, p);
    bool graph_changed = !s->project || strcmp(s->project->identity, p->identity);
    if (!graph_changed && !s->failed) {
        /* ACK display/extension-only edits, preserving the complete accepted
         * document, charge, timers, generation and peripheral state. */
        Esp32S3Project *old = s->project;
        s->project = p;
        esp32s3_project_free(old);
        return;
    }
    EnRcCircuit circuit;
    bool owned[49];
    unsigned pad_nodes[49];
    if (!build_drives(s, p, &circuit, owned, pad_nodes, errp)) {
        esp32s3_project_free(p);
        return;
    }
    EnRcSolver *candidate = NULL, *probe = NULL;
    EnRcSample sample;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (p->rc) {
        EnRcOptions options;
        en_rc_options_default(&options);
        options.abs_error_v = 1e-6;
        options.rel_error = 1e-5;
        options.crossing_tol_s = 1e-9;
        options.crossing_max_bisections = 64;
        EnRcStatus status;
        candidate = en_rc_create(&circuit, &options, &status);
        if (candidate) {
            status = en_rc_reset(candidate, now * 1e-9);
            if (rc_ok(status)) {
                if (!preserve_charge(s, p, candidate, now, graph_changed, errp)) {
                    goto reject;
                }
                status = en_rc_sample(candidate, &sample);
            }
        }
        if (!candidate || !rc_ok(status)) {
            error_setg(errp, "RC candidate graph/charge cannot settle (status %d)", status);
            goto reject;
        }
        probe = en_rc_create(&circuit, &options, &status);
        if (!probe) {
            error_setg(errp, "RC crossing workspace cannot settle (status %d)", status);
            goto reject;
        }
    } else {
        EnDcStatus status = solve_dc(s, &circuit, &sample);
        if (status != EN_DC_OK && status != EN_DC_FLOATING) {
            error_setg(errp, "DC candidate graph cannot settle: %s", sample.diagnostic);
            goto reject;
        }
    }
    /* All validation and candidate settlement precede the commit. */
    timer_del(s->timer);
    qemu_bh_cancel(s->pause_bh);
    en_rc_destroy(s->rc);
    en_rc_destroy(s->probe);
    esp32s3_project_free(s->project);
    s->project = p;
    s->rc = candidate;
    s->probe = probe;
    s->circuit = circuit;
    s->sample = sample;
    s->sample_ns = now;
    s->failed = false;
    s->diagnostic[0] = 0;
    memcpy(s->drive_owned, owned, sizeof(owned));
    memcpy(s->pad_node, pad_nodes, sizeof(pad_nodes));
    if (graph_changed) {
        ++s->generation;
    }
    ++s->source_generation;
    esp32s3_electrical_models_update_power(p, &s->sample, now);
    s->settling = true;
    for (size_t i = 0; i < G_N_ELEMENTS(s->factories); ++i) {
        if (s->factories[i].activate) {
            s->factories[i].activate(s->factories[i].opaque, DEVICE(s));
        }
    }
    s->settling = false;
    settle_sources(s);
    return;
reject:
    en_rc_destroy(candidate);
    en_rc_destroy(probe);
    esp32s3_project_free(p);
}

static void put_voltage(QDict *d, double voltage)
{
    bool valid = isfinite(voltage);
    qdict_put_bool(d, "valid", valid);
    if (valid) {
        qdict_put(d, "voltage_v", qnum_from_double(voltage));
    } else {
        qdict_put_null(d, "voltage_v");
    }
}

static void put_counter(QDict *out, const char *name, uint64_t value)
{
    char text[32];
    snprintf(text, sizeof(text), "%" PRIu64, value);
    qdict_put_str(out, name, text);
}

static char *snapshot(Object *obj, Error **errp)
{
    ESP32S3ElectricalState *s = ESP32S3_ELECTRICAL(obj);
    QDict *out = qdict_new(), *support = qdict_new();
    qdict_put_int(out, "abi", 1);
    qdict_put_bool(support, "dc", true);
    qdict_put_bool(support, "rc", true);
    qdict_put_bool(support, "adc", true);
    qdict_put_str(support, "profile", "s3-explicit-finite-v1");
    qdict_put_bool(support, "awake_sleep_configuration", true);
    qdict_put_bool(support, "active_sleep", false);
    qdict_put_bool(support, "hold", false);
    qdict_put(out, "support", support);
    put_counter(out, "generation", s->generation);
    put_counter(out, "source_generation", s->source_generation);
    put_counter(out, "timestamp_ns", s->sample_ns);
    qdict_put_str(out, "status", !s->project ? "unconfigured" : s->failed ? "failed" :
                  s->sample.status == EN_RC_FLOATING ? "floating" : "settled");
    qdict_put_str(out, "diagnostic", !s->project ? "Apply explicit v3 graph before electrical samples" :
                  s->failed ? s->diagnostic : s->sample.diagnostic);
    qdict_put_int(out, "dc_factorizations", en_dc_workspace_factorizations(s->dc));
    QList *nets = qlist_new(), *pads = qlist_new(), *currents = qlist_new();
    QList *physical_pads = qlist_new();
    if (s->project) {
        Esp32S3Project *p = s->project;
        for (size_t i = 0; i < p->graph.net_count; ++i) {
            QDict *net = qdict_new();
            unsigned node = p->projection.net_node[i];
            qdict_put_str(net, "id", p->nets[i].id.text);
            put_voltage(net, node == UINT_MAX ? NAN : s->sample.voltage[node]);
            qdict_put_bool(net, "floating", node == UINT_MAX || s->sample.floating[node]);
            qlist_append(nets, net);
        }
        double rail = s->sample.voltage[p->projection.terminal_node[p->vdd_terminal]];
        for (unsigned pad = 0; pad < 49; ++pad) {
            if (pad >= 22 && pad <= 25) {
                continue;
            }
            int flat = p->pad_terminal[pad];
            unsigned node = s->pad_node[pad];
            QDict *entry = qdict_new();
            qdict_put_int(entry, "gpio", pad);
            if (flat >= 0) {
                qdict_put_str(entry, "terminal_id", p->terminals[flat].id.text);
            }
            bool floating = node == UINT_MAX || s->sample.floating[node];
            double voltage = node == UINT_MAX ? NAN : s->sample.voltage[node];
            put_voltage(entry, voltage);
            EnPadLevel level = en_dc_classify_pad(voltage, floating, rail, 0.25, 0.75);
            static const char *const names[] = {"low", "high", "indeterminate", "floating", "invalid/unpowered"};
            qdict_put_bool(entry, "digital_valid", !s->failed && (level == EN_PAD_LOW || level == EN_PAD_HIGH));
            qdict_put_bool(entry, "floating", floating);
            qdict_put_str(entry, "diagnostic", s->failed ? s->diagnostic : names[level]);
            ESP32S3GpioDriveSnapshot digital;
            ESP32S3RtcIoDriveSnapshot rtc = {0};
            esp32s3_gpio_get_drive_snapshot(s->gpio, pad, &digital);
            if (pad < 22) {
                esp32s3_rtc_io_get_drive_snapshot(s->rtc, pad, &rtc);
            }
            QDict *native = qdict_new();
            qdict_put_int(native, "mcu_sel", digital.mcu_sel);
            qdict_put_int(native, "out_sel", digital.out_sel);
            qdict_put_bool(native, "rtc_mux", rtc.mux_sel);
            qdict_put_bool(native, "receiver_enabled", rtc.mux_sel ? rtc.fun_ie : digital.ie);
            qdict_put_bool(native, "gpio_output_enable", digital.out_oe);
            qdict_put_bool(native, "rtc_output_enable", rtc.out_oe);
            qdict_put_bool(native, "pull_up", rtc.mux_sel ? rtc.rue : digital.pull_up);
            qdict_put_bool(native, "pull_down", rtc.mux_sel ? rtc.rde : digital.pull_down);
            qdict_put_bool(native, "sleep_configured", rtc.mux_sel ? rtc.slp_sel : digital.slp_sel);
            qdict_put_bool(native, "sleep_requested", esp32s3_gpio_sleep_active(s->gpio));
            qdict_put(entry, "native_controls", native);
            if (flat >= 0) {
                qobject_ref(entry);
                qlist_append(pads, entry);
            }
            qlist_append(physical_pads, entry);
        }
        for (size_t i = 0; i < s->circuit.element_count; ++i) {
            QDict *entry = qdict_new();
            qdict_put_int(entry, "primitive", i);
            qdict_put_str(entry, "component_id", i < p->projection.circuit.element_count ?
                p->components[p->projection.primitive_component[i]].id.text : "native-firmware-drive/pull");
            if (isfinite(s->sample.current[i])) {
                qdict_put(entry, "current_a", qnum_from_double(s->sample.current[i]));
            } else {
                qdict_put_null(entry, "current_a");
            }
            qlist_append(currents, entry);
        }
    }
    qdict_put(out, "nets", nets);
    qdict_put(out, "pads", pads);
    qdict_put(out, "physical_pads", physical_pads);
    qdict_put(out, "currents", currents);
    if (s->project && !s->failed) {
        qdict_put(out, "max_kcl_residual_a", qnum_from_double(s->sample.max_kcl_residual_a));
        qdict_put(out, "max_source_residual_v", qnum_from_double(s->sample.max_source_residual_v));
    } else {
        qdict_put_null(out, "max_kcl_residual_a");
        qdict_put_null(out, "max_source_residual_v");
    }
    GString *json = qobject_to_json(QOBJECT(out));
    char *result = g_string_free(json, false);
    qobject_unref(out);
    return result;
}

static void adc_sample(Esp32S3AdcSampleProvider *provider, unsigned unit,
                       unsigned channel, Esp32S3AdcSample *out)
{
    ESP32S3ElectricalState *s = ESP32S3_ELECTRICAL(provider);
    out->voltage_v = NAN;
    out->validity = ESP32S3_ADC_SAMPLE_UNKNOWN;
    if (!s->project || s->failed || unit > 1 || channel > 9) {
        return;
    }
    /* Solve at the caller-requested aperture instant and echo it back
     * unchanged (Esp32S3AdcSampleProvider contract).  A fresh clock read
     * here drifts past the request under a running virtual clock and would
     * reject genuine acquisitions; only real time-travel (a request older
     * than the last solved frame) stays rejected so the monotonic RC
     * solver is never forced backwards. */
    int64_t requested_ns = (int64_t)out->sample_ns;
    if (requested_ns < s->sample_ns || !advance_now(s, requested_ns)) {
        return;
    }
    unsigned pad = (unit ? 11 : 1) + channel;
    if (s->drive_owned[pad]) {
        out->validity = ESP32S3_ADC_SAMPLE_DIGITAL_OWNED;
        return;
    }
    unsigned node = s->pad_node[pad];
    unsigned rail = s->project->projection.terminal_node[s->project->vdd_terminal];
    if (!isfinite(s->sample.voltage[rail]) || s->sample.voltage[rail] <= 0) {
        out->validity = ESP32S3_ADC_SAMPLE_UNPOWERED;
    } else if (node == UINT_MAX || s->sample.floating[node] || !isfinite(s->sample.voltage[node])) {
        out->validity = ESP32S3_ADC_SAMPLE_FLOATING;
    } else {
        out->voltage_v = s->sample.voltage[node];
        out->validity = ESP32S3_ADC_SAMPLE_VALID;
    }
}

static uint64_t adc_source_generation(Esp32S3AdcSampleProvider *provider,
                                      unsigned unit, unsigned channel)
{
    return ESP32S3_ELECTRICAL(provider)->source_generation;
}

bool esp32s3_electrical_set_matrix_drive(DeviceState *dev, unsigned signal,
                                       bool oe, bool level, bool open_drain)
{
    if (!dev || signal >= 256) {
        return false;
    }
    ESP32S3ElectricalState *s = ESP32S3_ELECTRICAL(dev);
    NativeMatrixDrive *source = &s->matrix[signal];
    if (source->present && source->oe == oe && source->level == level &&
        source->open_drain == open_drain) {
        return !s->failed;
    }
    *source = (NativeMatrixDrive){true, oe, level, open_drain};
    settle_sources(s);
    return !s->failed;
}



bool esp32s3_electrical_line(DeviceState *dev, unsigned gpio, uint64_t sample_ns,
                           double *voltage, bool *valid, bool *level)
{
    if (!voltage || !valid || !level) {
        return false;
    }
    *voltage = NAN;
    *valid = false;
    *level = false;
    if (!dev || gpio >= 49) {
        return false;
    }
    ESP32S3ElectricalState *s = ESP32S3_ELECTRICAL(dev);
    /* Solve at the caller-requested instant (same contract as the ADC
     * aperture): a fresh clock read here drifts under a running virtual
     * clock and would spuriously fail genuine samples; only real
     * time-travel (older than the last solved frame) stays rejected so
     * the monotonic solver is never forced backwards. */
    int64_t requested_ns = (int64_t)sample_ns;
    if (!s->project || s->failed || requested_ns < s->sample_ns ||
        !advance_now(s, requested_ns)) {
        return false;
    }
    unsigned node = s->pad_node[gpio];
    if (node == UINT_MAX) {
        return false;
    }
    unsigned rail = s->project->projection.terminal_node[s->project->vdd_terminal];
    *voltage = s->sample.voltage[node];
    EnPadLevel interpreted = en_dc_classify_pad(*voltage, s->sample.floating[node],
                                                s->sample.voltage[rail], 0.25, 0.75);
    *valid = interpreted == EN_PAD_LOW || interpreted == EN_PAD_HIGH;
    *level = interpreted == EN_PAD_HIGH;
    return isfinite(*voltage);
}




Esp32S3Project *esp32s3_electrical_project(DeviceState *dev)
{
    return dev ? ESP32S3_ELECTRICAL(dev)->project : NULL;
}

uint64_t esp32s3_electrical_generation(DeviceState *dev)
{
    return dev ? ESP32S3_ELECTRICAL(dev)->generation : 0;
}

const EnRcSample *esp32s3_electrical_sample_at(DeviceState *dev, uint64_t ns)
{
    if (!dev) {
        return NULL;
    }
    ESP32S3ElectricalState *s = ESP32S3_ELECTRICAL(dev);
    /* Solve at the caller-requested instant (same contract as the ADC
     * aperture): under a running virtual clock a fresh read here drifts
     * past the request and would spuriously fail genuine consumers;
     * only real time-travel (older than the last solved frame) stays
     * rejected so the monotonic solver is never forced backwards.  The
     * batch-dirty and notify guards are unchanged. */
    if (!s->project || s->failed || (int64_t)ns < s->sample_ns ||
        (s->batch_depth && s->dirty && !s->notifying)) {
        return NULL;
    }
    if (!s->notifying && !advance_now(s, ns)) {
        return NULL;
    }
    return &s->sample;
}

bool esp32s3_electrical_model_changed(DeviceState *dev)
{
    if (!dev) {
        return false;
    }
    ESP32S3ElectricalState *s = ESP32S3_ELECTRICAL(dev);
    settle_sources(s);
    return !s->failed;
}

bool esp32s3_electrical_add_model_notify(DeviceState *dev,
                                       void (*notify)(void *), void *opaque)
{
    return dev && esp32s3_electrical_observers_add_model_notify(
        &ESP32S3_ELECTRICAL(dev)->observers, notify, opaque);
}

void esp32s3_electrical_remove_model_notify(DeviceState *dev,
                                          void (*notify)(void *), void *opaque)
{
    if (dev) {
        esp32s3_electrical_observers_remove_model_notify(
            &ESP32S3_ELECTRICAL(dev)->observers, notify, opaque);
    }
}

bool esp32s3_electrical_subscribe(DeviceState *dev,
                                 void (*notify)(void *, uint64_t), void *opaque)
{
    return dev && esp32s3_electrical_observers_subscribe(
        &ESP32S3_ELECTRICAL(dev)->observers, notify, opaque);
}

void esp32s3_electrical_unsubscribe(DeviceState *dev,
                                   void (*notify)(void *, uint64_t), void *opaque)
{
    if (dev) {
        esp32s3_electrical_observers_unsubscribe(
            &ESP32S3_ELECTRICAL(dev)->observers, notify, opaque);
    }
}

bool esp32s3_electrical_begin_update(DeviceState *dev)
{
    if (!dev) {
        return false;
    }
    ESP32S3ElectricalState *s = ESP32S3_ELECTRICAL(dev);
    if (s->batch_depth == 32) {
        return false;
    }
    ++s->batch_depth;
    return true;
}

bool esp32s3_electrical_end_update(DeviceState *dev)
{
    if (!dev) {
        return false;
    }
    ESP32S3ElectricalState *s = ESP32S3_ELECTRICAL(dev);
    if (!s->batch_depth) {
        return false;
    }
    --s->batch_depth;
    if (!s->batch_depth && s->dirty) {
        settle_sources(s);
    }
    return !s->failed;
}

bool esp32s3_electrical_register_factory(
    DeviceState *dev, ESP32S3ElectricalModel model,
    ESP32S3ElectricalPreflight preflight,
    ESP32S3ElectricalActivate activate, void *opaque)
{
    if (!dev || !preflight || !activate || (unsigned)model >= 10) {
        return false;
    }
    ESP32S3ElectricalState *s = ESP32S3_ELECTRICAL(dev);
    NativeFactory *slot = &s->factories[model];
    if (slot->preflight) {
        return slot->preflight == preflight && slot->activate == activate &&
               slot->opaque == opaque;
    }
    *slot = (NativeFactory){preflight, activate, opaque};
    return true;
}

void esp32s3_electrical_unregister_factory(
    DeviceState *dev, ESP32S3ElectricalModel model, void *opaque)
{
    if (dev && (unsigned)model < 10) {
        NativeFactory *slot = &ESP32S3_ELECTRICAL(dev)->factories[model];
        if (slot->opaque == opaque) {
            memset(slot, 0, sizeof(*slot));
        }
    }
}

const QDict *esp32s3_electrical_model_component(
    DeviceState *dev, const char *component_id, const char **factory_key)
{
    if (factory_key) {
        *factory_key = NULL;
    }
    Esp32S3Project *p = esp32s3_electrical_project(dev);
    if (!p || !component_id) {
        return NULL;
    }
    for (size_t i = 0; i < p->device_count; ++i) {
        Esp32S3ProjectDevice *model = &p->devices[i];
        if (!strcmp(component_id, p->components[model->component_index].id.text)) {
            if (factory_key) {
                *factory_key = model->factory_identity;
            }
            return p->component_documents[model->component_index];
        }
    }
    return NULL;
}

static void instance_init(Object *obj)
{
    ESP32S3ElectricalState *s = ESP32S3_ELECTRICAL(obj);
    s->dc = en_dc_workspace_create();
    if (!s->dc) {
        error_report("Cannot allocate bounded electrical workspace");
        exit(1);
    }
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, timer_sample, s);
    s->pause_bh = qemu_bh_new(pause_dependency, s);
    object_property_add_str(obj, "project-json", get_project, set_project);
    object_property_add_str(obj, "snapshot-json", snapshot, NULL);
}

static void instance_finalize(Object *obj)
{
    ESP32S3ElectricalState *s = ESP32S3_ELECTRICAL(obj);
    if (s->gpio) {
        esp32s3_gpio_set_drive_observer(s->gpio, NULL, NULL);
        esp32s3_rtc_io_set_drive_observer(s->rtc, NULL, NULL);
    }
    timer_free(s->timer);
    qemu_bh_delete(s->pause_bh);
    en_dc_workspace_destroy(s->dc);
    en_rc_destroy(s->rc);
    en_rc_destroy(s->probe);
    esp32s3_project_free(s->project);
}

static void class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    Esp32S3AdcSampleProviderClass *provider = ESP32S3_ADC_SAMPLE_PROVIDER_CLASS(
        object_class_dynamic_cast(klass, TYPE_ESP32S3_ADC_SAMPLE_PROVIDER));
    provider->sample = adc_sample;
    provider->source_generation = adc_source_generation;
    dc->user_creatable = false;
    dc->desc = "Authoritative v3 electrical graph with explicit finite S3 drive profile";
}

static const TypeInfo electrical_info = {
    .name = TYPE_ESP32S3_ELECTRICAL,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(ESP32S3ElectricalState),
    .instance_init = instance_init,
    .instance_finalize = instance_finalize,
    .class_init = class_init,
    .interfaces = (InterfaceInfo[]){{TYPE_ESP32S3_ADC_SAMPLE_PROVIDER}, {}}
};
static void register_types(void)
{
    type_register_static(&electrical_info);
}
type_init(register_types)

DeviceState *esp32s3_electrical_create(Object *soc, ESP32S3GPIOState *gpio,
                                      ESP32S3RtcIoState *rtc, ESP32S3SensState *sens)
{
    DeviceState *dev = qdev_new(TYPE_ESP32S3_ELECTRICAL);
    ESP32S3ElectricalState *s = ESP32S3_ELECTRICAL(dev);
    object_property_add_child(soc, "electrical", OBJECT(dev));
    s->gpio = gpio;
    s->rtc = rtc;
    qdev_realize(dev, NULL, &error_fatal);
    esp32s3_gpio_set_drive_observer(gpio, drive_changed, s);
    esp32s3_rtc_io_set_drive_observer(rtc, drive_changed, s);
    if (!object_property_get_link(OBJECT(sens), "sample-provider", &error_fatal)) {
        object_property_set_link(OBJECT(sens), "sample-provider", OBJECT(dev), &error_fatal);
    }
    object_unref(OBJECT(dev));
    return dev;
}
