/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "hw/i2c/esp32s3_i2c_binding.h"
#include "hw/misc/esp32s3_electrical.h"
#include "hw/qdev-properties.h"
#include "qemu/module.h"
#include "qemu/cutils.h"
#include "qapi/error.h"

#define TYPE_I2C_BINDING "esp32s3-i2c-binding"
#define SERVICE_LIMIT 128

typedef struct ServiceSlot {
    char id[65];
    S3I2CService state;
    bool powered;
    uint64_t power_epoch;
    ESP32S3ElectricalModel model;
} ServiceSlot;
typedef struct I2CBinding {
    DeviceState parent;
    ESP32S3I2CState *controllers[2];
    ESP32S3GPIOState *gpio;
    DeviceState *electrical;
    ServiceSlot services[SERVICE_LIMIT];
    unsigned service_count;
    bool notifying, subscribed;
} I2CBinding;

/* Initial qualified routing requires the input and output matrix selectors to
 * name the same pad. Inversion/constant/bypass/IO_MUX-direct paths are not
 * coerced into favorable bus connectivity. */
static bool pin(I2CBinding *b, unsigned signal, unsigned *pad)
{
    uint32_t cfg = b->gpio->func_in_sel_cfg[signal];
    if (!(cfg & BIT(7)) || (cfg & BIT(6)) || (cfg & 63) >= 49) {
        return false;
    }
    *pad = cfg & 63;
    ESP32S3GpioDriveSnapshot snapshot;
    esp32s3_gpio_get_drive_snapshot(b->gpio, *pad, &snapshot);
    unsigned controller = (signal - 89) / 2;
    uint32_t force_bit = (signal & 1) ? BIT(1) : BIT(0);
    bool intrinsic_od = b->controllers[controller]->reg[1] & force_bit;
    return snapshot.matrix_gpio && snapshot.ie && snapshot.out_sel == signal &&
           !snapshot.out_inv && !snapshot.oen_inv &&
           (snapshot.oen_from_signal || snapshot.out_oe) &&
           (snapshot.open_drain || (snapshot.oen_from_signal && intrinsic_od));
}

static bool pins(I2CBinding *b, unsigned controller, unsigned *sda, unsigned *scl)
{
    return controller < 2 && pin(b, 90 + controller * 2, sda) &&
           pin(b, 89 + controller * 2, scl) && *sda != *scl;
}

static bool sample(void *opaque, unsigned controller, bool *sda, bool *scl)
{
    I2CBinding *b = opaque;
    unsigned sda_pad, scl_pad;
    double voltage;
    bool sda_valid = false, scl_valid = false;
    *sda = *scl = false;
    if (!pins(b, controller, &sda_pad, &scl_pad)) {
        return false;
    }
    uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    return esp32s3_electrical_line(b->electrical, sda_pad, now,
                                  &voltage, &sda_valid, sda) &&
           esp32s3_electrical_line(b->electrical, scl_pad, now,
                                  &voltage, &scl_valid, scl) &&
           sda_valid && scl_valid;
}

static void drive(void *opaque, unsigned controller, bool sda, bool scl)
{
    I2CBinding *b = opaque;
    if (controller < 2) {
        ESP32S3I2CState *s = b->controllers[controller];
        uint32_t ctr = s->reg[1], conf = s->reg[0x54 / 4];
        bool oe = s->gate && !s->reset_asserted && (conf & BIT(21)) &&
                  clock_get_hz((conf & BIT(20)) ? s->rc_fast : s->xtal);
        esp32s3_electrical_set_matrix_drive(b->electrical, 90 + controller * 2,
                                           oe, sda, !!(ctr & BIT(0)));
        esp32s3_electrical_set_matrix_drive(b->electrical, 89 + controller * 2,
                                           oe, scl, !!(ctr & BIT(1)));
    }
}

static void refresh_power(I2CBinding *b, ServiceSlot *slot,
                          const ESP32S3ElectricalI2CEndpoint *endpoint,
                          bool known);

static ServiceSlot *allocate_slot(I2CBinding *binding)
{
    for (unsigned i = 0; i < binding->service_count; i++) {
        if (!binding->services[i].id[0]) {
            return &binding->services[i];
        }
    }
    return binding->service_count == SERVICE_LIMIT ? NULL :
        &binding->services[binding->service_count++];
}

