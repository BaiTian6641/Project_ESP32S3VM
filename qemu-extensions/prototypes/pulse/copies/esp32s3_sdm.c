/* SPDX-License-Identifier: GPL-2.0-or-later
 * Named functional profile, NOT an exact silicon bitstream model.
 * TRM v1.8 6.5.4 gives Y = z^-1 X + (1-z^-1)^2 E but does not
 * disclose accumulator widths, quantizer ties or startup state.
 * This profile fixes those choices publicly; see waveform-assumptions QOM.
 */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "hw/qdev-clock.h"
#include "hw/misc/esp32s3_sdm.h"
#include "hw/misc/esp32s3_electrical.h"
#include "hw/xtensa/esp32s3_reset_domain.h"

#define CYCLE UINT64_C(1000000000)
#define FUNCTION_ENABLE (1U << 30)
#define CONFIG_ENABLE (1U << 31)

static unsigned divisor(const ESP32S3SdmChannel *c)
{
    return ((c->reg >> 8) & 255) + 1;
}

static uint64_t running_hz(ESP32S3SdmState *s)
{
    return (s->misc & FUNCTION_ENABLE) ? clock_get_hz(s->apb_clk) : 0;
}

/* A density change starts a new, zero-error profile trajectory. This is an
 * explicit profile choice, not a claim about an undocumented S3 integrator.
 * Divider/source changes and clock stops preserve the trajectory and phase. */
static void density_start(ESP32S3SdmChannel *c)
{
    c->error1 = c->error2 = 0;
    c->delayed_input = (int8_t)c->reg;
}

static void publish(ESP32S3SdmState *s, unsigned changed)
{
    if (!s->realized) {
        return;
    }
    esp32s3_electrical_begin_update(s->electrical);
    for (unsigned n = 0; n < 8; n++) {
        if (!(changed & (1U << n))) {
            continue;
        }
        /* Peripheral OE is always enabled; actual pad OE, mux, inversion,
         * finite output resistance and supply rails remain solver-owned. */
        esp32s3_electrical_set_matrix_drive(s->electrical, 93 + n,
                                           true, s->channels[n].level, false);
    }
    esp32s3_electrical_end_update(s->electrical);
}

static void account(ESP32S3SdmState *s, int64_t now)
{
    if (now > s->last_ns && s->hz) {
        __uint128_t elapsed = (__uint128_t)(now - s->last_ns) * s->hz;
        for (unsigned n = 0; n < 8; n++) {
            s->channels[n].phase += elapsed;
        }
    }
    s->last_ns = now;
}

static void schedule(ESP32S3SdmState *s)
{
    timer_del(s->event);
    if (!s->realized || !s->hz) {
        return;
    }
    uint64_t wait_ns = UINT64_MAX;
    for (unsigned n = 0; n < 8; n++) {
        ESP32S3SdmChannel *c = &s->channels[n];
        __uint128_t limit = (__uint128_t)divisor(c) * CYCLE;
        __uint128_t remaining = c->phase < limit ? limit - c->phase : 0;
        __uint128_t delay = (remaining + s->hz - 1) / s->hz;
        wait_ns = MIN(wait_ns, (uint64_t)MIN(delay, UINT64_MAX));
    }
    /* Bounded callback work: at most one pulse per channel per dispatch.
     * Overdue phase is retained, never truncated, with a positive-time
     * reschedule rather than an unbounded zero-time catch-up loop. */
    wait_ns = MAX(wait_ns, 1);
    if (wait_ns <= (uint64_t)(INT64_MAX - s->last_ns)) {
        timer_mod_ns(s->event, s->last_ns + wait_ns);
    }
}

static void tick(ESP32S3SdmChannel *c)
{
    /* Quantizer q(v) = +128 for v >= 0, -128 otherwise.
     * e[n] = q(v[n]) - v[n], v[n] = x[n-1]-2e[n-1]+e[n-2].
     * Therefore y[n]=x[n-1]+e[n]-2e[n-1]+e[n-2] exactly.
     * Exhaustive reachable-orbit certificates for all 256 densities bound
     * |e|<=28800 and |v|<=86528. Density changes MUST restart zero-error
     * state: arbitrary nonzero-error seeds need not be stable. Thus signed
     * 32-bit arithmetic is sufficient without wrapping or saturating. */
    int32_t v = c->delayed_input - 2 * c->error1 + c->error2;
    int32_t y = v >= 0 ? 128 : -128;
    c->error2 = c->error1;
    c->error1 = y - v;
    c->delayed_input = (int8_t)c->reg;
    c->level = y > 0;
}

