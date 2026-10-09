/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "hw/ssi/esp32s3_spi_binding.h"
#include "hw/ssi/esp32s3_spi_service.h"
#include "hw/misc/esp32s3_electrical.h"
#include "qemu/module.h"
#include "qemu/cutils.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "qapi/error.h"
#include "sysemu/runstate.h"

#define TYPE_SPI_BINDING "esp32s3-spi-binding"
#define MODEL_LIMIT 32

typedef struct SpiSlot {
    char id[65];
    S3SPINor nor;
    unsigned controllers;
    bool present, clock_valid, clock, seen;
} SpiSlot;
typedef struct SpiBinding {
    DeviceState parent;
    ESP32S3GpSpiState *controllers[2];
    ESP32S3GPIOState *gpio;
    DeviceState *electrical;
    SpiSlot slots[MODEL_LIMIT];
    QEMUBH *pause_bh;
    bool fault, subscribed;
} SpiBinding;
static const unsigned clock_signal[2] = {101, 66};
static const unsigned mosi_signal[2] = {103, 68};
static const unsigned miso_signal[2] = {102, 67};
static const unsigned cs_signal[2][6] = {{110, 111, 112, 113, 114, 115},
                                        {71, 72, 127, 0, 0, 0}};

static void model_stop(void *opaque)
{
    SpiBinding *b = opaque;
    if (b->fault) {
        vm_stop(RUN_STATE_PAUSED);
    }
}

static void strict(SpiBinding *b, const char *id, const char *reason, uint64_t now)
{
    if (!b->fault) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "SPI native NOR '%s' strict dependency: %s at %" PRIu64 " ns\n",
                      id, reason, now);
    }
    b->fault = true;
    qemu_bh_schedule(b->pause_bh);
}
static SpiSlot *slot_for(SpiBinding *b, const char *id, uint64_t now)
{
    SpiSlot *free_slot = NULL;
    for (unsigned i = 0; i < MODEL_LIMIT; i++) {
        SpiSlot *slot = &b->slots[i];
        if (slot->present && !strcmp(slot->id, id)) {
            return slot;
        }
        if (!slot->present && !free_slot) {
            free_slot = slot;
        }
    }
    /* A valid Apply may replace all 32 device identities at once. Reclaim a
     * removed model before allocating its replacement, without a second
     * historical device registry or temporarily exceeding the model bound. */
    if (!free_slot) {
        for (unsigned i = 0; i < MODEL_LIMIT; i++) {
            SpiSlot *slot = &b->slots[i];
            ESP32S3ElectricalSpiEndpoint endpoint;
            if (esp32s3_electrical_spi_endpoint(b->electrical, slot->id, now, &endpoint) ==
                ESP32S3_ELECTRICAL_ROUTE_NONE) {
                s3_spi_nor_cleanup(&slot->nor);
                memset(slot, 0, sizeof(*slot));
                free_slot = slot;
                break;
            }
        }
    }
    if (!free_slot || !s3_spi_nor_init(&free_slot->nor, NULL)) {
        strict(b, id, "native NOR model allocation/capacity unavailable", now);
        return NULL;
    }
    pstrcpy(free_slot->id, sizeof(free_slot->id), id);
    free_slot->present = true;
    return free_slot;
}

/* Enumerate firmware-selected pads, never a controller-only endpoint binding.
 * Routing is inspected at each physical CS assertion, not cached as RX data or
 * a second connectivity truth. Fan-out is supported by the actual graph; the
 * bounded GPIO cross-product is only used at CS boundaries for mode ownership. */
