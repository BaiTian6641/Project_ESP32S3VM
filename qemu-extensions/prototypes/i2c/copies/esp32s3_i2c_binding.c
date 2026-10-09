/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "hw/i2c/esp32s3_i2c_binding.h"
#include "hw/misc/esp32s3_electrical.h"
#include "hw/qdev-properties.h"
#include "qemu/module.h"
#include "qemu/cutils.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "qapi/qmp/qdict.h"
#include "qapi/qmp/qlist.h"
#include "qapi/qmp/qnum.h"

#define TYPE_I2C_BINDING "esp32s3-i2c-binding"
#define SERVICE_LIMIT 128

/* Graph-owned scripted I2C master peers. The electrical core requires a
 * native factory for i2c-scripted-master components; these engines replay a
 * bounded per-bit waveform through the shared electrical net, so the native
 * slave FSM, arbitration and glitch-filter paths are exercised through the
 * same connectivity and line-state authority as physical peers. */
#define SCRIPT_LIMIT 4
#define SCRIPT_TXN_LIMIT 32
#define SCRIPT_DATA_LIMIT 32
#define SCRIPT_WAIT_POLLS 8192

typedef struct I2cScriptTxn {
    uint16_t address;
    uint16_t count;
    uint32_t glitch_ns;
    uint8_t data[SCRIPT_DATA_LIMIT];
    bool ten_bit, read, relay, glitch;
} I2cScriptTxn;

typedef struct I2cScript {
    uint64_t bit_ns, start_delay_ns;
    unsigned txn_count;
    I2cScriptTxn txns[SCRIPT_TXN_LIMIT];
} I2cScript;

enum {
    SCR_HALTED = 0,
    SCR_DELAY,
    SCR_BUS_WAIT,
    SCR_START,
    SCR_BIT_SETUP,
    SCR_BIT_HIGH,
    SCR_BIT_MID,
    SCR_ACK_SAMPLE,
    SCR_STOP_SETUP,
    SCR_STOP_HIGH,
    SCR_STOP_RELEASE,
    SCR_RSTART_SETUP,
    SCR_RSTART_HIGH,
    SCR_GLITCH_LOW,
    SCR_GLITCH_RELEASE,
    SCR_GAP,
};

typedef struct ScriptSlot {
    char id[65];
    bool used, started, active, start_armed;
    I2cScript script;
    QEMUTimer *timer;
    DeviceState *electrical;
    unsigned phase, txn, byte, bit, polls;
    bool in_ack, relay_pass, sda_low, scl_low;
    uint8_t shift;
    uint8_t received[SCRIPT_DATA_LIMIT];
    unsigned received_count;
} ScriptSlot;

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
    ScriptSlot scripts[SCRIPT_LIMIT];
    QEMUTimer *script_pool[SCRIPT_LIMIT];
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
        esp32s3_electrical_set_matrix_drive(b->electrical, 89 + controller * 2,
                                           oe, scl, !!(ctr & BIT(1)));
        esp32s3_electrical_set_matrix_drive(b->electrical, 90 + controller * 2,
                                           oe, sda, !!(ctr & BIT(0)));
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

/* --------------------------------------------------------------------- */
/* Scripted I2C master engine. */

static void script_arm(ScriptSlot *slot, int64_t delay)
{
    timer_mod(slot->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              MAX(1, delay));
}

static int64_t script_quarter(const I2cScript *script)
{
    return MAX(1, (int64_t)script->bit_ns / 4);
}

static void script_drive(ScriptSlot *slot, bool sda_low, bool scl_low)
{
    slot->sda_low = sda_low;
    slot->scl_low = scl_low;
    if (!esp32s3_electrical_terminal_drive(slot->electrical, slot->id,
                                           "scl", scl_low, false) ||
        !esp32s3_electrical_terminal_drive(slot->electrical, slot->id,
                                           "sda", sda_low, false)) {
        /* The committed component vanished: never drive an unregistered net. */
        slot->active = false;
        slot->phase = SCR_HALTED;
    }
}

static bool script_line(ScriptSlot *slot, bool scl_line, bool *level)
{
    double voltage;
    bool valid = false;
    *level = true;
    return esp32s3_electrical_terminal_sample(
        slot->electrical, slot->id, scl_line ? "scl" : "sda",
        qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), &voltage, &valid, level) && valid;
}

