/* SPDX-License-Identifier: GPL-2.0-or-later */
/* ESP32-S3 TRM v1.8 chapter 35; pinned IDF 6.1 S3 ledc_reg/struct/LL.
 * Real comparator PWM, not an average-intensity LED surrogate. */
#include "qemu/osdep.h"
#include "hw/misc/esp32s3_ledc.h"
#include "hw/misc/esp32s3_electrical.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-clock.h"
#include "hw/xtensa/esp32s3_reset_domain.h"
#include "qapi/error.h"
#include "qemu/module.h"

#define B(n) (1U << (n))
#define NS_PER_CYCLE 1000000000ULL
#define TIMER_RESET B(23)
#define TIMER_PAUSE B(22)
#define TIMER_UPDATE B(25)
#define CH_UPDATE B(4)
#define DUTY_MASK 0x7ffffU

static uint32_t period(const ESP32S3LedcTimer *t)
{
    unsigned res = t->active & 15;
    return res && res <= 14 ? 1U << res : 0;
}

static uint32_t divider(const ESP32S3LedcTimer *t)
{
    return (t->active >> 4) & 0x3ffff;
}

static bool running(ESP32S3LedcState *s, ESP32S3LedcTimer *t)
{
    return s->gate && !s->reset_held && s->hz && period(t) &&
           divider(t) >= 256 && !(t->conf & (TIMER_RESET | TIMER_PAUSE));
}

static uint64_t selected_hz(ESP32S3LedcState *s)
{
    switch (s->conf & 3) {
    case 1: return clock_get_hz(s->apb_clk);
    case 2: return clock_get_hz(s->rc_fast_clk);
    case 3: return clock_get_hz(s->xtal_clk);
    default: return 0;
    }
}

/* Cost of n ref pulses, in source cycles. The fractional prescaler produces
 * A/A+1 integer source-cycle spacings, with persistent modulo-256 carry. */
static uint64_t pulse_cost(ESP32S3LedcTimer *t, uint32_t n)
{
    uint32_t div = divider(t);
    return (uint64_t)n * (div >> 8) +
           ((t->div_fraction + (uint64_t)n * (div & 255)) >> 8);
}

static void sample_width(ESP32S3LedcChannel *c)
{
    unsigned fraction = c->duty_fraction + (c->current_duty & 15);
    c->width = (c->current_duty >> 4) + (fraction >> 4);
    c->duty_fraction = fraction & 15;
}

static void channel_apply(ESP32S3LedcChannel *c)
{
    c->active0 = c->pending0;
    c->active_hpoint = c->pending_hpoint;
    c->update = false;
}

static void overflow(ESP32S3LedcState *s, unsigned index)
{
    ESP32S3LedcTimer *t = &s->timers[index];
    s->int_raw |= B(index);
    if (t->update) {
        t->active = t->pending;
        t->update = false;
    }
    for (unsigned i = 0; i < 8; ++i) {
        ESP32S3LedcChannel *c = &s->channels[i];
        if ((c->active0 & 3) != index) {
            continue;
        }
        if (c->update) {
            channel_apply(c);
        }
        if (c->active0 & B(15)) {
            if (++c->overflows >= (((c->conf0 >> 5) & 1023) + 1)) {
                c->overflows = 0;
                s->int_raw |= B(12 + i);
            }
        }
        if (c->fade) {
            unsigned cycle = MAX(1U, (c->fade_conf >> 10) & 1023);
            if (++c->fade_cycle >= cycle) {
                c->fade_cycle = 0;
                unsigned scale = (c->fade_conf & 1023) << 4;
                /* The 19-bit duty datapath retains its fractional nibble. */
                c->current_duty = (c->current_duty +
                    ((c->fade_conf & B(30)) ? scale : -scale)) & DUTY_MASK;
                if (!c->fade_left || !--c->fade_left) {
                    c->fade = false;
                    s->int_raw |= B(4 + i);
                }
            }
        }
        sample_width(c);
    }
}

/* Events are scheduled at comparator matches or counter wrap, so ordinary
 * virtual-time dispatch crosses no unseen output transitions. Fixed bounded
 * catch-up preserves residual phase if an external virtual-clock jump makes
 * dispatch late; it never allocates or enters an unbounded zero-time loop. */
