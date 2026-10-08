/*
 * ESP32-S3 ADC test/integration sample provider over an explicit DC circuit
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Test-only QOM object (user-creatable, no MMIO, no guest hooks) implementing
 * the "esp32s3-adc-sample-provider" interface on top of the GPL net-dc
 * primitive: each sample solves an ACTUAL explicit resistor/rail circuit
 * (rail -> Rtop -> tap -> Rbot -> reference) with en_dc_solve() and reports
 * the tap voltage with its validity classification.
 *
 * This is NOT part of the machine default wiring: the board's real provider
 * is the parent's net-solver layer.  The object exists so qtests and lane
 * integration runs drive the ADC through solved physical values instead of
 * bare host constants, and so explicit electrical states (floating tap,
 * unpowered rail, digital ownership, scheduled source steps) are
 * first-class, deterministic test inputs.
 *
 * Properties (all plain QOM; created in instance_init so they exist for
 * -object/-device option parsing AND stay runtime-settable):
 *   unit, channel       ADC unit 0/1 and channel 0..9 answered by this object
 *   rail-mv, rail-mv-2  rail voltage in millivolts (rail-mv-2 after a step)
 *   source-switch-ns    absolute virtual time of the rail step (default: none)
 *   r-top-ohm, r-bot-ohm divider resistances in ohms (r-bot omitted when
 *                       float-net = true, leaving the tap node floating)
 *   float-net           open-circuit the tap node (floating sample)
 *   powered             board power state (false => UNPOWERED samples)
 *   digital-owned       pad owned by a digital driver (=> DIGITAL_OWNED)
 *   next                provider consulted for unmodeled channels (link)
 */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qemu/timer.h"
#include "qom/object_interfaces.h"
#include "hw/adc/net-dc.h"
#include "hw/misc/esp32s3_adc_provider.h"

#define TYPE_ESP32S3_ADC_NET_PROVIDER "adc-dc-provider"
OBJECT_DECLARE_SIMPLE_TYPE(Esp32S3AdcNetProvider, ESP32S3_ADC_NET_PROVIDER)

struct Esp32S3AdcNetProvider {
    Object parent_obj;

    uint8_t unit;
    uint8_t channel;
    /* Circuit values in explicit integer units (mV / ohms) so the solved
     * tap voltage is exactly reproducible; converted to volts for the
     * solver. */
    uint32_t rail_mv;
    uint32_t rail_mv_2;
    uint64_t source_switch_ns;
    uint32_t r_top_ohm;
    uint32_t r_bot_ohm;
    bool float_net;
    bool powered;
    bool digital_owned;
    Esp32S3AdcSampleProvider *next;
    uint64_t generation;
    bool generation_after_step;
};

static void esp32s3_adc_net_provider_sample(Esp32S3AdcSampleProvider *iface,
                                            unsigned unit, unsigned channel,
                                            Esp32S3AdcSample *out)
{
    Esp32S3AdcNetProvider *p = ESP32S3_ADC_NET_PROVIDER(iface);
    EnDcCircuit circuit;
    EnDcResult result;
    double rail;
    /* The caller stamps the requested aperture instant; solve and echo
     * exactly that instant (source-switch gating included).  Restamping
     * from a later clock read would violate the provider echo contract
     * and mask running-clock drift. */
    uint64_t requested_ns = out->sample_ns;

    memset(out, 0, sizeof(*out));
    out->sample_ns = requested_ns;

    /* Answer only the channel this provider instance models; anything else
     * forwards down the "next" chain (a wiring error stays UNKNOWN, never a
     * silent cross-read). */
    if (unit != p->unit || channel != p->channel) {
        if (p->next) {
            ESP32S3_ADC_SAMPLE_PROVIDER_GET_CLASS(p->next)
                ->sample(p->next, unit, channel, out);
        } else {
            out->validity = ESP32S3_ADC_SAMPLE_UNKNOWN;
        }
        return;
    }
    if (p->digital_owned) {
        out->validity = ESP32S3_ADC_SAMPLE_DIGITAL_OWNED;
        return;
    }
    if (!p->powered) {
        out->validity = ESP32S3_ADC_SAMPLE_UNPOWERED;
        return;
    }

    rail = (p->source_switch_ns != UINT64_MAX &&
            out->sample_ns >= p->source_switch_ns) ?
           (double)p->rail_mv_2 / 1000.0 : (double)p->rail_mv / 1000.0;

    /* Explicit circuit: node 0 reference, node 1 tap, node 2 rail.
     * float-net disconnects the tap entirely (open divider): an
     * unreferenced island the solver reports as EN_DC_FLOATING. */
    memset(&circuit, 0, sizeof(circuit));
    circuit.node_count = 3;
    circuit.element_count = p->float_net ? 1 : 3;
    circuit.elements[0].kind = EN_DC_VOLTAGE_SOURCE;
    circuit.elements[0].p = 2;
    circuit.elements[0].n = 0;
    circuit.elements[0].value = rail;
    if (!p->float_net) {
        circuit.elements[1].kind = EN_DC_RESISTOR;
        circuit.elements[1].p = 2;
        circuit.elements[1].n = 1;
        circuit.elements[1].value = (double)p->r_top_ohm;
        circuit.elements[2].kind = EN_DC_RESISTOR;
        circuit.elements[2].p = 1;
        circuit.elements[2].n = 0;
        circuit.elements[2].value = (double)p->r_bot_ohm;
    }

    switch (en_dc_solve(&circuit, &result)) {
    case EN_DC_OK:
        out->validity = ESP32S3_ADC_SAMPLE_VALID;
        out->voltage_v = result.voltage[1];
        break;
    case EN_DC_FLOATING:
        out->validity = ESP32S3_ADC_SAMPLE_FLOATING;
        break;
    default:
        /* Conflicting/invalid circuit parameters are a provider wiring
         * error: UNKNOWN, never a coerced number. */
        out->validity = ESP32S3_ADC_SAMPLE_UNKNOWN;
        break;
    }
}