static void script_release(ScriptSlot *slot)
{
    script_drive(slot, false, false);
}

/* End the current transaction and continue with the next scripted step. */
static void script_txn_end(ScriptSlot *slot)
{
    const I2cScriptTxn *t = &slot->script.txns[slot->txn];
    if (t->relay && slot->relay_pass == 0) {
        /* The relay read pass feeds the following write pass. */
        slot->relay_pass = 1;
        slot->byte = 0;
        slot->bit = 0;
        slot->in_ack = false;
        slot->phase = SCR_BUS_WAIT;
        slot->polls = 0;
        script_arm(slot, script_quarter(&slot->script) * 4);
        return;
    }
    slot->txn++;
    slot->relay_pass = 0;
    slot->received_count = 0;
    if (slot->txn >= slot->script.txn_count) {
        slot->active = false;
        slot->phase = SCR_HALTED;
        script_release(slot);
        return;
    }
    slot->byte = 0;
    slot->bit = 0;
    slot->in_ack = false;
    slot->phase = SCR_GAP;
    script_arm(slot, script_quarter(&slot->script) * 4);
}

static unsigned script_addr_bytes(const ScriptSlot *slot, const I2cScriptTxn *t)
{
    bool read_pass = t->read || (t->relay && slot->relay_pass == 0);
    return t->ten_bit ? (read_pass ? 3 : 2) : 1;
}

static unsigned script_txn_bytes(const ScriptSlot *slot, const I2cScriptTxn *t)
{
    unsigned data = (t->relay && slot->relay_pass == 1) ?
                    slot->received_count : t->count;
    return script_addr_bytes(slot, t) + data;
}

static uint8_t script_tx_byte(const ScriptSlot *slot, const I2cScriptTxn *t,
                              unsigned index)
{
    bool read_pass = t->read || (t->relay && slot->relay_pass == 0);
    if (t->ten_bit) {
        if (read_pass) {
            if (index == 0 || index == 2) {
                return 0xF0 | (((t->address >> 8) & 3) << 1) | (index == 2);
            }
            if (index == 1) {
                return t->address & 0xff;
            }
        } else {
            if (index == 0) {
                return 0xF0 | (((t->address >> 8) & 3) << 1);
            }
            if (index == 1) {
                return t->address & 0xff;
            }
        }
    } else if (index == 0) {
        return (t->address << 1) | (read_pass ? 1 : 0);
    }
    if (t->relay && slot->relay_pass == 1) {
        return slot->received[index - 1];
    }
    return t->data[index - script_addr_bytes(slot, t)];
}

/* Does this byte position carry a data byte we receive (slave-driven)? */
static bool script_rx_byte(const ScriptSlot *slot, const I2cScriptTxn *t)
{
    bool read_pass = t->read || (t->relay && slot->relay_pass == 0);
    return read_pass && slot->byte >= script_addr_bytes(slot, t);
}

static void script_begin_bit(ScriptSlot *slot)
{
    const I2cScriptTxn *t = &slot->script.txns[slot->txn];
    int64_t q = script_quarter(&slot->script);
    bool rx = script_rx_byte(slot, t);
    if (slot->in_ack) {
        /* Ninth bit: we transmit the acknowledge for received bytes and
         * release SDA for transmitted bytes (the far side acknowledges). */
        bool final_byte = slot->byte + 1 == script_txn_bytes(slot, t);
        bool nack = rx && final_byte;
        script_drive(slot, rx && !nack, true);
    } else if (rx) {
        script_drive(slot, false, true);
    } else {
        uint8_t value = script_tx_byte(slot, t, slot->byte);
        bool bit = (value >> (7 - slot->bit)) & 1;
        script_drive(slot, !bit, true);
    }
    slot->phase = SCR_BIT_HIGH;
    script_arm(slot, q * 2);
}

