/* SPDX-License-Identifier: GPL-2.0-or-later
 * Real registered WS2812 sink and powered NEC open-drain envelope source.
 * One authoritative electrical graph owns terminals, rails and finite drives.
 * Timers use QEMU virtual time; no firmware hook, hidden pull or GPIO injection.
 */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/cutils.h"
#include "qemu/timer.h"
#include "qemu/log.h"
#include "hw/qdev-properties.h"
#include "hw/misc/esp32s3_electrical.h"
#include "hw/timer/esp32s3_rmt_peers.h"
#include "hw/timer/esp32s3_rmt_ws2812.h"
#include "hw/timer/esp32s3_rmt_nec.h"
#include "qapi/error.h"
#include "qapi/qmp/qdict.h"
#include "qapi/qmp/qlist.h"
#include "qapi/qmp/qnum.h"
#include "qapi/qmp/qjson.h"
#include <math.h>

#define RMT_PEER_LIMIT 32
#define RMT_PEER_IDENTITY_SIZE 160

static const ESP32S3ElectricalModel peer_models[] = {
    ESP32S3_ELECTRICAL_WS2812_FUNCTIONAL_3V3,
    ESP32S3_ELECTRICAL_NEC_ENVELOPE_SOURCE,
};

typedef struct RmtPeerConfig {
    ESP32S3ElectricalModel model;
    unsigned led_count;
    uint32_t address, command, period_ms;
} RmtPeerConfig;

typedef struct RmtPeerSlot {
    ESP32S3RmtPeersState *parent;
    QEMUTimer *timer;
    ESP32S3ElectricalEndpoint endpoint;
    char identity[RMT_PEER_IDENTITY_SIZE];
    bool active, seen, have_epoch, clean_power, drive_known, drive_oe;
    uint64_t epoch, last_latch_ns;
    union {
        Esp32S3RmtWs2812 ws;
        Esp32S3RmtNec nec;
    } state;
} RmtPeerSlot;

struct ESP32S3RmtPeersState {
    DeviceState parent_obj;
    DeviceState *electrical;
    RmtPeerSlot slots[RMT_PEER_LIMIT];
    bool subscribed, factories[2], healthy;
};

static void peer_timer(void *opaque);
static void peer_refresh(void *opaque, uint64_t ns);

static bool parameter(const QDict *params, const char *name, unsigned min,
                      unsigned max, uint32_t *out, Error **errp)
{
    const QDict *quantity = params ?
        qobject_to(QDict, qdict_get(params, name)) : NULL;
    const char *unit = quantity ? qdict_get_try_str(quantity, "unit") : NULL;
    QNum *number = quantity ?
        qobject_to(QNum, qdict_get(quantity, "value")) : NULL;
    double value = number ? qnum_get_double(number) : NAN;
    if (!unit || strcmp(unit, "count") || !isfinite(value) ||
        value < min || value > max || floor(value) != value) {
        error_setg(errp, "RMT peer parameter '%s' requires an explicit integral count in %u..%u", name, min, max);
        return false;
    }
    *out = value;
    return true;
}

static bool peer_config(const QDict *component, RmtPeerConfig *out, Error **errp)
{
    const char *type = component ? qdict_get_try_str(component, "type") : NULL;
    const QDict *params = component ?
        qobject_to(QDict, qdict_get(component, "parameters")) : NULL;
    uint32_t leds;
    memset(out, 0, sizeof(*out));
    if (type && !strcmp(type, "ws2812-functional-3v3")) {
        out->model = ESP32S3_ELECTRICAL_WS2812_FUNCTIONAL_3V3;
        if (!params || qdict_size(params) != 1 ||
            !parameter(params, "led_count", 1, 256, &leds, errp)) {
            if (!params || qdict_size(params) != 1) {
                error_setg(errp, "WS2812 peer requires only the explicit led_count parameter");
            }
            return false;
        }
        out->led_count = leds;
        return true;
    }
    if (type && !strcmp(type, "nec-envelope-source")) {
        out->model = ESP32S3_ELECTRICAL_NEC_ENVELOPE_SOURCE;
        if (!params || qdict_size(params) != 3) {
            error_setg(errp, "NEC peer requires explicit address, command and period_ms count parameters");
            return false;
        }
        return parameter(params, "address", 0, 255, &out->address, errp) &&
               parameter(params, "command", 0, 255, &out->command, errp) &&
               parameter(params, "period_ms", 100, 60000, &out->period_ms, errp);
    }
    error_setg(errp, "Unsupported registered RMT peer profile");
    return false;
}

