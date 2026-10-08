/* SPDX-License-Identifier: GPL-2.0-or-later */
/* ESP32-S3 PCNT: IDF 6.1 target pcnt_struct.h/pcnt_ll.h and TRM v1.8 ch38.
 * Inputs are solved electrical frames, not GPIO output bits or count injection.
 * One bounded timer services the sixteen pulse/control filter candidates.
 */
#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-clock.h"
#include "hw/misc/esp32s3_pcnt.h"
#include "hw/xtensa/esp32s3_reset_domain.h"

#define FILTER_EN BIT(10)
#define CTRL_MASK 0x100ffU
#define DATE_RESET 0x19072601U
#define CONF0_RESET 0x3c10U

static void pcnt_irq(ESP32S3PcntState *s)
{
    qemu_set_irq(s->irq, !s->reset_held && (s->int_raw & s->int_ena));
}

static bool pcnt_running(ESP32S3PcntState *s)
{
    return s->gate && !s->reset_held && s->period;
}

static uint64_t filter_cycles(ESP32S3PcntUnit *u)
{
    return (u->conf[0] & FILTER_EN) ? (uint64_t)(u->conf[0] & 1023) << 32 : 0;
}

/* The two channels feed one signed sixteen-bit adder. Simultaneous accepted
 * edges are summed before comparators run, rather than ordered by channel. */
static void pcnt_add(ESP32S3PcntState *s, unsigned index, int delta)
{
    ESP32S3PcntUnit *u = &s->unit[index];
    int old = u->count;
    int value;
    uint32_t events = 0, conf = u->conf[0];
    bool high, low;
    if (!delta || (s->ctrl & (3U << (2 * index)))) {
        return;
    }
    value = (int16_t)(uint16_t)(old + delta);
    high = delta > 0 && value == (int16_t)u->applied_limits;
    low = delta < 0 && value == (int16_t)(u->applied_limits >> 16);
    if ((conf & BIT(15)) && value == (int16_t)(u->applied_thres >> 16)) {
        events |= BIT(2);
    }
    if ((conf & BIT(14)) && value == (int16_t)u->applied_thres) {
        events |= BIT(3);
    }
    if ((conf & BIT(13)) && low) {
        events |= BIT(4);
    }
    if ((conf & BIT(12)) && high) {
        events |= BIT(5);
    }
    if (!value && (conf & BIT(11))) {
        events |= BIT(6);
    }
    /* A limit clears the adder irrespective of the comparator IRQ enable.
     * Limit reset does not fabricate a second zero-cross watch event. */
    u->count = (high || low) ? 0 : value;
    u->status = (u->status & ~3U) |
        (value < 0 ? 2 : value > 0 ? 3 : old < 0 ? 1 : 0);
    if (events) {
        u->status |= events;
        s->int_raw |= BIT(index);
    }
}

static int pulse_action(ESP32S3PcntUnit *u, unsigned channel, bool rising)
{
    ESP32S3PcntInput *control = &u->input[2 + channel];
    unsigned shift = 16 + 8 * channel;
    unsigned edge = (u->conf[0] >> (shift + (rising ? 2 : 0))) & 3;
    unsigned level;
    int delta;
    if (!control->valid || (edge != 1 && edge != 2)) {
        return 0;
    }
    level = (u->conf[0] >> (shift + (control->filtered ? 4 : 6))) & 3;
    if (level >= 2) {
        return 0;
    }
    delta = edge == 1 ? 1 : -1;
    return level ? -delta : delta;
}

static void pcnt_commit(ESP32S3PcntState *s)
{
    if (!pcnt_running(s)) {
        return;
    }
    for (unsigned n = 0; n < ESP32S3_PCNT_UNITS; ++n) {
        ESP32S3PcntUnit *u = &s->unit[n];
        uint64_t threshold = filter_cycles(u);
        int delta = 0;
        /* Controls settle before pulse decisions from this same frame. */
        for (unsigned k = 2; k < 4; ++k) {
            ESP32S3PcntInput *in = &u->input[k];
            if (in->pending && in->age >= threshold) {
                in->filtered = in->raw;
                in->pending = false;
            }
        }
        for (unsigned k = 0; k < 2; ++k) {
            ESP32S3PcntInput *in = &u->input[k];
            if (in->pending && in->age >= threshold) {
                in->filtered = in->raw;
                in->pending = false;
                delta += pulse_action(u, k, in->filtered);
            }
        }
        pcnt_add(s, n, delta);
    }
    pcnt_irq(s);
}