static void synchronize(ESP32S3LedcState *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint64_t elapsed = now > s->last_ns ? now - s->last_ns : 0;
    s->last_ns = now;
    for (unsigned i = 0; i < 4; ++i) {
        ESP32S3LedcTimer *t = &s->timers[i];
        if (!running(s, t)) {
            continue;
        }
        t->source_phase += (__uint128_t)elapsed * s->hz;
        for (unsigned batch = 0; batch < 16 && running(s, t); ++batch) {
            uint32_t lo = 0, hi = period(t) - t->count;
            while (lo < hi) {
                uint32_t mid = lo + (hi - lo + 1) / 2;
                if ((__uint128_t)pulse_cost(t, mid) * NS_PER_CYCLE <=
                    t->source_phase) {
                    lo = mid;
                } else {
                    hi = mid - 1;
                }
            }
            if (!lo) {
                break;
            }
            t->source_phase -= (__uint128_t)pulse_cost(t, lo) * NS_PER_CYCLE;
            t->div_fraction = (t->div_fraction + lo * (divider(t) & 255)) & 255;
            t->count += lo;
            if (t->count == period(t)) {
                t->count = 0;
                overflow(s, i);
            } else {
                break;
            }
        }
    }
}

static void publish(ESP32S3LedcState *s)
{
    if (!s->realized) {
        return;
    }
    unsigned changed = 0;
    for (unsigned i = 0; i < 8; ++i) {
        ESP32S3LedcChannel *c = &s->channels[i];
        bool previous = c->level;
        ESP32S3LedcTimer *t = &s->timers[c->active0 & 3];
        unsigned p = period(t);
        if (!(c->active0 & B(2))) {
            c->level = !!(c->active0 & B(3));
        } else if (p && c->active_hpoint < p) {
            unsigned position = (t->count + p - c->active_hpoint) % p;
            c->level = position < c->width;
        }
        changed |= s->publish_all || previous != c->level ? B(i) : 0;
    }
    if (changed) {
        esp32s3_electrical_begin_update(s->electrical);
        for (unsigned i = 0; i < 8; ++i) {
            if (changed & B(i)) {
                esp32s3_electrical_set_matrix_drive(s->electrical,
                    ESP32S3_LEDC_MATRIX_BASE + i, true, s->channels[i].level, false);
            }
        }
        esp32s3_electrical_end_update(s->electrical);
    }
    s->publish_all = false;
    qemu_set_irq(s->irq, !!(s->int_raw & s->int_ena));
}

static uint32_t next_ticks(ESP32S3LedcState *s, unsigned index)
{
    ESP32S3LedcTimer *t = &s->timers[index];
    unsigned p = period(t), next = p - t->count;
    for (unsigned i = 0; i < 8; ++i) {
        ESP32S3LedcChannel *c = &s->channels[i];
        if ((c->active0 & 3) != index || !(c->active0 & B(2)) ||
            c->active_hpoint >= p || !c->width || c->width >= p) {
            continue;
        }
        unsigned hp = c->active_hpoint;
        unsigned lp = (hp + c->width) % p;
        if (hp > t->count) {
            next = MIN(next, hp - t->count);
        }
        if (lp > t->count) {
            next = MIN(next, lp - t->count);
        }
    }
    return next;
}

static void schedule(ESP32S3LedcState *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint64_t delay = UINT64_MAX;
    timer_del(s->event);
    for (unsigned i = 0; i < 4; ++i) {
        ESP32S3LedcTimer *t = &s->timers[i];
        if (!running(s, t)) {
            continue;
        }
        __uint128_t cost = (__uint128_t)pulse_cost(t, next_ticks(s, i)) *
                           NS_PER_CYCLE;
        __uint128_t remaining = cost > t->source_phase ? cost - t->source_phase : 0;
        uint64_t ns = (remaining + s->hz - 1) / s->hz;
        delay = MIN(delay, MAX(1ULL, ns));
    }
    if (delay != UINT64_MAX && delay <= INT64_MAX - now) {
        timer_mod(s->event, now + delay);
    }
}

static void event(void *opaque)
{
    ESP32S3LedcState *s = opaque;
    synchronize(s);
    schedule(s);
    publish(s);
}

