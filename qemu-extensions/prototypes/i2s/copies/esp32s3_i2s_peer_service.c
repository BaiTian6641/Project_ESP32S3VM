/* SPDX-License-Identifier: GPL-2.0-or-later */
/* External digital peers use only committed registered models and resolved
 * terminals. JSON allocation is confined to Apply and explicit QOM queries. */
#include "qemu/osdep.h"
#include "hw/misc/esp32s3_i2s_peer_service.h"
#include "hw/misc/esp32s3_i2s_peer.h"
#include "hw/misc/esp32s3_i2s.h"
#include "hw/misc/esp32s3_electrical.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qapi/qmp/qjson.h"
#include "qapi/qmp/qlist.h"
#include "qapi/qmp/qnum.h"
#include "qapi/qmp/qstring.h"
#include "sysemu/runstate.h"
#include <math.h>

#define TYPE_I2S_PEER_SERVICE "esp32s3-i2s-peer-service"
/* Matches the authoritative project's total registered-model bound. */
#define PEER_LIMIT 32
#define CAPTURE_WINDOW_LIMIT 1024

typedef struct I2sPeerService I2sPeerService;
typedef struct I2sPeerSlot {
    I2sPeerService *service;
    char id[65];
    char identity[65]; /* Identity of this committed instance, never proposed. */
    S3I2sPeer *peer;
    bool started;
    S3I2sPeerNotify notify;
    void *notify_opaque;
    char *activation_error;
} I2sPeerSlot;

struct I2sPeerService {
    Object parent;
    DeviceState *electrical;
    Object *soc; /* Borrowed owner; never inferred from a peer's geometry. */
    I2sPeerSlot slots[PEER_LIMIT];
    bool registered, frame_subscribed, model_subscribed, activating, closing;
    bool request_valid;
    char request_id[65];
    uint64_t request_offset, request_count;
    bool request_transitions;
    uint32_t request_controller;
    uint64_t request_peer_offset, request_controller_offset;
    uint64_t request_transition_count;
};

static const char *const terminal_roles[] = {
    [S3_I2S_PEER_VDD] = "vdd", [S3_I2S_PEER_GND] = "gnd",
    [S3_I2S_PEER_BCLK] = "bclk", [S3_I2S_PEER_WS] = "ws",
    [S3_I2S_PEER_DIN] = "din", [S3_I2S_PEER_DOUT] = "dout",
};

static bool provider_power(void *opaque, S3I2sPeerPower *power)
{
    I2sPeerSlot *slot = opaque;
    ESP32S3ElectricalEndpoint endpoint;
    ESP32S3ElectricalRoute route = esp32s3_electrical_model_endpoint(
        slot->service->electrical, slot->id,
        qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), &endpoint);
    if (route == ESP32S3_ELECTRICAL_ROUTE_NONE ||
        endpoint.model != ESP32S3_ELECTRICAL_I2S_SAMPLE_PEER) {
        return false;
    }
    *power = (S3I2sPeerPower) {
        .known = endpoint.power_known,
        .powered = endpoint.powered,
        .vdd = endpoint.vdd_v, .gnd = endpoint.gnd_v,
        .power_epoch = endpoint.power_epoch,
        .generation = endpoint.generation,
        .power_on_ns = endpoint.power_on_ns,
    };
    return true;
}

static bool provider_sample(void *opaque, S3I2sPeerTerminal terminal,
                            int64_t ns, bool *valid, bool *level)
{
    I2sPeerSlot *slot = opaque;
    double voltage;
    if ((unsigned)terminal >= G_N_ELEMENTS(terminal_roles) || ns < 0) {
        return false;
    }
    return esp32s3_electrical_terminal_sample(slot->service->electrical,
        slot->id, terminal_roles[terminal], ns, &voltage, valid, level);
}

static bool provider_drive(void *opaque, S3I2sPeerTerminal terminal,
                           bool oe, bool level)
{
    I2sPeerSlot *slot = opaque;
    if ((unsigned)terminal >= G_N_ELEMENTS(terminal_roles)) {
        return false;
    }
    return esp32s3_electrical_terminal_drive(slot->service->electrical,
        slot->id, terminal_roles[terminal], oe, level);
}