static S3I2CService *resolve(void *opaque, unsigned controller, uint8_t address,
                             bool *collision)
{
    I2CBinding *b = opaque;
    ESP32S3ElectricalI2CEndpoint endpoint;
    unsigned sda, scl;
    *collision = false;
    if (!pins(b, controller, &sda, &scl)) {
        return NULL;
    }
    ESP32S3ElectricalRoute route = esp32s3_electrical_i2c_resolve(
        b->electrical, sda, scl, address, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), &endpoint);
    *collision = route == ESP32S3_ELECTRICAL_ROUTE_COLLISION;
    if (route != ESP32S3_ELECTRICAL_ROUTE_OK || !endpoint.power_known ||
        !endpoint.powered) {
        return NULL;
    }
    S3I2CServiceKind kind = endpoint.model == ESP32S3_ELECTRICAL_SHT21 ?
                          S3_I2C_SHT21 : S3_I2C_EEPROM;
    for (unsigned i = 0; i < b->service_count; i++) {
        ServiceSlot *slot = &b->services[i];
        if (!strcmp(slot->id, endpoint.component_id)) {
            if (slot->state.kind == S3_I2C_EXTERNAL) {
                if (!slot->state.ops || slot->model != endpoint.model) {
                    return NULL;
                }
                refresh_power(b, slot, &endpoint, true);
                return &slot->state;
            }
            if (endpoint.model != ESP32S3_ELECTRICAL_SHT21 &&
                endpoint.model != ESP32S3_ELECTRICAL_24C02) {
                return NULL;
            }
            refresh_power(b, slot, &endpoint, true);
            return slot->state.kind == kind ? &slot->state : NULL;
        }
    }
    if (endpoint.model != ESP32S3_ELECTRICAL_SHT21 &&
        endpoint.model != ESP32S3_ELECTRICAL_24C02) {
        return NULL;
    }
    ServiceSlot *slot = allocate_slot(b);
    if (!slot) {
        return NULL;
    }
    pstrcpy(slot->id, sizeof(slot->id), endpoint.component_id);
    slot->model = endpoint.model;
    s3_i2c_service_init(&slot->state, kind);
    s3_i2c_service_power_reset(&slot->state, endpoint.power_on_ns);
    slot->powered = true;
    slot->power_epoch = endpoint.power_epoch;
    return &slot->state;
}

static void slave_drive(void *opaque, S3I2CService *service, bool sda, bool scl)
{
    I2CBinding *b = opaque;
    for (unsigned i = 0; i < b->service_count; i++) {
        if (&b->services[i].state == service) {
            esp32s3_electrical_i2c_drive(b->electrical, b->services[i].id, !sda, !scl);
            return;
        }
    }
}