static void script_step(void *opaque)
{
    ScriptSlot *slot = opaque;
    if (!slot->active || !slot->used) {
        return;
    }
    const I2cScript *script = &slot->script;
    const I2cScriptTxn *t = &script->txns[slot->txn];
    int64_t q = script_quarter(script);

    switch (slot->phase) {
    case SCR_DELAY:
    case SCR_GAP: {
        if (t->glitch) {
            slot->phase = SCR_GLITCH_LOW;
            script_arm(slot, q);
            return;
        }
        bool sda = false, scl = false;
        slot->start_armed = script_line(slot, false, &sda) &&
                           script_line(slot, true, &scl) && sda && scl;
        slot->phase = SCR_BUS_WAIT;
        slot->polls = 0;
        script_arm(slot, 1);
        return;
    }
    case SCR_BUS_WAIT: {
        bool sda, scl;
        if (!script_line(slot, false, &sda) || !script_line(slot, true, &scl)) {
            script_release(slot);
            slot->active = false;
            slot->phase = SCR_HALTED;
            return;
        }
        if (scl && (sda || slot->start_armed)) {
            slot->start_armed = false;
            /* START: SDA falls while SCL stays high. */
            slot->byte = 0;
            slot->bit = 0;
            slot->in_ack = false;
            script_drive(slot, true, false);
            slot->phase = SCR_START;
            script_arm(slot, q * 2);
            return;
        }
        if (++slot->polls > SCRIPT_WAIT_POLLS) {
            /* A stuck bus is not a fabricated idle line. */
            slot->txn = script->txn_count;
            script_txn_end(slot);
            return;
        }
        script_arm(slot, q);
        return;
    }
    case SCR_START:
        script_begin_bit(slot);
        return;
    case SCR_BIT_SETUP:
        script_begin_bit(slot);
        return;
    case SCR_BIT_HIGH:
        /* Release SCL; a stretching slave keeps the line low. */
        script_drive(slot, slot->sda_low, false);
        slot->phase = SCR_BIT_MID;
        script_arm(slot, q);
        return;
    case SCR_BIT_MID: {
        bool scl;
        if (!script_line(slot, true, &scl)) {
            script_release(slot);
            slot->active = false;
            slot->phase = SCR_HALTED;
            return;
        }
        if (!scl) {
            if (++slot->polls > SCRIPT_WAIT_POLLS) {
                slot->txn = script->txn_count;
                script_txn_end(slot);
                return;
            }
            script_arm(slot, q);
            return;
        }
        slot->polls = 0;
        bool rx = script_rx_byte(slot, t);
        if (slot->in_ack) {
            if (rx) {
                /* Our own acknowledge of a received byte: continue. */
                script_drive(slot, false, true);
                slot->byte++;
                slot->bit = 0;
                slot->in_ack = false;
                if (slot->byte >= script_txn_bytes(slot, t)) {
                    slot->phase = SCR_STOP_SETUP;
                    script_arm(slot, q);
                } else {
                    slot->phase = SCR_BIT_SETUP;
                    script_arm(slot, q);
                }
                return;
            }
            /* Far-side acknowledge of a byte we transmitted. */
            slot->phase = SCR_ACK_SAMPLE;
            script_arm(slot, q);
            return;
        }
        bool sda;
        if (!script_line(slot, false, &sda)) {
            script_release(slot);
            slot->active = false;
            slot->phase = SCR_HALTED;
            return;
        }
        if (rx) {
            /* Data bit driven by the addressed slave. */
            slot->shift = (slot->shift << 1) | sda;
        } else {
            uint8_t value = script_tx_byte(slot, t, slot->byte);
            bool bit = (value >> (7 - slot->bit)) & 1;
            if (bit && !sda) {
                /* A released-high bit read low is a lost arbitration. */
                script_release(slot);
                slot->active = false;
                slot->phase = SCR_HALTED;
                return;
            }
        }
        slot->bit++;
        if (slot->bit >= 8) {
            if (rx) {
                if (slot->byte >= script_addr_bytes(slot, t) && slot->received_count < SCRIPT_DATA_LIMIT) {
                    slot->received[slot->received_count++] = slot->shift;
                }
            }
            slot->in_ack = true;
        }
        script_drive(slot, slot->sda_low, true);
        slot->phase = SCR_BIT_SETUP;
        script_arm(slot, q);
        return;
    }
    case SCR_ACK_SAMPLE: {
        bool sda;
        if (!script_line(slot, false, &sda)) {
            script_release(slot);
            slot->active = false;
            slot->phase = SCR_HALTED;
            return;
        }
        if (sda) {
            /* No acknowledge: end this transaction through a normal STOP. */
            slot->phase = SCR_STOP_SETUP;
            script_arm(slot, q);
            return;
        }
        slot->byte++;
        slot->bit = 0;
        slot->in_ack = false;
        if (t->ten_bit && (t->read || t->relay) && slot->relay_pass == 0 &&
            slot->byte == 2) {
            /* Repeated START for the second 10-bit header (read phase). */
            script_drive(slot, false, true);
            slot->phase = SCR_RSTART_SETUP;
            script_arm(slot, q * 2);
            return;
        }
        if (slot->byte >= script_txn_bytes(slot, t)) {
            script_drive(slot, true, true);
            slot->phase = SCR_STOP_SETUP;
            script_arm(slot, q);
            return;
        }
        script_drive(slot, slot->sda_low, true);
        slot->phase = SCR_BIT_SETUP;
        script_arm(slot, q);
        return;
    }
    case SCR_STOP_SETUP:
        /* SDA low while SCL low, then SCL released and SDA released. */
        script_drive(slot, true, true);
        slot->phase = SCR_STOP_HIGH;
        script_arm(slot, q * 2);
        return;
    case SCR_STOP_HIGH:
        script_drive(slot, true, false);
        slot->phase = SCR_STOP_RELEASE;
        script_arm(slot, q);
        return;
    case SCR_STOP_RELEASE:
        script_release(slot);
        script_txn_end(slot);
        return;
    case SCR_RSTART_SETUP:
        script_drive(slot, false, false);
        slot->phase = SCR_RSTART_HIGH;
        script_arm(slot, q);
        return;
    case SCR_RSTART_HIGH:
        script_drive(slot, true, false);
        slot->phase = SCR_START;
        script_arm(slot, q * 2);
        return;
    case SCR_GLITCH_LOW:
        script_drive(slot, true, false);
        slot->phase = SCR_GLITCH_RELEASE;
        script_arm(slot, t->glitch_ns);
        return;
    case SCR_GLITCH_RELEASE:
        script_release(slot);
        script_txn_end(slot);
        return;
    default:
        slot->active = false;
        slot->phase = SCR_HALTED;
        return;
    }
}