static void pcnt_advance(ESP32S3PcntState *s, uint64_t now)
{
    if (pcnt_running(s) && now > s->last_ns) {
        /* QEMU clock periods use 2^-32 ns. Q32 cycles retain fractional
         * progress even at a nonintegral period or while the source changes. */
        __uint128_t elapsed = ((__uint128_t)(now - s->last_ns) << 64) +
                              s->cycle_remainder;
        __uint128_t cycles = elapsed / s->period;
        s->cycle_remainder = elapsed % s->period;
        for (unsigned n = 0; n < ESP32S3_PCNT_UNITS; ++n) {
            for (unsigned k = 0; k < 4; ++k) {
                ESP32S3PcntInput *in = &s->unit[n].input[k];
                if (in->pending) {
                    /* Only 1023 cycles matter; saturating avoids overflow. */
                    in->age = MIN((__uint128_t)in->age + cycles,
                                  (uint64_t)1023 << 32);
                }
            }
        }
    }
    s->last_ns = now;
    pcnt_commit(s);
}

static void pcnt_schedule(ESP32S3PcntState *s)
{
    uint64_t remaining = UINT64_MAX;
    timer_del(s->filter_timer);
    if (!pcnt_running(s)) {
        return;
    }
    for (unsigned n = 0; n < ESP32S3_PCNT_UNITS; ++n) {
        uint64_t threshold = filter_cycles(&s->unit[n]);
        for (unsigned k = 0; k < 4; ++k) {
            ESP32S3PcntInput *in = &s->unit[n].input[k];
            if (in->pending) {
                remaining = MIN(remaining, threshold > in->age ? threshold - in->age : 0);
            }
        }
    }
    if (remaining != UINT64_MAX) {
        __uint128_t delay = (__uint128_t)remaining * s->period;
        uint64_t ns = MIN((delay + (((__uint128_t)1 << 64) - 1)) >> 64,
                          INT64_MAX);
        /* A zero threshold commits synchronously; never a zero-time loop. */
        timer_mod(s->filter_timer, MIN((__uint128_t)s->last_ns + MAX(ns, 1), INT64_MAX));
    }
}

static bool pcnt_sample(ESP32S3PcntState *s, unsigned signal, bool *level)
{
    uint32_t route = s->gpio->func_in_sel_cfg[signal];
    unsigned pad = route & 63;
    bool sampled;
    if (pad < ESP32S3_GPIO_COUNT) {
        ESP32S3GpioDriveSnapshot drive;
        esp32s3_gpio_get_drive_snapshot(s->gpio, pad, &drive);
        if (!drive.ie) {
            return false;
        }
    }
    /* Read the GPIO owner's register truth, not a second route/net store.
     * matrix_sample is the strict UNKNOWN consumption gate. Its persisted
     * bit must not become an edge when the electrical frame is unresolved. */
    sampled = esp32s3_gpio_matrix_sample(s->gpio, signal, level);
    if (!(route & BIT(7))) {
        return false;
    }
    if (pad == GPIO_FUNC_IN_HIGH || pad == GPIO_FUNC_IN_LOW) {
        return sampled;
    }
    return sampled && pad < ESP32S3_GPIO_COUNT &&
           esp32s3_gpio_net_valid(s->gpio, pad);
}

static void pcnt_inputs(void *opaque, uint64_t now)
{
    ESP32S3PcntState *s = opaque;
    if (!s->subscribed || s->reset_held) {
        return;
    }
    pcnt_advance(s, now);
    for (unsigned n = 0; n < ESP32S3_PCNT_UNITS; ++n) {
        for (unsigned k = 0; k < 4; ++k) {
            ESP32S3PcntInput *in = &s->unit[n].input[k];
            unsigned channel = k & 1;
            uint32_t modes = s->unit[n].conf[0] >> (16 + 8 * channel);
            unsigned positive = (modes >> 2) & 3, negative = modes & 3;
            if (!pcnt_running(s) || (s->ctrl & (3U << (2 * n))) ||
                ((positive != 1 && positive != 2) &&
                 (negative != 1 && negative != 2))) {
                in->valid = in->pending = false;
                in->age = 0;
                continue;
            }
            bool level;
            bool valid = pcnt_sample(s, 33 + 4 * n + k, &level);
            if (!valid || !pcnt_running(s)) {
                in->valid = in->pending = false;
                in->age = 0;
            } else if (!in->valid) {
                /* First resolved sample establishes a baseline, not an edge. */
                in->valid = true;
                in->raw = in->filtered = level;
                in->pending = false;
                in->age = 0;
            } else if (level != in->raw) {
                in->raw = level;
                in->pending = level != in->filtered;
                in->age = 0;
            }
        }
    }
    pcnt_commit(s);
    pcnt_schedule(s);
}