static void update_channel(ESP32S3LedcState *s, unsigned index)
{
    ESP32S3LedcChannel *c = &s->channels[index];
    c->pending0 = c->conf0 & 0x800f;
    c->pending_hpoint = c->hpoint;
    c->update = true;
    if (c->fade_dirty) {
        c->fade_conf = c->conf1;
        c->current_duty = c->duty;
        c->fade_left = (c->conf1 >> 20) & 1023;
        c->fade_cycle = 0;
        c->fade = !!(c->conf1 & B(31));
        c->fade_dirty = false;
    }
    ESP32S3LedcTimer *t = &s->timers[c->active0 & 3];
    /* A reset/unconfigured timebase cannot overflow. Initial programming
     * therefore establishes its shadow state before the counter starts. */
    if (!period(t) || (t->conf & TIMER_RESET)) {
        channel_apply(c);
        sample_width(c);
    }
}

static uint64_t ledc_read(void *opaque, hwaddr addr, unsigned size)
{
    ESP32S3LedcState *s = opaque;
    synchronize(s);
    schedule(s);
    publish(s);
    if (!s->gate || s->reset_held) {
        return 0;
    }
    if (addr < 0xa0) {
        ESP32S3LedcChannel *c = &s->channels[addr / 20];
        switch (addr % 20) {
        case 0: return c->conf0;
        case 4: return c->hpoint;
        case 8: return c->duty;
        case 12: return c->conf1;
        case 16: return c->current_duty;
        }
    } else if (addr < 0xc0) {
        ESP32S3LedcTimer *t = &s->timers[(addr - 0xa0) / 8];
        return (addr & 4) ? t->count : t->conf;
    }
    switch (addr) {
    case 0xc0: return s->int_raw;
    case 0xc4: return s->int_raw & s->int_ena;
    case 0xc8: return s->int_ena;
    case 0xd0: return s->conf;
    case 0xfc: return s->date;
    default: return 0;
    }
}

static void ledc_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    ESP32S3LedcState *s = opaque;
    uint32_t v = value;
    if (!s->gate || s->reset_held) {
        return;
    }
    synchronize(s);
    if (addr < 0xa0) {
        unsigned i = addr / 20;
        ESP32S3LedcChannel *c = &s->channels[i];
        switch (addr % 20) {
        case 0:
            c->conf0 = v & 0xffef;
            c->active0 = (c->active0 & ~B(3)) | (v & B(3));
            if (v & B(16)) {
                c->overflows = 0;
            }
            if (v & CH_UPDATE) {
                update_channel(s, i);
            }
            break;
        case 4: c->hpoint = v & 0x3fff; break;
        case 8: c->duty = v & DUTY_MASK; c->fade_dirty = true; break;
        case 12: c->conf1 = v; c->fade_dirty = true; break;
        default: break;
        }
    } else if (addr < 0xc0 && !(addr & 4)) {
        ESP32S3LedcTimer *t = &s->timers[(addr - 0xa0) / 8];
        t->conf = v & 0x00ffffff; /* bit24 is reserved on S3, not REF_TICK. */
        t->conf &= ~B(24);
        if (v & TIMER_UPDATE) {
            t->pending = v & 0x3fffff;
            t->update = true;
            if (!period(t) || (v & TIMER_RESET)) {
                t->active = t->pending;
                t->update = false;
            }
        }
        if (v & TIMER_RESET) {
            t->count = 0;
            t->source_phase = 0;
            t->div_fraction = 0;
        }
    } else {
        switch (addr) {
        case 0xc8: s->int_ena = v & 0xfffff; break;
        case 0xcc: s->int_raw &= ~(v & 0xfffff); break;
        case 0xd0: s->conf = v & 0x80000003; s->hz = selected_hz(s); break;
        case 0xfc: s->date = v; break;
        default: break;
        }
    }
    schedule(s);
    publish(s);
}

static const MemoryRegionOps ledc_ops = {
    .read = ledc_read, .write = ledc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4, .unaligned = false },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static void ledc_reset(DeviceState *dev)
{
    ESP32S3LedcState *s = ESP32S3_LEDC(dev);
    timer_del(s->event);
    memset(s->timers, 0, sizeof(s->timers));
    memset(s->channels, 0, sizeof(s->channels));
    for (unsigned i = 0; i < 4; ++i) {
        s->timers[i].conf = TIMER_RESET;
    }
    for (unsigned i = 0; i < 8; ++i) {
        s->channels[i].conf1 = B(30);
    }
    s->conf = s->int_raw = s->int_ena = 0;
    s->date = 0x19040200;
    s->hz = 0;
    s->last_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    qemu_set_irq(s->irq, 0);
    s->publish_all = true;
    publish(s);
}