/* Pure candidate validation. No current state, timer or drive is mutated.
 * NativeNet owns the returned canonical key and its successful-Apply lifetime. */
static bool peer_preflight(void *opaque, const QDict *component,
                           char **identity, Error **errp)
{
    RmtPeerConfig config;
    if (!peer_config(component, &config, errp)) {
        return false;
    }
    if (config.model == ESP32S3_ELECTRICAL_WS2812_FUNCTIONAL_3V3) {
        *identity = g_strdup_printf("ws2812-functional-3v3:v1:led_count=%u", config.led_count);
    } else {
        *identity = g_strdup_printf("nec-envelope-source:v1:address=%u:command=%u:period_ms=%u",
                                    config.address, config.command, config.period_ms);
    }
    return true;
}

static void schedule_peer(RmtPeerSlot *slot)
{
    int64_t next = slot->endpoint.model == ESP32S3_ELECTRICAL_WS2812_FUNCTIONAL_3V3 ?
        esp32s3_rmt_ws2812_deadline(&slot->state.ws) :
        esp32s3_rmt_nec_deadline(&slot->state.nec);
    if (next < 0 || !slot->active) {
        if (slot->timer) {
            timer_del(slot->timer);
        }
        return;
    }
    if (!slot->timer) {
        slot->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, peer_timer, slot);
    }
    timer_mod(slot->timer, next);
}

/* A logical NEC high is RELEASE, never a hidden high source. Only the real
 * explicit external pull-up and shared finite electrical kernel resolve OUT. */
static bool nec_drive(RmtPeerSlot *slot)
{
    Esp32S3RmtNec *nec = &slot->state.nec;
    bool low = nec->configured && nec->power_known && nec->powered && !nec->level;
    if (slot->drive_known && slot->drive_oe == low) {
        return true;
    }
    if (!esp32s3_electrical_terminal_drive(slot->parent->electrical,
                                          slot->endpoint.component_id, "out", low, false)) {
        slot->parent->healthy = false;
        if (slot->timer) {
            timer_del(slot->timer);
        }
        qemu_log_mask(LOG_GUEST_ERROR, "RMT peer '%s': actual registered OUT drive unavailable\n", slot->endpoint.component_id);
        return false;
    }
    slot->drive_known = true;
    slot->drive_oe = low;
    return true;
}

static void ws_latch_time(RmtPeerSlot *slot, uint64_t before, uint64_t ns)
{
    if (slot->state.ws.frame_count != before) {
        slot->last_latch_ns = ns;
    }
}

static void peer_power_sample(RmtPeerSlot *slot,
                              const ESP32S3ElectricalEndpoint *endpoint, uint64_t ns)
{
    if (endpoint->model == ESP32S3_ELECTRICAL_NEC_ENVELOPE_SOURCE) {
        esp32s3_rmt_nec_power(&slot->state.nec, endpoint->power_known,
                              endpoint->powered, endpoint->power_on_ns,
                              endpoint->power_epoch, ns);
        slot->endpoint = *endpoint;
        if (nec_drive(slot)) {
            schedule_peer(slot);
        }
        return;
    }
    if (!endpoint->power_known) {
        esp32s3_rmt_ws2812_input(&slot->state.ws, false, false, ns);
        slot->endpoint = *endpoint;
        schedule_peer(slot);
        return;
    }
    if (!endpoint->powered) {
        if (!slot->clean_power) {
            esp32s3_rmt_ws2812_reset(&slot->state.ws);
            slot->last_latch_ns = 0;
            slot->clean_power = true;
        }
        slot->endpoint = *endpoint;
        if (slot->timer) {
            timer_del(slot->timer);
        }
        return;
    }
    if (!slot->have_epoch || slot->epoch != endpoint->power_epoch) {
        if (!slot->clean_power) {
            esp32s3_rmt_ws2812_reset(&slot->state.ws);
            slot->last_latch_ns = 0;
        }
        slot->epoch = endpoint->power_epoch;
        slot->have_epoch = true;
    }
    slot->clean_power = false;
    slot->endpoint = *endpoint;
    double volts;
    bool valid = false, level = false;
    bool available = esp32s3_electrical_terminal_sample(slot->parent->electrical,
                         endpoint->component_id, "din", ns, &volts, &valid, &level);
    uint64_t before = slot->state.ws.frame_count;
    esp32s3_rmt_ws2812_input(&slot->state.ws, available && valid, level, ns);
    ws_latch_time(slot, before, ns);
    schedule_peer(slot);
}