/* --------------------------------------------------------------------- */
/* Scripted-master factory: preflight, activation and frame dispatch. */

static bool script_txn_parse(const QDict *txn, I2cScriptTxn *out, Error **errp)
{
    memset(out, 0, sizeof(*out));
    if (qdict_haskey(txn, "glitch_ns")) {
        int64_t glitch = qdict_get_try_int(txn, "glitch_ns", -1);
        if (glitch < 1 || glitch > 100000000) {
            error_setg(errp, "i2c-scripted-master: glitch_ns out of range");
            return false;
        }
        out->glitch = true;
        out->glitch_ns = glitch;
        return true;
    }
    int64_t address = qdict_get_try_int(txn, "address", -1);
    if (address < 0 || address > 1023) {
        error_setg(errp, "i2c-scripted-master: transaction address out of range");
        return false;
    }
    out->address = address;
    out->ten_bit = qdict_get_try_bool(txn, "ten_bit", false);
    if (out->ten_bit && address < 128) {
        error_setg(errp, "i2c-scripted-master: 10-bit addresses start at 0x78");
        return false;
    }
    if (qdict_haskey(txn, "relay")) {
        int64_t relay = qdict_get_try_int(txn, "relay", -1);
        if (relay < 1 || relay > SCRIPT_DATA_LIMIT || out->ten_bit ||
            qdict_haskey(txn, "read") || qdict_haskey(txn, "data")) {
            error_setg(errp, "i2c-scripted-master: invalid relay transaction");
            return false;
        }
        out->relay = true;
        out->count = relay;
        return true;
    }
    out->read = qdict_get_try_bool(txn, "read", false);
    if (out->read) {
        int64_t count = qdict_get_try_int(txn, "count", -1);
        if (count < 1 || count > SCRIPT_DATA_LIMIT) {
            error_setg(errp, "i2c-scripted-master: read count out of range");
            return false;
        }
        out->count = count;
        return true;
    }
    QList *data = qobject_to(QList, qdict_get(txn, "data"));
    if (!data) {
        error_setg(errp, "i2c-scripted-master: write transaction requires data");
        return false;
    }
    if (qlist_size(data) > SCRIPT_DATA_LIMIT) {
        error_setg(errp, "i2c-scripted-master: write data exceeds the bound");
        return false;
    }
    const QListEntry *entry;
    QLIST_FOREACH_ENTRY(data, entry) {
        QNum *num = qobject_to(QNum, qlist_entry_obj(entry));
        int64_t value;
        if (!num || !qnum_get_try_int(num, &value) || value < 0 || value > 255) {
            error_setg(errp, "i2c-scripted-master: write data byte out of range");
            return false;
        }
        out->data[out->count++] = value;
    }
    if (!out->count) {
        error_setg(errp, "i2c-scripted-master: empty write transaction");
        return false;
    }
    return true;
}