static void ledc_reset_hold(Object *obj, ResetType type)
{
    ESP32S3LedcState *s = ESP32S3_LEDC(obj);
    if (!s->soc_reset || esp32s3_reset_covers_periph(s->soc_reset)) {
        ledc_reset(DEVICE(obj));
    }
}

static void clock_changed(void *opaque, ClockEvent event_type)
{
    ESP32S3LedcState *s = opaque;
    if (event_type == ClockUpdate) {
        synchronize(s); /* uses old cached frequency before selecting new */
        s->hz = selected_hz(s);
        schedule(s);
        publish(s);
    }
}

static void gate_changed(void *opaque, int n, int level)
{
    ESP32S3LedcState *s = opaque;
    synchronize(s);
    s->gate = !!level;
    schedule(s);
    publish(s);
}

static void reset_changed(void *opaque, int n, int level)
{
    ESP32S3LedcState *s = opaque;
    synchronize(s);
    if (level && !s->reset_held) {
        ledc_reset(DEVICE(s));
    }
    s->reset_held = !!level;
    schedule(s);
    publish(s);
}

static void ledc_realize(DeviceState *dev, Error **errp)
{
    ESP32S3LedcState *s = ESP32S3_LEDC(dev);
    if (!s->gpio || !s->electrical) {
        error_setg(errp, "LEDC requires GPIO and authoritative electrical links");
        return;
    }
    s->realized = true;
    ledc_reset(dev);
}

static void ledc_unrealize(DeviceState *dev)
{
    ESP32S3LedcState *s = ESP32S3_LEDC(dev);
    s->realized = false;
    timer_del(s->event);
    esp32s3_electrical_begin_update(s->electrical);
    for (unsigned i = 0; i < 8; ++i) {
        esp32s3_electrical_set_matrix_drive(s->electrical,
            ESP32S3_LEDC_MATRIX_BASE + i, false, false, false);
    }
    esp32s3_electrical_end_update(s->electrical);
    qemu_set_irq(s->irq, 0);
}

static void ledc_init(Object *obj)
{
    ESP32S3LedcState *s = ESP32S3_LEDC(obj);
    memory_region_init_io(&s->iomem, obj, &ledc_ops, s, TYPE_ESP32S3_LEDC,
                          ESP32S3_LEDC_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    s->event = timer_new_ns(QEMU_CLOCK_VIRTUAL, event, s);
    s->gate = true;
    s->apb_clk = qdev_init_clock_in(DEVICE(obj), "apb-clk", clock_changed, s, ClockUpdate);
    s->xtal_clk = qdev_init_clock_in(DEVICE(obj), "xtal-clk", clock_changed, s, ClockUpdate);
    s->rc_fast_clk = qdev_init_clock_in(DEVICE(obj), "rc-fast-clk", clock_changed, s, ClockUpdate);
    qdev_init_gpio_in_named(DEVICE(obj), gate_changed, "clock-enable", 1);
    qdev_init_gpio_in_named(DEVICE(obj), reset_changed, "reset-held", 1);
}

static void ledc_finalize(Object *obj)
{
    timer_free(ESP32S3_LEDC(obj)->event);
}

static Property ledc_properties[] = {
    DEFINE_PROP_LINK("gpio", ESP32S3LedcState, gpio, TYPE_ESP32S3_GPIO, ESP32S3GPIOState *),
    DEFINE_PROP_LINK("electrical", ESP32S3LedcState, electrical, TYPE_ESP32S3_ELECTRICAL, DeviceState *),
    DEFINE_PROP_LINK("soc-reset", ESP32S3LedcState, soc_reset, TYPE_DEVICE, DeviceState *),
    DEFINE_PROP_END_OF_LIST(),
};

static void ledc_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = ledc_realize;
    dc->unrealize = ledc_unrealize;
    RESETTABLE_CLASS(klass)->phases.hold = ledc_reset_hold;
    device_class_set_props(dc, ledc_properties);
}

static const TypeInfo ledc_info = {
    .name = TYPE_ESP32S3_LEDC, .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ESP32S3LedcState), .instance_init = ledc_init,
    .instance_finalize = ledc_finalize, .class_init = ledc_class_init,
};

static void ledc_register_types(void)
{
    type_register_static(&ledc_info);
}
type_init(ledc_register_types)