static void remove_peer(RmtPeerSlot *slot)
{
    if (slot->timer) {
        timer_del(slot->timer);
    }
    /* A removed graph object has no output branch left to release. During
     * unrealize, where it still exists, release the actual declared OUT. */
    slot->active = false;
    slot->drive_known = false;
}

static RmtPeerSlot *find_peer(ESP32S3RmtPeersState *s,
                              const ESP32S3ElectricalEndpoint *endpoint)
{
    RmtPeerSlot *free_slot = NULL;
    for (unsigned i = 0; i < RMT_PEER_LIMIT; ++i) {
        RmtPeerSlot *slot = &s->slots[i];
        if (slot->active && !strcmp(slot->endpoint.component_id, endpoint->component_id)) {
            return slot;
        }
        if (!slot->active && !free_slot) {
            free_slot = slot;
        }
    }
    return free_slot;
}

static bool configure_peer(RmtPeerSlot *slot,
                            const ESP32S3ElectricalEndpoint *endpoint, uint64_t ns)
{
    /* Configuration metadata is immutable within a committed graph generation.
     * Cache only typed configuration, never routing, voltage or RX payload. */
    if (slot->active && slot->endpoint.model == endpoint->model &&
        slot->endpoint.generation == endpoint->generation) {
        slot->seen = true;
        peer_power_sample(slot, endpoint, ns);
        return true;
    }
    const char *key = NULL;
    const QDict *component = esp32s3_electrical_model_component(slot->parent->electrical,
                                                               endpoint->component_id, &key);
    if (!component || !key || strlen(key) >= sizeof(slot->identity)) {
        slot->parent->healthy = false;
        return false;
    }
    if (slot->active && slot->endpoint.model == endpoint->model &&
        !strcmp(slot->identity, key)) {
        slot->seen = true;
        peer_power_sample(slot, endpoint, ns);
        return true;
    }
    RmtPeerConfig config;
    if (!peer_config(component, &config, NULL) || config.model != endpoint->model) {
        slot->parent->healthy = false;
        return false;
    }
    bool fresh = !slot->active || slot->endpoint.model != endpoint->model;
    bool changed = fresh || strcmp(slot->identity, key);
    if (changed) {
        if (slot->timer) {
            timer_del(slot->timer);
        }
        if (fresh || endpoint->model == ESP32S3_ELECTRICAL_WS2812_FUNCTIONAL_3V3) {
            memset(&slot->state, 0, sizeof(slot->state));
            slot->have_epoch = false;
            slot->clean_power = true;
            slot->last_latch_ns = 0;
            slot->drive_known = false;
            bool ok = endpoint->model == ESP32S3_ELECTRICAL_WS2812_FUNCTIONAL_3V3 ?
                esp32s3_rmt_ws2812_init(&slot->state.ws, config.led_count) :
                esp32s3_rmt_nec_init(&slot->state.nec, config.address, config.command, config.period_ms);
            if (!ok) {
                slot->parent->healthy = false;
                return false;
            }
        } else if (!esp32s3_rmt_nec_configure(&slot->state.nec, config.address,
                                              config.command, config.period_ms, ns)) {
            slot->parent->healthy = false;
            return false;
        }
        pstrcpy(slot->identity, sizeof(slot->identity), key);
    }
    slot->active = slot->seen = true;
    peer_power_sample(slot, endpoint, ns);
    return true;
}