static void pcnt_timer(void *opaque)
{
    pcnt_inputs(opaque, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
}

static void pcnt_clear_unit(ESP32S3PcntUnit *u)
{
    u->count = 0;
    u->status &= 3;
    u->applied_thres = u->conf[1];
    u->applied_limits = u->conf[2];
}

static void pcnt_reset(ESP32S3PcntState *s)
{
    timer_del(s->filter_timer);
    s->ctrl = 0x55;
    s->int_raw = s->int_ena = 0;
    s->date = DATE_RESET;
    s->last_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->cycle_remainder = 0;
    for (unsigned n = 0; n < ESP32S3_PCNT_UNITS; ++n) {
        ESP32S3PcntUnit *u = &s->unit[n];
        memset(u, 0, sizeof(*u));
        u->conf[0] = CONF0_RESET;
    }
    pcnt_irq(s);
}

static uint64_t pcnt_read(void *opaque, hwaddr addr, unsigned size)
{
    ESP32S3PcntState *s = opaque;
    pcnt_advance(s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    pcnt_schedule(s);
    if (addr < 0x30) {
        return s->unit[addr / 12].conf[(addr % 12) / 4];
    }
    if (addr >= 0x30 && addr < 0x40) {
        return (uint16_t)s->unit[(addr - 0x30) / 4].count;
    }
    if (addr >= 0x50 && addr < 0x60) {
        return s->unit[(addr - 0x50) / 4].status;
    }
    switch (addr) {
    case 0x40: return s->int_raw;
    case 0x44: return s->int_raw & s->int_ena;
    case 0x48: return s->int_ena;
    case 0x60: return s->ctrl;
    case 0xfc: return s->date;
    default: return 0;
    }
}

static void pcnt_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    ESP32S3PcntState *s = opaque;
    uint32_t val = value;
    if (s->reset_held) {
        return;
    }
    pcnt_advance(s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    if (addr < 0x30) {
        unsigned n = addr / 12, reg = (addr % 12) / 4;
        ESP32S3PcntUnit *u = &s->unit[n];
        u->conf[reg] = val;
        if (s->ctrl & BIT(2 * n)) {
            pcnt_clear_unit(u);
        }
    } else {
        switch (addr) {
        case 0x48:
            s->int_ena = val & 15;
            break;
        case 0x4c:
            s->int_raw &= ~(val & 15);
            for (unsigned n = 0; n < ESP32S3_PCNT_UNITS; ++n) {
                if (val & BIT(n)) {
                    s->unit[n].status &= 3;
                }
            }
            break;
        case 0x60:
            s->ctrl = val & CTRL_MASK;
            for (unsigned n = 0; n < ESP32S3_PCNT_UNITS; ++n) {
                if (val & BIT(2 * n)) {
                    pcnt_clear_unit(&s->unit[n]);
                }
            }
            break;
        case 0xfc:
            s->date = val;
            break;
        default:
            break; /* Counter/status/RAW/ST are read-only, holes are zero. */
        }
    }
    if (s->subscribed) {
        pcnt_inputs(s, s->last_ns);
    }
    pcnt_commit(s);
    pcnt_schedule(s);
    pcnt_irq(s);
}

static const MemoryRegionOps pcnt_ops = {
    .read = pcnt_read, .write = pcnt_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4, .unaligned = false },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static void pcnt_gate(void *opaque, int line, int level)
{
    ESP32S3PcntState *s = opaque;
    pcnt_advance(s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    s->gate = level != 0;
    if (s->subscribed) {
        pcnt_inputs(s, s->last_ns);
    }
    pcnt_schedule(s);
}

static void pcnt_reset_held(void *opaque, int line, int level)
{
    ESP32S3PcntState *s = opaque;
    s->reset_held = level != 0;
    if (s->reset_held) {
        pcnt_reset(s);
    } else if (s->subscribed) {
        pcnt_inputs(s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    }
}

static void pcnt_clock(void *opaque, ClockEvent event)
{
    ESP32S3PcntState *s = opaque;
    pcnt_advance(s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    if (s->period) {
        s->cycle_remainder = (__uint128_t)s->cycle_remainder *
                            clock_get(s->apb_clk) / s->period;
    } else {
        s->cycle_remainder = 0;
    }
    s->period = clock_get(s->apb_clk);
    if (s->subscribed) {
        pcnt_inputs(s, s->last_ns);
    }
    pcnt_schedule(s);
}

static void pcnt_reset_hold(Object *obj, ResetType type)
{
    ESP32S3PcntState *s = ESP32S3_PCNT(obj);
    if (!s->soc_reset || esp32s3_reset_covers_periph(s->soc_reset)) {
        pcnt_reset(s);
    }
}

static void pcnt_realize(DeviceState *dev, Error **errp)
{
    ESP32S3PcntState *s = ESP32S3_PCNT(dev);
    if (!s->gpio || !s->electrical || !clock_has_source(s->apb_clk)) {
        error_setg(errp, "PCNT requires GPIO, electrical and connected apb-clk");
        return;
    }
    s->period = clock_get(s->apb_clk);
    pcnt_reset(s);
    if (!esp32s3_electrical_subscribe(s->electrical, pcnt_inputs, s)) {
        error_setg(errp, "PCNT electrical observer capacity exhausted");
        return;
    }
    s->subscribed = true;
    pcnt_inputs(s, s->last_ns);
}

static void pcnt_unrealize(DeviceState *dev)
{
    ESP32S3PcntState *s = ESP32S3_PCNT(dev);
    if (s->subscribed) {
        esp32s3_electrical_unsubscribe(s->electrical, pcnt_inputs, s);
        s->subscribed = false;
    }
    timer_del(s->filter_timer);
    s->int_raw = s->int_ena = 0;
    pcnt_irq(s);
}

static void pcnt_finalize(Object *obj)
{
    timer_free(ESP32S3_PCNT(obj)->filter_timer);
}

static void pcnt_init(Object *obj)
{
    ESP32S3PcntState *s = ESP32S3_PCNT(obj);
    memory_region_init_io(&s->iomem, obj, &pcnt_ops, s, TYPE_ESP32S3_PCNT, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    s->filter_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, pcnt_timer, s);
    s->apb_clk = qdev_init_clock_in(DEVICE(obj), "apb-clk", pcnt_clock, s, ClockUpdate);
    qdev_init_gpio_in_named(DEVICE(obj), pcnt_gate, "clock-enable", 1);
    qdev_init_gpio_in_named(DEVICE(obj), pcnt_reset_held, "reset-held", 1);
}

static Property pcnt_properties[] = {
    DEFINE_PROP_LINK("gpio", ESP32S3PcntState, gpio, TYPE_ESP32S3_GPIO, ESP32S3GPIOState *),
    DEFINE_PROP_LINK("electrical", ESP32S3PcntState, electrical, TYPE_ESP32S3_ELECTRICAL, DeviceState *),
    DEFINE_PROP_LINK("soc-reset", ESP32S3PcntState, soc_reset, TYPE_DEVICE, DeviceState *),
    DEFINE_PROP_END_OF_LIST(),
};

static void pcnt_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = pcnt_realize;
    dc->unrealize = pcnt_unrealize;
    device_class_set_props(dc, pcnt_properties);
    RESETTABLE_CLASS(klass)->phases.hold = pcnt_reset_hold;
}

static const TypeInfo pcnt_info = {
    .name = TYPE_ESP32S3_PCNT,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ESP32S3PcntState),
    .instance_init = pcnt_init,
    .instance_finalize = pcnt_finalize,
    .class_init = pcnt_class_init,
};

static void pcnt_register_types(void)
{
    type_register_static(&pcnt_info);
}
type_init(pcnt_register_types)