static bool provider_drive_frame(void *opaque, bool bclk_oe, bool bclk,
                                 bool ws_oe, bool ws, bool dout_oe, bool dout)
{
    I2sPeerSlot *slot = opaque;
    DeviceState *dev = slot->service->electrical;
    if (!esp32s3_electrical_begin_update(dev)) {
        return false;
    }
    /* Do not short-circuit: always close the batch and publish all roles in
     * one next delta. NativeNet retains its consumed observer frame. */
    bool ok = esp32s3_electrical_terminal_drive(dev, slot->id, "bclk", bclk_oe, bclk);
    ok &= esp32s3_electrical_terminal_drive(dev, slot->id, "ws", ws_oe, ws);
    ok &= esp32s3_electrical_terminal_drive(dev, slot->id, "dout", dout_oe, dout);
    ok &= esp32s3_electrical_end_update(dev);
    return ok;
}

static bool provider_subscribe(void *opaque, S3I2sPeerNotify notify,
                               void *notify_opaque)
{
    I2sPeerSlot *slot = opaque;
    if (!notify || slot->notify || slot->service->closing ||
        !slot->service->frame_subscribed || !slot->service->model_subscribed) {
        return false;
    }
    slot->notify = notify;
    slot->notify_opaque = notify_opaque;
    return true;
}

static void provider_unsubscribe(void *opaque, S3I2sPeerNotify notify,
                                 void *notify_opaque)
{
    I2sPeerSlot *slot = opaque;
    if (slot->notify == notify && slot->notify_opaque == notify_opaque) {
        slot->notify = NULL;
        slot->notify_opaque = NULL;
    }
}

static void provider_dependency(void *opaque, const char *reason, int64_t ns)
{
    I2sPeerSlot *slot = opaque;
    qemu_log_mask(LOG_GUEST_ERROR,
                  "I2S peer '%s' strict dependency: %s at %" PRId64 " ns\n",
                  slot->id, reason, ns);
    vm_stop(RUN_STATE_PAUSED);
}

static void slot_clear(I2sPeerSlot *slot)
{
    /* Cancel fanout before freeing timers or issuing any release drive. */
    slot->notify = NULL;
    slot->notify_opaque = NULL;
    S3I2sPeer *peer = slot->peer;
    slot->peer = NULL;
    slot->started = false;
    if (slot->service->request_valid &&
        !strcmp(slot->service->request_id, slot->id)) {
        slot->service->request_valid = false;
    }
    esp32s3_i2s_peer_free(peer);
    g_free(slot->activation_error);
    slot->activation_error = NULL;
    slot->id[0] = 0;
    slot->identity[0] = 0;
}

static I2sPeerSlot *find_slot(I2sPeerService *service, const char *id)
{
    for (unsigned i = 0; i < PEER_LIMIT; ++i) {
        if (service->slots[i].id[0] && !strcmp(service->slots[i].id, id)) {
            return &service->slots[i];
        }
    }
    return NULL;
}

static void physical_frame(void *opaque, uint64_t ns)
{
    I2sPeerService *service = opaque;
    if (service->activating || service->closing) {
        return;
    }
    for (unsigned i = 0; i < PEER_LIMIT; ++i) {
        I2sPeerSlot *slot = &service->slots[i];
        if (!slot->id[0]) {
            continue;
        }
        ESP32S3ElectricalEndpoint endpoint;
        if (esp32s3_electrical_model_endpoint(service->electrical, slot->id,
                ns, &endpoint) == ESP32S3_ELECTRICAL_ROUTE_NONE ||
            endpoint.model != ESP32S3_ELECTRICAL_I2S_SAMPLE_PEER) {
            /* Removal is not an unknown-input event. Never feed a removed
             * instance into the consumer or synthesize a power-off sample. */
            slot_clear(slot);
        } else if (slot->peer && !slot->started) {
            Error *error = NULL;
            /* Start on the first committed solved notification, not inside
             * the Apply batch where terminal sampling is unavailable. New
             * source drives become the following immutable solve delta.
             * start false is an Error-bearing setup failure only; a genuine
             * runtime electrical pause is a started peer with status.dependency,
             * never activation_error. The engine already paused/logged it. */
            slot->started = true;
            if (!esp32s3_i2s_peer_start(slot->peer, &error)) {
                slot->activation_error = g_strdup(error_get_pretty(error));
                error_free(error);
                vm_stop(RUN_STATE_PAUSED);
            }
        } else if (slot->notify) {
            slot->notify(slot->notify_opaque, ns);
        }
    }
}