static bool script_parse(const QDict *component, I2cScript *out,
                         char **identity, Error **errp)
{
    memset(out, 0, sizeof(*out));
    QDict *attributes = qobject_to(QDict, qdict_get(component, "attributes"));
    QDict *config = attributes ?
        qobject_to(QDict, qdict_get(attributes, "native_i2c_script")) : NULL;
    if (!config) {
        error_setg(errp, "i2c-scripted-master: missing attributes.native_i2c_script");
        return false;
    }
    if (!qdict_haskey(config, "bit_ns")) {
        error_setg(errp, "i2c-scripted-master: bit_ns is required");
        return false;
    }
    int64_t bit_ns = qdict_get_try_int(config, "bit_ns", -1);
    if (bit_ns < 1000 || bit_ns > 100000000) {
        error_setg(errp, "i2c-scripted-master: bit_ns out of range");
        return false;
    }
    out->bit_ns = bit_ns;
    int64_t start_delay = qdict_get_try_int(config, "start_delay_ns", 0);
    if (start_delay < 0 || start_delay > 1000000000000LL) {
        error_setg(errp, "i2c-scripted-master: start_delay_ns out of range");
        return false;
    }
    out->start_delay_ns = start_delay;
    QList *txns = qobject_to(QList, qdict_get(config, "transactions"));
    if (!txns || !qlist_size(txns) || qlist_size(txns) > SCRIPT_TXN_LIMIT) {
        error_setg(errp, "i2c-scripted-master: 1..32 transactions required");
        return false;
    }
    GString *id = g_string_new("i2c-script-v1");
    g_string_append_printf(id, "|b=%llu|d=%llu|n=%d",
                           (unsigned long long)out->bit_ns,
                           (unsigned long long)out->start_delay_ns,
                           (int)qlist_size(txns));
    const QListEntry *entry;
    QLIST_FOREACH_ENTRY(txns, entry) {
        QDict *txn = qobject_to(QDict, qlist_entry_obj(entry));
        if (!txn || !script_txn_parse(txn, &out->txns[out->txn_count], errp)) {
            if (!txn) {
                error_setg(errp, "i2c-scripted-master: transaction must be an object");
            }
            g_string_free(id, true);
            return false;
        }
        I2cScriptTxn *t = &out->txns[out->txn_count++];
        if (t->glitch) {
            g_string_append_printf(id, "|g=%u", t->glitch_ns);
        } else {
            g_string_append_printf(id, "|a=%u;t=%d;r=%d;y=%d;c=%u;d=",
                                   t->address, t->ten_bit, t->read, t->relay,
                                   t->count);
            for (unsigned i = 0; i < t->count; i++) {
                g_string_append_printf(id, "%02x", t->data[i]);
            }
        }
    }
    *identity = g_string_free(id, false);
    return true;
}

static bool script_preflight(void *opaque, const QDict *component,
                             char **identity, Error **errp)
{
    I2cScript script;
    return script_parse(component, &script, identity, errp);
}