static void peer_refresh(void *opaque, uint64_t ns)
{
    ESP32S3RmtPeersState *s = opaque;
    for (unsigned i = 0; i < RMT_PEER_LIMIT; ++i) {
        s->slots[i].seen = false;
    }
    for (unsigned model = 0; model < G_N_ELEMENTS(peer_models); ++model) {
        for (unsigned index = 0; index < RMT_PEER_LIMIT; ++index) {
            ESP32S3ElectricalEndpoint endpoint;
            ESP32S3ElectricalRoute route = esp32s3_electrical_model_endpoint_at(
                s->electrical, peer_models[model], index, ns, &endpoint);
            if (route == ESP32S3_ELECTRICAL_ROUTE_NONE) {
                break;
            }
            if (!endpoint.component_id[0]) {
                s->healthy = false;
                break;
            }
            RmtPeerSlot *slot = find_peer(s, &endpoint);
            if (!slot || !configure_peer(slot, &endpoint, ns)) {
                s->healthy = false;
                break;
            }
        }
    }
    for (unsigned i = 0; i < RMT_PEER_LIMIT; ++i) {
        if (s->slots[i].active && !s->slots[i].seen) {
            remove_peer(&s->slots[i]);
        }
    }
}

static void peer_activate(void *opaque, DeviceState *electrical)
{
    ESP32S3RmtPeersState *s = opaque;
    /* Read only the successfully COMMITTED component/key; no failed-Apply
     * staging cache can replace the old model state or invent a power cycle. */
    s->healthy = true;
    /* Reclaim removed identities before allocating replacements, so a valid
     * full-capacity Apply cannot fail because all old slots are still occupied.
     * This registry work occurs only at a committed Apply, never per edge. */
    uint64_t ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    for (unsigned i = 0; i < RMT_PEER_LIMIT; ++i) {
        RmtPeerSlot *slot = &s->slots[i];
        if (slot->active) {
            ESP32S3ElectricalEndpoint endpoint;
            ESP32S3ElectricalRoute route = esp32s3_electrical_model_endpoint(
                s->electrical, slot->endpoint.component_id, ns, &endpoint);
            if (route == ESP32S3_ELECTRICAL_ROUTE_NONE ||
                (endpoint.component_id[0] &&
                 endpoint.model != ESP32S3_ELECTRICAL_WS2812_FUNCTIONAL_3V3 &&
                 endpoint.model != ESP32S3_ELECTRICAL_NEC_ENVELOPE_SOURCE)) {
                remove_peer(slot);
            }
        }
    }
    peer_refresh(s, ns);
}

static void peer_timer(void *opaque)
{
    RmtPeerSlot *slot = opaque;
    if (!slot->active || !slot->parent->healthy) {
        return;
    }
    uint64_t ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    ESP32S3ElectricalEndpoint endpoint;
    ESP32S3ElectricalRoute route = esp32s3_electrical_model_endpoint(
        slot->parent->electrical, slot->endpoint.component_id, ns, &endpoint);
    if (route == ESP32S3_ELECTRICAL_ROUTE_NONE) {
        remove_peer(slot);
        return;
    }
    if (!endpoint.component_id[0]) {
        slot->parent->healthy = false;
        return;
    }
    peer_power_sample(slot, &endpoint, ns);
    if (!endpoint.power_known || !endpoint.powered) {
        return;
    }
    if (endpoint.model == ESP32S3_ELECTRICAL_WS2812_FUNCTIONAL_3V3) {
        uint64_t before = slot->state.ws.frame_count;
        esp32s3_rmt_ws2812_expire(&slot->state.ws, ns);
        ws_latch_time(slot, before, ns);
    } else {
        esp32s3_rmt_nec_step(&slot->state.nec, ns);
        if (!nec_drive(slot)) {
            return;
        }
    }
    schedule_peer(slot);
}

static void put_u64(QDict *dict, const char *name, uint64_t value)
{
    char number[32];
    snprintf(number, sizeof(number), "%" PRIu64, value);
    qdict_put_str(dict, name, number);
}