static uint64_t esp32s3_adc_net_provider_generation(
    Esp32S3AdcSampleProvider *iface, unsigned unit, unsigned channel)
{
    Esp32S3AdcNetProvider *p = ESP32S3_ADC_NET_PROVIDER(iface);
    bool after_step;

    if (unit != p->unit || channel != p->channel) {
        return p->next ?
            ESP32S3_ADC_SAMPLE_PROVIDER_GET_CLASS(p->next)
                ->source_generation(p->next, unit, channel) : 0;
    }
    after_step = p->source_switch_ns != UINT64_MAX &&
                 qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) >= p->source_switch_ns;
    if (after_step != p->generation_after_step) {
        p->generation_after_step = after_step;
        if (p->rail_mv != p->rail_mv_2) {
            ++p->generation;
        }
    }
    return p->generation;
}

static void source_get_uint32(Object *obj, Visitor *v, const char *name,
                              void *opaque, Error **errp)
{
    visit_type_uint32(v, name, opaque, errp);
}

static void source_set_uint32(Object *obj, Visitor *v, const char *name,
                              void *opaque, Error **errp)
{
    Esp32S3AdcNetProvider *p = ESP32S3_ADC_NET_PROVIDER(obj);
    uint32_t *field = opaque, value;

    if (visit_type_uint32(v, name, &value, errp) && value != *field) {
        *field = value;
        ++p->generation;
    }
}

static void source_get_uint64(Object *obj, Visitor *v, const char *name,
                              void *opaque, Error **errp)
{
    visit_type_uint64(v, name, opaque, errp);
}

static void source_set_uint64(Object *obj, Visitor *v, const char *name,
                              void *opaque, Error **errp)
{
    Esp32S3AdcNetProvider *p = ESP32S3_ADC_NET_PROVIDER(obj);
    uint64_t *field = opaque, value;

    if (visit_type_uint64(v, name, &value, errp) && value != *field) {
        *field = value;
        ++p->generation;
    }
}

static bool bool_get_float_net(Object *obj, Error **errp)
{
    return ESP32S3_ADC_NET_PROVIDER(obj)->float_net;
}

static bool bool_get_powered(Object *obj, Error **errp)
{
    return ESP32S3_ADC_NET_PROVIDER(obj)->powered;
}

static bool bool_get_digital_owned(Object *obj, Error **errp)
{
    return ESP32S3_ADC_NET_PROVIDER(obj)->digital_owned;
}

static void bool_set_float_net(Object *obj, bool val, Error **errp)
{
    Esp32S3AdcNetProvider *p = ESP32S3_ADC_NET_PROVIDER(obj);

    if (p->float_net != val) {
        p->float_net = val;
        ++p->generation;
    }
}