static void model_wake(void *opaque)
{
    physical_frame(opaque, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
}

static bool preflight(void *opaque, const QDict *component,
                      char **identity, Error **errp)
{
    S3I2sPeerConfig config = {0};
    if (!esp32s3_i2s_peer_parse(component, &config, errp)) {
        return false;
    }
    /* Exercise the actual bounded capture allocation before commit. Source
     * storage was already allocated and validated by the strict parser.
     * No instance, timer, proposed-config cache or live drive is created. */
    void *capture = g_try_malloc_n(config.capture_capacity,
                                  sizeof(S3I2sPeerCapture));
    void *transitions = g_try_malloc_n(2 * config.capture_capacity + 16,
                                      sizeof(S3I2sPeerTransition));
    bool allocated = capture && transitions;
    g_free(capture);
    g_free(transitions);
    if (!allocated) {
        error_setg(errp, "i2s-sample-peer: bounded capture/transition resources unavailable before Apply");
        esp32s3_i2s_peer_config_clear(&config);
        return false;
    }
    *identity = g_strdup(config.identity);
    esp32s3_i2s_peer_config_clear(&config);
    return true;
}
static void construct_peer(I2sPeerSlot *slot, const QDict *component,
                           const char *identity)
{
    S3I2sPeerConfig config = {0};
    Error *error = NULL;
    S3I2sPeerProvider provider = {
        .opaque = slot,
        .power = provider_power,
        .sample = provider_sample,
        .drive = provider_drive,
        .drive_frame = provider_drive_frame,
        .subscribe = provider_subscribe,
        .unsubscribe = provider_unsubscribe,
        .dependency = provider_dependency,
    };
    if (identity) {
        g_strlcpy(slot->identity, identity, sizeof(slot->identity));
    }
    if (!component || !identity || !*identity) {
        error_setg(&error, "committed I2S component metadata unavailable");
    } else if (esp32s3_i2s_peer_parse(component, &config, &error)) {
        if (strcmp(config.identity, identity)) {
            error_setg(&error, "committed I2S canonical identity disagrees with its actual configuration");
        } else {
            slot->peer = esp32s3_i2s_peer_new(&config, &provider, &error);
        }
    }
    esp32s3_i2s_peer_config_clear(&config);
    if (error) {
        slot->activation_error = g_strdup(error_get_pretty(error));
        error_free(error);
        provider_dependency(slot, slot->activation_error,
                            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    }
}


static QDict *peer_transition_summary(const S3I2sPeerTransitionWindow *w)
{
    QDict *out = qdict_new();
#define U(field) qdict_put(out, #field, qnum_from_uint(w->field))
    U(version); U(generation); U(epoch); U(capacity); U(count);
    U(first_index); U(first_sequence); U(total); U(lost); U(power_on_ns);
#undef U
    qdict_put_int(out, "config_activation_ns", w->config_activation_ns);
    qdict_put_int(out, "activation_ns", w->activation_ns);
    return out;
}

static QDict *controller_transition_summary(const S3I2sTxTransitionWindow *w)
{
    QDict *out = qdict_new();
#define U(field) qdict_put(out, #field, qnum_from_uint(w->field))
    U(version); U(controller); U(epoch); U(capacity); U(count);
    U(first_index); U(first_sequence); U(total); U(lost);
#undef U
    qdict_put_bool(out, "active", w->active);
    qdict_put_int(out, "activation_ns", w->activation_ns);
    return out;
}

static QDict *status_dict(I2sPeerSlot *slot)
{
    QDict *out = qdict_new();
    ESP32S3ElectricalEndpoint endpoint = {0};
    esp32s3_electrical_model_endpoint(slot->service->electrical, slot->id,
        qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), &endpoint);
    qdict_put_str(out, "component_id", slot->id);
    qdict_put_bool(out, "power_known", endpoint.power_known);
    qdict_put_bool(out, "registered_powered", endpoint.powered);
    qdict_put(out, "generation", qnum_from_uint(endpoint.generation));
    qdict_put(out, "power_on_ns", qnum_from_uint(endpoint.power_on_ns));
    qdict_put(out, "registered_power_epoch", qnum_from_uint(endpoint.power_epoch));
    if (isfinite(endpoint.vdd_v) && endpoint.power_known) {
        qdict_put(out, "vdd_v", qnum_from_double(endpoint.vdd_v));
    } else {
        qdict_put_null(out, "vdd_v");
    }
    if (isfinite(endpoint.gnd_v) && endpoint.power_known) {
        qdict_put(out, "gnd_v", qnum_from_double(endpoint.gnd_v));
    } else {
        qdict_put_null(out, "gnd_v");
    }
    if (slot->activation_error) {
        qdict_put_str(out, "activation_error", slot->activation_error);
    } else {
        qdict_put_null(out, "activation_error");
    }
    if (slot->peer) {
        S3I2sPeerStatus status;
        esp32s3_i2s_peer_status(slot->peer, &status);
        qdict_put_str(out, "config_identity", status.config_identity);
        qdict_put_bool(out, "powered", status.powered);
        qdict_put_bool(out, "paused", status.paused);
        qdict_put_bool(out, "synchronized", status.synchronized);
        qdict_put_bool(out, "exhausted", status.exhausted);
        qdict_put_bool(out, "capture_overflow", status.capture_overflow);
        qdict_put(out, "power_epoch", qnum_from_uint(status.power_epoch));
        qdict_put(out, "transmitted", qnum_from_uint(status.transmitted));
        qdict_put(out, "captured", qnum_from_uint(status.captured));
        qdict_put(out, "capture_count", qnum_from_uint(status.capture_count));
        S3I2sPeerTransitionWindow transitions;
        esp32s3_i2s_peer_transition_window(slot->peer, &transitions);
        qdict_put(out, "transitions", peer_transition_summary(&transitions));
        if (status.dependency) {
            qdict_put_str(out, "dependency", status.dependency);
        } else {
            qdict_put_null(out, "dependency");
        }
    } else if (slot->identity[0]) {
        qdict_put_str(out, "config_identity", slot->identity);
    }
    return out;
}

static char *serialize(QObject *object)
{
    GString *json = qobject_to_json(object);
    qobject_unref(object);
    return g_string_free(json, false);
}

static char *get_status(Object *obj, Error **errp)
{
    I2sPeerService *service = (I2sPeerService *)obj;
    QDict *out = qdict_new();
    QList *peers = qlist_new();
    qdict_put_int(out, "version", 1);
    qdict_put_str(out, "kind", "i2s-peer-status");
    for (unsigned i = 0; i < PEER_LIMIT; ++i) {
        if (service->slots[i].id[0]) {
            qlist_append(peers, status_dict(&service->slots[i]));
        }
    }
    qdict_put(out, "peers", peers);
    QList *controllers = qlist_new();
    for (unsigned i = 0; i < 2; ++i) {
        Object *controller = object_resolve_path_component(
            service->soc, i == 0 ? "i2s0" : "i2s1");
        if (!controller || !object_dynamic_cast(controller, TYPE_ESP32S3_I2S)) {
            error_setg(errp, "I2S status has no actual controller child i2s%u", i);
            qobject_unref(controllers);
            qobject_unref(out);
            return NULL;
        }
        S3I2sTxTransitionWindow window;
        ESP32S3I2sState *state = ESP32S3_I2S(controller);
        QDict *summary;
        bool enabled = esp32s3_i2s_tx_transition_window(state, &window);
        if (enabled) {
            summary = controller_transition_summary(&window);
        } else {
            summary = qdict_new();
            qdict_put(summary, "controller", qnum_from_uint(state->controller));
        }
        qdict_put_bool(summary, "capture_enabled", enabled);
        qlist_append(controllers, summary);
    }
    qdict_put(out, "controllers", controllers);
    return serialize(QOBJECT(out));
}

static bool request_uint(QDict *request, const char *field, uint64_t *value,
                         Error **errp)
{
    QNum *number = qobject_to(QNum, qdict_get(request, field));
    if (!number || !qnum_get_try_uint(number, value)) {
        error_setg(errp, "I2S capture request '%s' requires an unsigned JSON integer", field);
        return false;
    }
    return true;
}

static bool controller_window(I2sPeerService *service, uint32_t controller,
                              S3I2sTxTransitionWindow *window, Error **errp)
{
    const char *child = controller == 0 ? "i2s0" : "i2s1";
    Object *obj = object_resolve_path_component(service->soc, child);
    if (!obj || !object_dynamic_cast(obj, TYPE_ESP32S3_I2S)) {
        error_setg(errp, "I2S transition scope '%s' has no actual controller", child);
        return false;
    }
    if (!esp32s3_i2s_tx_transition_window(ESP32S3_I2S(obj), window)) {
        error_setg(errp, "I2S controller '%s' transition capture is not enabled", child);
        return false;
    }
    if (window->controller != controller) {
        error_setg(errp, "I2S requested controller scope disagrees with actual C state");
        return false;
    }
    return true;
}

static bool transition_offset(uint64_t offset, uint64_t first,
                              uint64_t total, const char *scope, Error **errp)
{
    if (offset < first || offset > total) {
        error_setg(errp, "I2S %s transition offset %" PRIu64
                   " outside retained [%" PRIu64 ",%" PRIu64
                   "]; lost=%" PRIu64, scope, offset, first, total, first);
        return false;
    }
    return true;
}

static QDict *peer_transition_record(const S3I2sPeerTransition *r)
{
    QDict *out = qdict_new();
#define U(field) qdict_put(out, #field, qnum_from_uint(r->field))
#define I(field) qdict_put_int(out, #field, r->field)
    U(sequence); U(epoch); U(power_epoch); U(generation);
    U(source_id); U(source_cursor); U(word_ordinal); U(frame);
    U(first_halfphase); U(last_halfphase);
    I(ns); I(first_ns); I(last_ns); I(first_boundary_ns); I(last_boundary_ns);
    U(kind); U(source); U(slot); U(raw_channel); U(physical_phase);
    U(bit_phase); U(power_on_ns);
    U(valid_bits); U(slot_bits); U(published_bits);
#undef I
#undef U
    qdict_put_bool(out, "raw", r->raw);
    qdict_put_bool(out, "power_known", r->power_known);
    qdict_put_bool(out, "powered", r->powered);
    return out;
}

static QDict *controller_transition_record(const S3I2sTxTransition *r)
{
    QDict *out = qdict_new();
#define U(field) qdict_put(out, #field, qnum_from_uint(r->field))
#define I(field) qdict_put_int(out, #field, r->field)
    U(sequence); U(epoch); U(source_word_id); U(word_ordinal); U(frame);
    U(first_halfphase); U(last_halfphase);
    I(ns); I(first_ns); I(last_ns); I(first_boundary_ns); I(last_boundary_ns);
    U(kind); U(reason); U(flags); U(source); U(slot); U(raw_channel);
    U(physical_phase); U(valid_bits); U(slot_bits); U(shifted_bits);
#undef I
#undef U
    return out;
}

static QDict *peer_transition_export(const S3I2sPeerTransitionWindow *w,
                                    uint64_t offset, uint64_t count)
{
    QDict *out = peer_transition_summary(w);
    QList *records = qlist_new();
    qdict_put(out, "offset", qnum_from_uint(offset));
    uint64_t end = offset + MIN(count, w->total - offset);
    for (uint64_t sequence = offset; sequence < end; ++sequence) {
        size_t index = (w->first_index + sequence - w->first_sequence) %
                       w->capacity;
        qlist_append(records, peer_transition_record(&w->records[index]));
    }
    qdict_put(out, "records", records);
    return out;
}

static QDict *controller_transition_export(const S3I2sTxTransitionWindow *w,
                                          uint64_t offset, uint64_t count)
{
    QDict *out = controller_transition_summary(w);
    QList *records = qlist_new();
    qdict_put(out, "offset", qnum_from_uint(offset));
    uint64_t end = offset + MIN(count, w->total - offset);
    for (uint64_t sequence = offset; sequence < end; ++sequence) {
        size_t index = (w->first_index + sequence - w->first_sequence) %
                       w->capacity;
        qlist_append(records, controller_transition_record(&w->records[index]));
    }
    qdict_put(out, "records", records);
    return out;
}

static void set_capture_request(Object *obj, const char *value, Error **errp)
{
    I2sPeerService *service = (I2sPeerService *)obj;
    QObject *parsed = qobject_from_json(value, errp);
    if (!parsed) {
        return;
    }
    QDict *request = qobject_to(QDict, parsed);
    uint64_t offset, count;
    uint64_t controller = 0, peer_offset = 0, controller_offset = 0;
    uint64_t transition_count = 0;
    bool with_transitions = request && qdict_haskey(request, "transitions");
    if (!request || qdict_size(request) != (with_transitions ? 4 : 3) ||
        !qdict_haskey(request, "componentId") ||
        !qdict_haskey(request, "offset") || !qdict_haskey(request, "count")) {
        error_setg(errp, "I2S capture request requires componentId, offset, count and optional transitions");
        goto out;
    }
    QString *id_object = qobject_to(QString, qdict_get(request, "componentId"));
    const char *id = id_object ? qstring_get_str(id_object) : NULL;
    if (!id || !*id || strlen(id) >= sizeof(service->request_id)) {
        error_setg(errp, "I2S capture request componentId requires a 1..64 byte string");
        goto out;
    }
    if (!request_uint(request, "offset", &offset, errp) ||
        !request_uint(request, "count", &count, errp)) {
        goto out;
    }
    if (!count || count > CAPTURE_WINDOW_LIMIT) {
        error_setg(errp, "I2S capture request count requires 1..1024");
        goto out;
    }
    I2sPeerSlot *slot = find_slot(service, id);
    size_t total;
    if (!slot || !slot->peer) {
        error_setg(errp, "I2S capture request component '%s' has no actual peer", id);
        goto out;
    }
    esp32s3_i2s_peer_capture(slot->peer, &total);
    if (offset > total) {
        error_setg(errp, "I2S capture request offset exceeds actual DIN capture count");
        goto out;
    }
    if (with_transitions) {
        QDict *t = qobject_to(QDict, qdict_get(request, "transitions"));
        if (!t || qdict_size(t) != 4 ||
            !request_uint(t, "controllerId", &controller, errp) ||
            !request_uint(t, "peerOffset", &peer_offset, errp) ||
            !request_uint(t, "controllerOffset", &controller_offset, errp) ||
            !request_uint(t, "count", &transition_count, errp)) {
            if (t && qdict_size(t) == 4) {
                goto out;
            }
            error_setg(errp, "I2S transitions requires exactly controllerId, peerOffset, controllerOffset, count");
            goto out;
        }
        if (controller > 1 || !transition_count ||
            transition_count > CAPTURE_WINDOW_LIMIT) {
            error_setg(errp, "I2S transitions requires controllerId 0..1 and count 1..1024");
            goto out;
        }
        S3I2sPeerTransitionWindow peer;
        S3I2sTxTransitionWindow core;
        esp32s3_i2s_peer_transition_window(slot->peer, &peer);
        if (!controller_window(service, controller, &core, errp) ||
            !transition_offset(peer_offset, peer.first_sequence, peer.total,
                               "peer", errp) ||
            !transition_offset(controller_offset, core.first_sequence,
                               core.total, "controller", errp)) {
            goto out;
        }
    }
    g_strlcpy(service->request_id, id, sizeof(service->request_id));
    service->request_offset = offset;
    service->request_count = count;
    service->request_transitions = with_transitions;
    service->request_controller = controller;
    service->request_peer_offset = peer_offset;
    service->request_controller_offset = controller_offset;
    service->request_transition_count = transition_count;
    service->request_valid = true;
 out:
    qobject_unref(parsed);
}

static char *get_capture(Object *obj, Error **errp)
{
    I2sPeerService *service = (I2sPeerService *)obj;
    I2sPeerSlot *slot = service->request_valid ?
                        find_slot(service, service->request_id) : NULL;
    if (!slot || !slot->peer) {
        error_setg(errp, "Set capture-request-json for an actual peer before reading capture-json");
        return NULL;
    }
    size_t total;
    const S3I2sPeerCapture *capture = esp32s3_i2s_peer_capture(slot->peer, &total);
    if (service->request_offset > total) {
        error_setg(errp, "I2S capture request offset exceeds the current peer's actual capture count");
        return NULL;
    }
    S3I2sPeerTransitionWindow peer;
    S3I2sTxTransitionWindow core;
    if (service->request_transitions) {
        esp32s3_i2s_peer_transition_window(slot->peer, &peer);
        if (!controller_window(service, service->request_controller,
                               &core, errp) ||
            !transition_offset(service->request_peer_offset,
                               peer.first_sequence, peer.total, "peer", errp) ||
            !transition_offset(service->request_controller_offset,
                               core.first_sequence, core.total,
                               "controller", errp)) {
            return NULL;
        }
    }
    S3I2sPeerStatus status;
    esp32s3_i2s_peer_status(slot->peer, &status);
    QDict *out = qdict_new();
    QList *events = qlist_new();
    qdict_put_int(out, "version", 1);
    qdict_put_str(out, "kind", "i2s-peer-din-capture");
    qdict_put_str(out, "component_id", slot->id);
    qdict_put_str(out, "config_identity", status.config_identity);
    qdict_put(out, "offset", qnum_from_uint(service->request_offset));
    qdict_put(out, "total", qnum_from_uint(total));
    size_t end = service->request_offset +
        MIN(service->request_count, total - service->request_offset);
    for (size_t i = service->request_offset; i < end; ++i) {
        const S3I2sPeerCapture *record = &capture[i];
        QDict *event = qdict_new();
        qdict_put_int(event, "ns", record->ns);
        qdict_put(event, "sequence", qnum_from_uint(record->sequence));
        qdict_put(event, "sample", qnum_from_uint(record->sample));
        qdict_put(event, "slot", qnum_from_uint(record->slot));
        qdict_put_bool(event, "raw", record->raw);
        qlist_append(events, event);
    }
    qdict_put(out, "events", events);
    qdict_put(out, "status", status_dict(slot));
    if (service->request_transitions) {
        QDict *transitions = qdict_new();
        qdict_put_int(transitions, "version", 1);
        qdict_put(transitions, "peer", peer_transition_export(
            &peer, service->request_peer_offset,
            service->request_transition_count));
        qdict_put(transitions, "controller", controller_transition_export(
            &core, service->request_controller_offset,
            service->request_transition_count));
        qdict_put(out, "transitions", transitions);
    }
    return serialize(QOBJECT(out));
}

static void instance_init(Object *obj)
{
    I2sPeerService *service = (I2sPeerService *)obj;
    for (unsigned i = 0; i < PEER_LIMIT; ++i) {
        service->slots[i].service = service;
    }
    object_property_add_str(obj, "capture-request-json", NULL, set_capture_request);
    object_property_add_str(obj, "capture-json", get_capture, NULL);
    object_property_add_str(obj, "status-json", get_status, NULL);
}

static void instance_finalize(Object *obj)
{
    I2sPeerService *service = (I2sPeerService *)obj;
    service->closing = true;
    if (!service->electrical) {
        return;
    }
    if (service->registered) {
        esp32s3_electrical_unregister_factory(service->electrical,
            ESP32S3_ELECTRICAL_I2S_SAMPLE_PEER, service);
    }
    if (service->frame_subscribed) {
        esp32s3_electrical_unsubscribe(service->electrical, physical_frame, service);
    }
    if (service->model_subscribed) {
        esp32s3_electrical_remove_model_notify(service->electrical, model_wake, service);
    }
    for (unsigned i = 0; i < PEER_LIMIT; ++i) {
        slot_clear(&service->slots[i]);
    }
    object_unref(OBJECT(service->electrical));
}

static const TypeInfo service_type = {
    .name = TYPE_I2S_PEER_SERVICE,
    .parent = TYPE_OBJECT,
    .instance_size = sizeof(I2sPeerService),
    .instance_init = instance_init,
    .instance_finalize = instance_finalize,
};

static void register_types(void)
{
    type_register_static(&service_type);
}

type_init(register_types)

static void activate(void *opaque, DeviceState *electrical)
{
    I2sPeerService *service = opaque;
    uint64_t ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    service->activating = true;
    if (!esp32s3_electrical_begin_update(electrical)) {
        /* Stop timers and detach consumers even if the actual provider can
         * no longer accept output releases. Never run old configuration on
         * a newly committed graph after an activation failure. */
        for (unsigned i = 0; i < PEER_LIMIT; ++i) {
            slot_clear(&service->slots[i]);
        }
        error_report("I2S peer activation: native atomic update unavailable");
        vm_stop(RUN_STATE_PAUSED);
        service->activating = false;
        return;
    }
    /* Reclaim removed and changed identities before allocating replacements:
     * a valid Apply can replace all 32 registered models at once. Neither
     * the document nor its borrowed key escapes this activation callback. */
    for (unsigned i = 0; i < PEER_LIMIT; ++i) {
        I2sPeerSlot *slot = &service->slots[i];
        if (!slot->id[0]) {
            continue;
        }
        const char *key = NULL;
        const QDict *component = esp32s3_electrical_model_component(
            electrical, slot->id, &key);
        ESP32S3ElectricalEndpoint endpoint;
        ESP32S3ElectricalRoute route = esp32s3_electrical_model_endpoint(
            electrical, slot->id, ns, &endpoint);
        if (!component || !key || !slot->peer ||
            route == ESP32S3_ELECTRICAL_ROUTE_NONE ||
            endpoint.model != ESP32S3_ELECTRICAL_I2S_SAMPLE_PEER ||
            strcmp(slot->identity, key)) {
            slot_clear(slot);
        }
    }
    for (unsigned index = 0; index < PEER_LIMIT; ++index) {
        ESP32S3ElectricalEndpoint endpoint;
        if (esp32s3_electrical_model_endpoint_at(electrical,
                ESP32S3_ELECTRICAL_I2S_SAMPLE_PEER, index, ns, &endpoint) ==
                ESP32S3_ELECTRICAL_ROUTE_NONE) {
            break;
        }
        if (find_slot(service, endpoint.component_id)) {
            continue; /* Exact committed ID + canonical key survived. */
        }
        I2sPeerSlot *slot = NULL;
        for (unsigned i = 0; i < PEER_LIMIT; ++i) {
            if (!service->slots[i].id[0]) {
                slot = &service->slots[i];
                break;
            }
        }
        if (!slot) {
            error_report("I2S peer activation exceeds registered-model resource bound");
            vm_stop(RUN_STATE_PAUSED);
            break;
        }
        g_strlcpy(slot->id, endpoint.component_id, sizeof(slot->id));
        const char *key = NULL;
        const QDict *component = esp32s3_electrical_model_component(
            electrical, slot->id, &key);
        construct_peer(slot, component, key);
    }
    /* Core publishes the committed solved frame after activate returns;
     * new services start there, never sample the pending Apply batch. */
    bool settled = esp32s3_electrical_end_update(electrical);
    service->activating = false;
    if (!settled) {
        error_report("I2S peer activation: native atomic settlement unavailable");
        vm_stop(RUN_STATE_PAUSED);
    }
}

Object *esp32s3_i2s_peer_service_create(Object *soc, DeviceState *electrical)
{
    Object *obj = object_new(TYPE_I2S_PEER_SERVICE);
    I2sPeerService *service = (I2sPeerService *)obj;
    service->electrical = electrical;
    service->soc = soc;
    object_ref(OBJECT(electrical));
    object_property_add_child(soc, "i2s-sample-peers", obj);
    service->frame_subscribed = esp32s3_electrical_subscribe(
        electrical, physical_frame, service);
    service->model_subscribed = esp32s3_electrical_add_model_notify(
        electrical, model_wake, service);
    if (!service->frame_subscribed || !service->model_subscribed) {
        error_setg(&error_fatal, "I2S peer global electrical observer capacity unavailable");
    }
    service->registered = esp32s3_electrical_register_factory(
        electrical, ESP32S3_ELECTRICAL_I2S_SAMPLE_PEER,
        preflight, activate, service);
    if (!service->registered) {
        error_setg(&error_fatal, "I2S peer registered model factory unavailable");
    }
    object_unref(obj);
    return obj; /* Borrowed: the SOC's child property holds its strong ref. */
}