static char *peer_capture(Object *obj, Error **errp)
{
    ESP32S3RmtPeersState *s = ESP32S3_RMT_PEERS(obj);
    uint64_t ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    QDict *root = qdict_new();
    QList *peers = qlist_new();
    qdict_put_int(root, "abi", 1);
    put_u64(root, "sample_ns", ns);
    qdict_put_bool(root, "healthy", s->healthy);
    for (unsigned i = 0; i < RMT_PEER_LIMIT; ++i) {
        RmtPeerSlot *slot = &s->slots[i];
        if (!slot->active) {
            continue;
        }
        QDict *record = qdict_new();
        const ESP32S3ElectricalEndpoint *endpoint = &slot->endpoint;
        qdict_put_str(record, "component_id", endpoint->component_id);
        qdict_put_bool(record, "power_known", endpoint->power_known);
        qdict_put_bool(record, "powered", endpoint->powered);
        put_u64(record, "power_epoch", endpoint->power_epoch);
        put_u64(record, "power_on_ns", endpoint->power_on_ns);
        if (endpoint->model == ESP32S3_ELECTRICAL_WS2812_FUNCTIONAL_3V3) {
            Esp32S3RmtWs2812 *ws = &slot->state.ws;
            qdict_put_str(record, "model", "ws2812-functional-3v3");
            qdict_put_int(record, "led_count", ws->led_count);
            qdict_put_bool(record, "latch_valid", ws->latch_valid);
            if (ws->latch_valid) {
                static const char digits[] = "0123456789abcdef";
                char text[ESP32S3_RMT_WS2812_MAX_BYTES * 2 + 1];
                unsigned length = ws->led_count * 3;
                for (unsigned j = 0; j < length; ++j) {
                    text[j * 2] = digits[ws->latched[j] >> 4];
                    text[j * 2 + 1] = digits[ws->latched[j] & 15];
                }
                text[length * 2] = 0;
                qdict_put_str(record, "grb", text);
            } else {
                qdict_put_null(record, "grb");
            }
            put_u64(record, "last_latch_ns", slot->last_latch_ns);
            put_u64(record, "frame_count", ws->frame_count);
            put_u64(record, "errors", ws->errors);
            put_u64(record, "unknown_inputs", ws->unknown_inputs);
        } else {
            Esp32S3RmtNec *nec = &slot->state.nec;
            qdict_put_str(record, "model", "nec-envelope-source");
            qdict_put_int(record, "address", nec->address);
            qdict_put_int(record, "command", nec->command);
            qdict_put_int(record, "period_ms", nec->period_ms);
            put_u64(record, "frame_count", nec->frame_count);
            put_u64(record, "frame_abort_count", nec->frame_abort_count);
            qdict_put_bool(record, "drive_oe", slot->drive_known && slot->drive_oe);
            qdict_put_bool(record, "drive_level", false);
            /* Physical voltage is independently observable while this device
             * is off/high-Z. Do not invent a logic threshold from off rails. */
            double physical_voltage;
            bool physical_valid = false;
            bool physical_available = esp32s3_electrical_terminal_voltage(
                s->electrical, endpoint->component_id, "out", ns,
                &physical_voltage, &physical_valid);
            qdict_put_bool(record, "out_voltage_valid", physical_available && physical_valid);
            if (physical_available && physical_valid) {
                qdict_put(record, "out_voltage_v", qnum_from_double(physical_voltage));
            } else {
                qdict_put_null(record, "out_voltage_v");
            }
            bool valid = false, level = false;
            double voltage;
            bool available = esp32s3_electrical_terminal_sample(s->electrical,
                endpoint->component_id, "out", ns, &voltage, &valid, &level);
            qdict_put_bool(record, "out_valid", available && valid);
            if (available && valid) {
                qdict_put_bool(record, "out_level", level);
            } else {
                qdict_put_null(record, "out_level");
            }
        }
        qlist_append(peers, record);
    }
    qdict_put(root, "peers", peers);
    GString *json = qobject_to_json(QOBJECT(root));
    qobject_unref(root);
    return g_string_free(json, false);
}