static void script_activate(void *opaque, DeviceState *electrical)
{
    I2CBinding *b = opaque;
    for (unsigned i = 0; i < SCRIPT_LIMIT; i++) {
        ScriptSlot *slot = &b->scripts[i];
        if (slot->timer) {
            timer_del(slot->timer);
        }
        memset(slot, 0, sizeof(*slot));
    }
    for (unsigned i = 0; i < SCRIPT_LIMIT; i++) {
        ESP32S3ElectricalEndpoint endpoint = { 0 };
        ESP32S3ElectricalRoute route = esp32s3_electrical_model_endpoint_at(
            electrical, ESP32S3_ELECTRICAL_I2C_SCRIPTED_MASTER, i,
            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), &endpoint);
        if (route == ESP32S3_ELECTRICAL_ROUTE_NONE) {
            break;
        }
        const char *factory_key = NULL;
        const QDict *component = esp32s3_electrical_model_component(
            electrical, endpoint.component_id, &factory_key);
        I2cScript script;
        char *identity = NULL;
        Error *error = NULL;
        ScriptSlot *slot = &b->scripts[i];
        if (!component || !script_parse(component, &script, &identity, &error) ||
            (factory_key && strcmp(factory_key, identity))) {
            error_free(error);
            g_free(identity);
            continue;
        }
        pstrcpy(slot->id, sizeof(slot->id), endpoint.component_id);
        slot->used = true;
        slot->script = script;
        slot->electrical = electrical;
        slot->timer = b->script_pool[i];
        g_free(identity);
    }
}

static void script_frame(I2CBinding *b)
{
    uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    for (unsigned i = 0; i < SCRIPT_LIMIT; i++) {
        ScriptSlot *slot = &b->scripts[i];
        if (!slot->used) {
            continue;
        }
        ESP32S3ElectricalEndpoint endpoint = { 0 };
        ESP32S3ElectricalRoute route = esp32s3_electrical_model_endpoint(
            b->electrical, slot->id, now, &endpoint);
        if (route != ESP32S3_ELECTRICAL_ROUTE_OK || !endpoint.powered) {
            /* Unknown or unpowered graph peers never drive the bus. */
            if (slot->active) {
                slot->active = false;
                slot->phase = SCR_HALTED;
                script_release(slot);
            }
            continue;
        }
        if (!slot->started) {
            /* Start on the first committed solved frame after activation,
             * never inside the Apply batch. */
            slot->started = true;
            slot->active = true;
            slot->phase = SCR_DELAY;
            script_arm(slot, slot->script.start_delay_ns);
        }
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
    for (unsigned i = 0; i < 2; i++) {
        /* Published edges synchronize master clocks and decode slave frames. */
        esp32s3_i2c_frame(b->controllers[i]);
    }
    script_frame(b);
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
    /* Graph i2c-scripted-master components require this native factory. */
    if (!esp32s3_electrical_register_factory(electrical,
            ESP32S3_ELECTRICAL_I2C_SCRIPTED_MASTER,
            script_preflight, script_activate, binding)) {
        error_setg(&error_fatal, "ESP32-S3 I2C scripted-master factory is already owned");
    }
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
        endpoint.model == ESP32S3_ELECTRICAL_24C02 ||
        endpoint.model == ESP32S3_ELECTRICAL_I2C_SCRIPTED_MASTER) {
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

static void binding_init(Object *obj)
{
    I2CBinding *b = (I2CBinding *)obj;
    for (unsigned i = 0; i < SCRIPT_LIMIT; i++) {
        b->script_pool[i] = timer_new_ns(QEMU_CLOCK_VIRTUAL, script_step,
                                         &b->scripts[i]);
    }
}

static void binding_finalize(Object *obj)
{
    I2CBinding *b = (I2CBinding *)obj;
    if (b->subscribed) {
        esp32s3_electrical_remove_model_notify(b->electrical, model_changed, b);
        esp32s3_electrical_unregister_factory(b->electrical,
            ESP32S3_ELECTRICAL_I2C_SCRIPTED_MASTER, b);
    }
    for (unsigned i = 0; i < SCRIPT_LIMIT; i++) {
        if (b->script_pool[i]) {
            timer_free(b->script_pool[i]);
        }
    }
}

static const TypeInfo binding_type = {
    .name = TYPE_I2C_BINDING, .parent = TYPE_DEVICE,
    .instance_size = sizeof(I2CBinding),
    .instance_init = binding_init,
    .instance_finalize = binding_finalize,
};
static void register_binding(void)
{
    type_register_static(&binding_type);
}
type_init(register_binding)