static void event(void *opaque)
{
    ESP32S3SdmState *s = opaque;
    unsigned changed = 0;
    account(s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    for (unsigned n = 0; n < 8 && s->hz; n++) {
        ESP32S3SdmChannel *c = &s->channels[n];
        __uint128_t limit = (__uint128_t)divisor(c) * CYCLE;
        if (c->phase >= limit) {
            bool old = c->level;
            c->phase -= limit;
            tick(c);
            changed |= old != c->level ? 1U << n : 0;
        }
    }
    if (changed) {
        publish(s, changed);
    }
    schedule(s);
}

static uint64_t read_reg(void *opaque, hwaddr addr, unsigned size)
{
    ESP32S3SdmState *s = opaque;
    if (addr < 0x20) {
        return s->channels[addr / 4].reg;
    }
    switch (addr) {
    case 0x20: return s->cg;
    case 0x24: return s->misc;
    case 0x28: return s->version;
    default: return 0;
    }
}

static void write_reg(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    ESP32S3SdmState *s = opaque;
    account(s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    if (addr < 0x20) {
        ESP32S3SdmChannel *c = &s->channels[addr / 4];
        unsigned old_div = divisor(c);
        uint32_t previous = c->reg;
        c->reg = value & 0xffff;
        /* Retain fractional progress through the divided pulse cycle. */
        c->phase = c->phase * divisor(c) / old_div;
        if ((previous & 255) != (c->reg & 255)) {
            density_start(c);
        }
    } else {
        switch (addr) {
        case 0x20: s->cg = value & CONFIG_ENABLE; break;
        case 0x24: s->misc = value & (FUNCTION_ENABLE | (1U << 31)); break;
        case 0x28: s->version = value & 0x0fffffff; break;
        default: break;
        }
    }
    s->hz = running_hz(s);
    schedule(s);
}

static const MemoryRegionOps ops = {
    .read = read_reg, .write = write_reg, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4, .unaligned = false },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static void clock_changed(void *opaque, ClockEvent event_type)
{
    ESP32S3SdmState *s = opaque;
    account(s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    s->hz = running_hz(s);
    schedule(s);
}

static void reset(DeviceState *dev)
{
    ESP32S3SdmState *s = ESP32S3_SDM(dev);
    if (s->soc_reset && !esp32s3_reset_covers_periph(s->soc_reset)) {
        return;
    }
    timer_del(s->event);
    s->cg = s->misc = 0;
    s->version = 0x01802260;
    s->hz = 0;
    s->last_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    for (unsigned n = 0; n < 8; n++) {
        ESP32S3SdmChannel *c = &s->channels[n];
        c->reg = 0xff00;
        c->phase = 0;
        c->level = false;
        density_start(c);
    }
    publish(s, 0xff);
}

static char *profile(Object *obj, Error **errp)
{
    return g_strdup(ESP32S3_SDM_PROFILE);
}

static char *assumptions(Object *obj, Error **errp)
{
    return g_strdup("NOT silicon-bit-exact; second-order error-feedback quantizer +/-128, "
                    "tie-high; signed32 bounded |error|<=28800; zero error/reset output-low; "
                    "density-change restarts zero-error "
                    "trajectory and seeds delayed input; divider/source/gate retain phase/errors; "
                    "CG is configuration autogate override; MISC30 gates function clock; "
                    "pad inversion/OE/rails and passive RC are external solver state");
}

static void realize(DeviceState *dev, Error **errp)
{
    ESP32S3SdmState *s = ESP32S3_SDM(dev);
    if (!s->electrical || !clock_has_source(s->apb_clk)) {
        error_setg(errp, "SDM requires authoritative electrical graph and APB source");
        return;
    }
    s->realized = true;
    reset(dev);
}

static void unrealize(DeviceState *dev)
{
    ESP32S3SdmState *s = ESP32S3_SDM(dev);
    s->realized = false;
    timer_del(s->event);
    esp32s3_electrical_begin_update(s->electrical);
    for (unsigned n = 0; n < 8; n++) {
        esp32s3_electrical_set_matrix_drive(s->electrical, 93 + n,
                                           false, false, false);
    }
    esp32s3_electrical_end_update(s->electrical);
}

static void init(Object *obj)
{
    ESP32S3SdmState *s = ESP32S3_SDM(obj);
    memory_region_init_io(&s->iomem, obj, &ops, s, TYPE_ESP32S3_SDM, 0x2c);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    s->event = timer_new_ns(QEMU_CLOCK_VIRTUAL, event, s);
    s->apb_clk = qdev_init_clock_in(DEVICE(obj), "apb-clk", clock_changed,
                                   s, ClockUpdate);
    object_property_add_link(obj, "electrical", TYPE_DEVICE, (Object **)&s->electrical,
                             object_property_allow_set_link, OBJ_PROP_LINK_STRONG);
    object_property_add_link(obj, "soc-reset", TYPE_DEVICE, (Object **)&s->soc_reset,
                             object_property_allow_set_link, OBJ_PROP_LINK_STRONG);
    object_property_add_str(obj, "waveform-profile", profile, NULL);
    object_property_add_str(obj, "waveform-assumptions", assumptions, NULL);
}

static void finalize(Object *obj)
{
    ESP32S3SdmState *s = ESP32S3_SDM(obj);
    timer_free(s->event);
}

static void class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = realize;
    dc->unrealize = unrealize;
    device_class_set_legacy_reset(dc, reset);
    dc->desc = "ESP32-S3 SDM named second-order functional profile (not silicon-bit-exact)";
}

static const TypeInfo type_info = {
    .name = TYPE_ESP32S3_SDM, .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ESP32S3SdmState), .instance_init = init,
    .instance_finalize = finalize, .class_init = class_init,
};

static void register_types(void)
{
    type_register_static(&type_info);
}
type_init(register_types)