static void refresh_power(I2CBinding *b, ServiceSlot *slot,
                          const ESP32S3ElectricalI2CEndpoint *endpoint,
                          bool known)
{
    bool external = slot->state.kind == S3_I2C_EXTERNAL && slot->state.ops &&
                    slot->model == endpoint->model;
    bool supported = slot->state.kind == S3_I2C_EXTERNAL ? external :
        endpoint->model == ESP32S3_ELECTRICAL_SHT21 ||
        endpoint->model == ESP32S3_ELECTRICAL_24C02;
    known = known && endpoint->power_known && supported;
    S3I2CServiceKind kind = external ? S3_I2C_EXTERNAL :
        endpoint->model == ESP32S3_ELECTRICAL_SHT21 ? S3_I2C_SHT21 : S3_I2C_EEPROM;
    bool changed = known &&
        (endpoint->power_epoch != slot->power_epoch ||
         endpoint->powered != slot->powered || kind != slot->state.kind);
    if (!known || !endpoint->powered || changed) {
        for (unsigned controller = 0; controller < 2; controller++) {
            ESP32S3I2CState *s = b->controllers[controller];
            if (s->service == &slot->state) {
                esp32s3_i2c_invalidate(s);
            }
        }
        slave_drive(b, &slot->state, true, true);
    }
    if (changed) {
        if (kind != slot->state.kind) {
            s3_i2c_service_init(&slot->state, kind);
        }
        s3_i2c_service_power_reset(&slot->state,
            endpoint->powered ? endpoint->power_on_ns :
            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    }
    if (known) {
        slot->powered = endpoint->powered;
        slot->power_epoch = endpoint->power_epoch;
    }
}

static void model_changed(void *opaque)
{
    I2CBinding *b = opaque;
    if (b->notifying) {
        return;
    }
    b->notifying = true;
    uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    for (unsigned i = 0; i < b->service_count; i++) {
        ServiceSlot *slot = &b->services[i];
        if (!slot->id[0]) {
            continue;
        }
        ESP32S3ElectricalI2CEndpoint endpoint = { 0 };
        bool known = esp32s3_electrical_i2c_endpoint(
            b->electrical, slot->id, now, &endpoint);
        refresh_power(b, slot, &endpoint, known);
    }
    for (unsigned i = 0; i < 2; i++) {
        ESP32S3I2CState *s = b->controllers[i];
        if (s->service) {
            bool collision = false;
            if (resolve(b, i, s->address, &collision) != s->service || collision) {
                esp32s3_i2c_invalidate(s);
            }
        }
    }
    b->notifying = false;
}

void esp32s3_i2c_bind_electrical(ESP32S3I2CState *a, ESP32S3I2CState *b,
                                ESP32S3GPIOState *gpio, DeviceState *electrical)
{
    DeviceState *dev = qdev_new(TYPE_I2C_BINDING);
    I2CBinding *binding = (I2CBinding *)dev;
    binding->controllers[0] = a;
    binding->controllers[1] = b;
    binding->gpio = gpio;
    binding->electrical = electrical;
    object_property_add_child(OBJECT(electrical), "i2c-services", OBJECT(dev));
    qdev_realize(dev, NULL, &error_fatal);
    S3I2CProvider provider = {
        .opaque = binding, .sample = sample, .drive = drive,
        .resolve = resolve, .slave_drive = slave_drive,
    };
    esp32s3_i2c_bind(a, &provider);
    esp32s3_i2c_bind(b, &provider);
    if (!esp32s3_electrical_add_model_notify(electrical, model_changed, binding)) {
        error_setg(&error_fatal, "ESP32-S3 I2C electrical observer registry is full");
    }
    binding->subscribed = true;
    object_unref(OBJECT(dev));
}

bool esp32s3_i2c_register_service(DeviceState *electrical, const char *component_id,
                                const S3I2CServiceOps *ops, void *opaque)
{
    if (!electrical || !component_id || !*component_id ||
        strlen(component_id) > 64 || !ops || !ops->address || !ops->write ||
        !ops->read || !ops->read_ack || !ops->stop || !ops->cancel ||
        !ops->power_reset || !ops->ready_ns) {
        return false;
    }
    Object *object = object_resolve_path_component(OBJECT(electrical), "i2c-services");
    if (!object) {
        return false;
    }
    I2CBinding *binding = (I2CBinding *)object;
    ESP32S3ElectricalEndpoint endpoint = { 0 };
    ESP32S3ElectricalRoute route = esp32s3_electrical_model_endpoint(
        electrical, component_id, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), &endpoint);
    if (route == ESP32S3_ELECTRICAL_ROUTE_NONE ||
        strcmp(endpoint.component_id, component_id) ||
        endpoint.model == ESP32S3_ELECTRICAL_SHT21 ||
        endpoint.model == ESP32S3_ELECTRICAL_24C02) {
        return false;
    }
    ServiceSlot *slot = NULL;
    for (unsigned i = 0; i < binding->service_count; i++) {
        if (!strcmp(binding->services[i].id, component_id)) {
            slot = &binding->services[i];
            if (slot->state.kind != S3_I2C_EXTERNAL ||
                (slot->state.ops && (slot->state.ops != ops ||
                                     slot->state.opaque != opaque))) {
                return false;
            }
            if (slot->state.ops) {
                return slot->model == endpoint.model;
            }
            break;
        }
    }
    if (!slot) {
        slot = allocate_slot(binding);
        if (!slot) {
            return false;
        }
        pstrcpy(slot->id, sizeof(slot->id), component_id);
    }
    if (!s3_i2c_service_init_external(&slot->state, ops, opaque)) {
        return false;
    }
    slot->model = endpoint.model;
    slot->powered = endpoint.power_known && endpoint.powered;
    slot->power_epoch = endpoint.power_epoch;
    if (slot->powered) {
        s3_i2c_service_power_reset(&slot->state, endpoint.power_on_ns);
    }
    return true;
}

void esp32s3_i2c_unregister_service(DeviceState *electrical,
                                  const char *component_id, void *opaque)
{
    if (!electrical || !component_id) {
        return;
    }
    Object *object = object_resolve_path_component(OBJECT(electrical), "i2c-services");
    if (!object) {
        return;
    }
    I2CBinding *binding = (I2CBinding *)object;
    for (unsigned i = 0; i < binding->service_count; i++) {
        ServiceSlot *slot = &binding->services[i];
        if (strcmp(slot->id, component_id) || slot->state.kind != S3_I2C_EXTERNAL ||
            !slot->state.ops || slot->state.opaque != opaque) {
            continue;
        }
        for (unsigned controller = 0; controller < 2; controller++) {
            ESP32S3I2CState *s = binding->controllers[controller];
            if (s->service == &slot->state) {
                esp32s3_i2c_invalidate(s);
            }
        }
        slave_drive(binding, &slot->state, true, true);
        s3_i2c_service_cancel(&slot->state);
        slot->state.ops = NULL;
        slot->state.opaque = NULL;
        slot->id[0] = 0;
        return;
    }
}

static void binding_finalize(Object *obj)
{
    I2CBinding *b = (I2CBinding *)obj;
    if (b->subscribed) {
        esp32s3_electrical_remove_model_notify(b->electrical, model_changed, b);
    }
}

static const TypeInfo binding_type = {
    .name = TYPE_I2C_BINDING, .parent = TYPE_DEVICE,
    .instance_size = sizeof(I2CBinding),
    .instance_finalize = binding_finalize,
};
static void register_binding(void)
{
    type_register_static(&binding_type);
}
type_init(register_binding)