static unsigned output_pads(SpiBinding *b, unsigned signal, unsigned pads[49])
{
    unsigned count = 0;
    for (unsigned pad = 0; pad < 49; pad++) {
        ESP32S3GpioDriveSnapshot snapshot;
        esp32s3_gpio_get_drive_snapshot(b->gpio, pad, &snapshot);
        if (snapshot.matrix_gpio && snapshot.out_sel == signal &&
            (snapshot.oen_from_signal || snapshot.out_oe)) {
            pads[count++] = pad;
        }
    }
    return count;
}
static unsigned owners(SpiBinding *b, const char *id, uint64_t now)
{
    unsigned mask = 0;
    for (unsigned c = 0; c < 2; c++) {
        ESP32S3GpSpiState *controller = b->controllers[c];
        if (!controller->cs_mask) {
            continue;
        }
        unsigned mosi[49], sclk[49], cs[49];
        unsigned nm = output_pads(b, mosi_signal[c], mosi);
        unsigned nk = output_pads(b, clock_signal[c], sclk);
        for (unsigned select = 0; select < (c ? 3 : 6); select++) {
            if (!(controller->cs_mask & BIT(select))) {
                continue;
            }
            unsigned nc = output_pads(b, cs_signal[c][select], cs);
            for (unsigned m = 0; m < nm; m++) {
                for (unsigned k = 0; k < nk; k++) {
                    for (unsigned x = 0; x < nc; x++) {
                        ESP32S3ElectricalSpiEndpoint endpoint;
                        ESP32S3ElectricalRoute route = esp32s3_electrical_spi_resolve(
                            b->electrical, mosi[m], UINT_MAX, sclk[k], cs[x], now, &endpoint);
                        if (route == ESP32S3_ELECTRICAL_ROUTE_COLLISION) {
                            strict(b, id, "multiple devices on the selected physical CS", now);
                        } else if (route == ESP32S3_ELECTRICAL_ROUTE_OK &&
                                   !strcmp(endpoint.component_id, id)) {
                            mask |= BIT(c);
                        }
                    }
                }
            }
        }
    }
    return mask;
}
static bool accepts_mosi(S3SPINor *nor)
{
    return nor->phase == S3_SPI_NOR_COMMAND || nor->phase == S3_SPI_NOR_ADDRESS ||
           nor->phase == S3_SPI_NOR_PROGRAM;
}
static void physical_frame(void *opaque, uint64_t now)
{
    SpiBinding *b = opaque;
    b->fault = false;
    for (unsigned i = 0; i < MODEL_LIMIT; i++) {
        b->slots[i].seen = false;
    }
    for (unsigned index = 0; index < MODEL_LIMIT; index++) {
        ESP32S3ElectricalSpiEndpoint endpoint;
        ESP32S3ElectricalRoute route = esp32s3_electrical_spi_endpoint_at(
            b->electrical, index, now, &endpoint);
        if (route == ESP32S3_ELECTRICAL_ROUTE_NONE) {
            break;
        }
        if (endpoint.model != ESP32S3_ELECTRICAL_SPI_NOR_1M) {
            strict(b, endpoint.component_id, "unrecognized native SPI model", now);
            continue;
        }
        SpiSlot *slot = slot_for(b, endpoint.component_id, now);
        if (!slot) {
            continue;
        }
        slot->seen = true;
        if (!endpoint.power_known || route == ESP32S3_ELECTRICAL_ROUTE_UNKNOWN) {
            /* Unknown power is not a fabricated off event. Preserve storage,
             * WEL and pending work; block if those states depend on the rail. */
            if (slot->nor.selected || slot->nor.operation != S3_SPI_NOR_IDLE) {
                strict(b, slot->id, "unknown component power", now);
            }
            esp32s3_electrical_spi_drive(b->electrical, slot->id, false, false);
            slot->clock_valid = false;
            continue;
        }
        s3_spi_nor_power(&slot->nor, endpoint.powered, now);
        if (!endpoint.powered) {
            esp32s3_electrical_spi_drive(b->electrical, slot->id, false, false);
            slot->clock_valid = false;
            slot->controllers = 0;
            continue;
        }
        bool mosi_valid, mosi, sclk_valid, sclk, cs_valid, cs;
        if (!esp32s3_electrical_spi_sample(b->electrical, slot->id, now,
                                          &mosi_valid, &mosi, &sclk_valid, &sclk,
                                          &cs_valid, &cs) || !cs_valid) {
            strict(b, slot->id, "CS floating/contended/unknown", now);
            continue;
        }
        if (!cs && !slot->nor.selected) {
            slot->controllers = owners(b, slot->id, now);
            for (unsigned c = 0; c < 2; c++) {
                if ((slot->controllers & BIT(c)) &&
                    b->controllers[c]->cpol != b->controllers[c]->cpha) {
                    strict(b, slot->id, "spi-nor-1m supports electrical modes 0/3 only", now);
                }
            }
        }
        if (b->fault) {
            continue;
        }
        s3_spi_nor_select(&slot->nor, !cs, now);
        if (!cs) {
            if (!sclk_valid) {
                strict(b, slot->id, "selected SCLK floating/contended/unknown", now);
                continue;
            }
            if (slot->clock_valid && slot->clock != sclk) {
                if (sclk && accepts_mosi(&slot->nor) && !mosi_valid) {
                    strict(b, slot->id, "selected MOSI floating/contended/unknown", now);
                    continue;
                }
                bool miso, driven;
                /* MOSI has no semantic input in response/dummy/end phases.
                 * Its placeholder bit is ignored by the real parser, never RX. */
                s3_spi_nor_edge(&slot->nor, sclk, mosi_valid && mosi, now, &miso, &driven);
            }
        } else {
            slot->controllers = 0;
        }
        slot->clock_valid = sclk_valid;
        if (sclk_valid) {
            slot->clock = sclk;
        }
        if (!esp32s3_electrical_spi_drive(b->electrical, slot->id,
                                         slot->nor.driven, slot->nor.miso)) {
            strict(b, slot->id, "native finite MISO drive unavailable", now);
        }
    }
    /* Removing a graph component destroys that model instance. Rewiring or
     * controller reset does not: the registered component remains present. */
    for (unsigned i = 0; i < MODEL_LIMIT; i++) {
        SpiSlot *slot = &b->slots[i];
        if (slot->present && !slot->seen) {
            s3_spi_nor_cleanup(&slot->nor);
            memset(slot, 0, sizeof(*slot));
        }
    }
}
static bool drive(void *opaque, unsigned controller, bool clk, bool mosi,
                  bool mosi_oe, unsigned active_cs, unsigned polarity)
{
    SpiBinding *b = opaque;
    if (controller >= 2) {
        return false;
    }
    /* Source and settle MOSI before the physical clock edge. Establish idle
     * SCLK before initial CS assertion. Subscriber MISO consequences settle
     * through the electrical owner's bounded delta loop before we return. */
    esp32s3_electrical_set_matrix_drive(b->electrical, mosi_signal[controller],
                                       mosi_oe, mosi, false);
    esp32s3_electrical_set_matrix_drive(b->electrical, clock_signal[controller],
                                       true, clk, false);
    for (unsigned cs = 0; cs < (controller ? 3 : 6); cs++) {
        bool level = !(active_cs & BIT(cs));
        level ^= !!(polarity & BIT(cs));
        esp32s3_electrical_set_matrix_drive(b->electrical, cs_signal[controller][cs],
                                           true, level, false);
    }
    return !b->fault;
}
static bool sample(void *opaque, unsigned controller, bool *miso)
{
    SpiBinding *b = opaque;
    if (controller >= 2 || b->fault) {
        return false;
    }
    unsigned signal = miso_signal[controller];
    uint32_t cfg = b->gpio->func_in_sel_cfg[signal];
    if (!(cfg & BIT(7))) {
        return false; /* IO_MUX-direct input remains a canonical GPIO gate. */
    }
    unsigned pad = cfg & 63;
    if (pad == GPIO_FUNC_IN_HIGH || pad == GPIO_FUNC_IN_LOW) {
        return esp32s3_gpio_matrix_sample(b->gpio, signal, miso);
    }
    if (pad >= 49) {
        return false;
    }
    ESP32S3GpioDriveSnapshot snapshot;
    esp32s3_gpio_get_drive_snapshot(b->gpio, pad, &snapshot);
    if (!snapshot.ie) {
        return false;
    }
    double voltage;
    bool valid, level;
    if (!esp32s3_electrical_line(b->electrical, pad,
                                qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
                                &voltage, &valid, &level) || !valid) {
        return false;
    }
    /* This authoritative helper applies firmware-selected input inversion and
     * FUN_IE gating; it is distinct from an output latch and never injects input. */
    return esp32s3_gpio_matrix_sample(b->gpio, signal, miso);
}
static void end(void *opaque, unsigned controller, bool abort)
{
    SpiBinding *b = opaque;
    if (!abort || controller >= 2) {
        return;
    }
    uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (!esp32s3_electrical_begin_update(b->electrical)) {
        strict(b, "controller", "atomic electrical abort update unavailable", now);
        return;
    }
    /* Abort the chip transaction BEFORE releasing physical CS, so a reset
     * cannot masquerade as a clean program/WREN transaction boundary. */
    for (unsigned i = 0; i < MODEL_LIMIT; i++) {
        SpiSlot *slot = &b->slots[i];
        if (slot->present && (slot->controllers & BIT(controller))) {
            s3_spi_nor_abort(&slot->nor, now);
            slot->controllers &= ~BIT(controller);
        }
    }
    for (unsigned cs = 0; cs < (controller ? 3 : 6); cs++) {
        esp32s3_electrical_set_matrix_drive(b->electrical, cs_signal[controller][cs], false, false, false);
    }
    esp32s3_electrical_set_matrix_drive(b->electrical, mosi_signal[controller], false, false, false);
    esp32s3_electrical_set_matrix_drive(b->electrical, clock_signal[controller], false, false, false);
    if (!esp32s3_electrical_end_update(b->electrical)) {
        strict(b, "controller", "atomic electrical abort did not settle", now);
    }
}
void esp32s3_spi_bind_electrical(ESP32S3GpSpiState *a, ESP32S3GpSpiState *b,
                                ESP32S3GPIOState *gpio, DeviceState *electrical)
{
    DeviceState *dev = qdev_new(TYPE_SPI_BINDING);
    SpiBinding *binding = (SpiBinding *)dev;
    binding->controllers[0] = a;
    binding->controllers[1] = b;
    binding->gpio = gpio;
    binding->electrical = electrical;
    binding->pause_bh = qemu_bh_new(model_stop, binding);
    object_property_add_child(OBJECT(electrical), "spi-services", OBJECT(dev));
    qdev_realize(dev, NULL, &error_fatal);
    S3SPIProvider provider = {.opaque = binding, .drive = drive, .sample = sample, .end = end};
    esp32s3_gpspi_bind(a, &provider);
    esp32s3_gpspi_bind(b, &provider);
    /* IDF routes the MISO pad output through FSPIQ_OUT with peripheral OE.
     * The qualified master never drives it; register its released authority
     * so the electrical owner leaves the input buffer uncontended. */
    for (unsigned c = 0; c < 2; c++) {
        esp32s3_electrical_set_matrix_drive(electrical, miso_signal[c],
                                           false, false, false);
    }
    if (!esp32s3_electrical_subscribe(electrical, physical_frame, binding)) {
        error_report("SPI native service electrical observer capacity unavailable");
        exit(1);
    }
    binding->subscribed = true;
    physical_frame(binding, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    object_unref(OBJECT(dev));
}
static void finalize(Object *obj)
{
    SpiBinding *b = (SpiBinding *)obj;
    if (b->subscribed) {
        esp32s3_electrical_unsubscribe(b->electrical, physical_frame, b);
    }
    qemu_bh_delete(b->pause_bh);
    for (unsigned i = 0; i < MODEL_LIMIT; i++) {
        if (b->slots[i].present) {
            s3_spi_nor_cleanup(&b->slots[i].nor);
        }
    }
}
static const TypeInfo binding_type = {
    .name = TYPE_SPI_BINDING, .parent = TYPE_DEVICE,
    .instance_size = sizeof(SpiBinding), .instance_finalize = finalize,
};
static void register_binding(void) { type_register_static(&binding_type); }
type_init(register_binding)