static void peers_realize(DeviceState *dev, Error **errp)
{
    ESP32S3RmtPeersState *s = ESP32S3_RMT_PEERS(dev);
    if (!s->electrical) {
        error_setg(errp, "RMT peers require the one authoritative electrical graph");
        return;
    }
    for (unsigned i = 0; i < G_N_ELEMENTS(peer_models); ++i) {
        s->factories[i] = esp32s3_electrical_register_factory(s->electrical,
            peer_models[i], peer_preflight, peer_activate, s);
        if (!s->factories[i]) {
            for (unsigned j = 0; j < i; ++j) {
                esp32s3_electrical_unregister_factory(s->electrical, peer_models[j], s);
                s->factories[j] = false;
            }
            error_setg(errp, "RMT peer factory already owned or unavailable");
            return;
        }
    }
    s->subscribed = esp32s3_electrical_subscribe(s->electrical, peer_refresh, s);
    if (!s->subscribed) {
        for (unsigned i = 0; i < G_N_ELEMENTS(peer_models); ++i) {
            esp32s3_electrical_unregister_factory(s->electrical, peer_models[i], s);
            s->factories[i] = false;
        }
        error_setg(errp, "RMT peer physical observer capacity exhausted");
        return;
    }
    s->healthy = true;
}

static void peers_unrealize(DeviceState *dev)
{
    ESP32S3RmtPeersState *s = ESP32S3_RMT_PEERS(dev);
    if (s->subscribed) {
        esp32s3_electrical_unsubscribe(s->electrical, peer_refresh, s);
        s->subscribed = false;
    }
    for (unsigned i = 0; i < G_N_ELEMENTS(peer_models); ++i) {
        if (s->factories[i]) {
            esp32s3_electrical_unregister_factory(s->electrical, peer_models[i], s);
            s->factories[i] = false;
        }
    }
    for (unsigned i = 0; i < RMT_PEER_LIMIT; ++i) {
        RmtPeerSlot *slot = &s->slots[i];
        if (slot->active && slot->endpoint.model == ESP32S3_ELECTRICAL_NEC_ENVELOPE_SOURCE) {
            esp32s3_electrical_terminal_drive(s->electrical, slot->endpoint.component_id, "out", false, false);
        }
        remove_peer(slot);
    }
}

static void peers_init(Object *obj)
{
    ESP32S3RmtPeersState *s = ESP32S3_RMT_PEERS(obj);
    for (unsigned i = 0; i < RMT_PEER_LIMIT; ++i) {
        s->slots[i].parent = s;
    }
    object_property_add_str(obj, "capture-json", peer_capture, NULL);
}

static void peers_finalize(Object *obj)
{
    ESP32S3RmtPeersState *s = ESP32S3_RMT_PEERS(obj);
    for (unsigned i = 0; i < RMT_PEER_LIMIT; ++i) {
        if (s->slots[i].timer) {
            timer_free(s->slots[i].timer);
        }
    }
}

static Property peers_properties[] = {
    DEFINE_PROP_LINK("electrical", ESP32S3RmtPeersState, electrical,
                     TYPE_ESP32S3_ELECTRICAL, DeviceState *),
    DEFINE_PROP_END_OF_LIST(),
};

static void peers_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = peers_realize;
    dc->unrealize = peers_unrealize;
    dc->user_creatable = false;
    device_class_set_props(dc, peers_properties);
    /* Deliberately no MCU reset handler. Real external rail epochs alone
     * reset the peer state; circuit charge/topology belong to NativeNet. */
}

static const TypeInfo peers_type = {
    .name = TYPE_ESP32S3_RMT_PEERS,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(ESP32S3RmtPeersState),
    .instance_init = peers_init,
    .instance_finalize = peers_finalize,
    .class_init = peers_class_init,
};

static void peers_register_types(void)
{
    type_register_static(&peers_type);
}
type_init(peers_register_types)

DeviceState *esp32s3_rmt_peers_create(Object *soc, DeviceState *electrical)
{
    DeviceState *dev = qdev_new(TYPE_ESP32S3_RMT_PEERS);
    object_property_add_child(soc, "rmt-peers", OBJECT(dev));
    object_property_set_link(OBJECT(dev), "electrical", OBJECT(electrical), &error_fatal);
    qdev_realize(dev, NULL, &error_fatal);
    object_unref(OBJECT(dev));
    return dev;
}