static void bool_set_powered(Object *obj, bool val, Error **errp)
{
    Esp32S3AdcNetProvider *p = ESP32S3_ADC_NET_PROVIDER(obj);

    if (p->powered != val) {
        p->powered = val;
        ++p->generation;
    }
}

static void bool_set_digital_owned(Object *obj, bool val, Error **errp)
{
    Esp32S3AdcNetProvider *p = ESP32S3_ADC_NET_PROVIDER(obj);

    if (p->digital_owned != val) {
        p->digital_owned = val;
        ++p->generation;
    }
}

static void esp32s3_adc_net_provider_init(Object *obj)
{
    Esp32S3AdcNetProvider *p = ESP32S3_ADC_NET_PROVIDER(obj);

    /* Documented reset state.  Values from -object/-device option parsing
     * are applied after instance_init and override these; the mutable
     * electrical state stays runtime-settable (plain QOM properties are
     * not frozen by qdev realize). */
    p->rail_mv = 0;
    p->rail_mv_2 = 0;
    p->source_switch_ns = UINT64_MAX;
    p->float_net = false;
    p->powered = true;
    p->digital_owned = false;
    p->r_top_ohm = 10000;
    p->r_bot_ohm = 10000;

    object_property_add(obj, "rail-mv", "uint32", source_get_uint32,
                         source_set_uint32, NULL, &p->rail_mv);
    object_property_add(obj, "rail-mv-2", "uint32", source_get_uint32,
                         source_set_uint32, NULL, &p->rail_mv_2);
    object_property_add(obj, "source-switch-ns", "uint64", source_get_uint64,
                         source_set_uint64, NULL, &p->source_switch_ns);
    object_property_add(obj, "r-top-ohm", "uint32", source_get_uint32,
                         source_set_uint32, NULL, &p->r_top_ohm);
    object_property_add(obj, "r-bot-ohm", "uint32", source_get_uint32,
                         source_set_uint32, NULL, &p->r_bot_ohm);
    object_property_add_bool(obj, "float-net", bool_get_float_net,
                             bool_set_float_net);
    object_property_add_bool(obj, "powered", bool_get_powered,
                             bool_set_powered);
    object_property_add_bool(obj, "digital-owned", bool_get_digital_owned,
                             bool_set_digital_owned);

    /* Static wiring. */
    object_property_add_uint8_ptr(obj, "unit", &p->unit,
                                  OBJ_PROP_FLAG_READ | OBJ_PROP_FLAG_WRITE);
    object_property_add_uint8_ptr(obj, "channel", &p->channel,
                                  OBJ_PROP_FLAG_READ | OBJ_PROP_FLAG_WRITE);
    object_property_add_link(obj, "next", TYPE_ESP32S3_ADC_SAMPLE_PROVIDER,
                             (Object **)&p->next,
                             object_property_allow_set_link, 0);
}

static void esp32s3_adc_net_provider_class_init(ObjectClass *klass, void *data)
{
    Esp32S3AdcSampleProviderClass *pc =
        ESP32S3_ADC_SAMPLE_PROVIDER_CLASS(klass);

    pc->sample = esp32s3_adc_net_provider_sample;
    pc->source_generation = esp32s3_adc_net_provider_generation;
}

static const TypeInfo esp32s3_adc_net_provider_info = {
    .name = TYPE_ESP32S3_ADC_NET_PROVIDER,
    .parent = TYPE_OBJECT,
    .instance_size = sizeof(Esp32S3AdcNetProvider),
    .instance_init = esp32s3_adc_net_provider_init,
    .class_init = esp32s3_adc_net_provider_class_init,
    .interfaces = (InterfaceInfo[]) {
        { TYPE_ESP32S3_ADC_SAMPLE_PROVIDER },
        { TYPE_USER_CREATABLE },
        { }
    },
};

static const TypeInfo esp32s3_adc_sample_provider_intf_info = {
    .name = TYPE_ESP32S3_ADC_SAMPLE_PROVIDER,
    .parent = TYPE_INTERFACE,
    .class_size = sizeof(Esp32S3AdcSampleProviderClass),
};

static void esp32s3_adc_net_provider_register_types(void)
{
    type_register_static(&esp32s3_adc_sample_provider_intf_info);
    type_register_static(&esp32s3_adc_net_provider_info);
}

type_init(esp32s3_adc_net_provider_register_types)
