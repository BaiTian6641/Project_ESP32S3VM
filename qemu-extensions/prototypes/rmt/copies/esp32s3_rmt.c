/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Native ESP32-S3 RMT waveform engine.
 * Register contract: pinned IDF 6.1 rmt_struct.h/rmt_ll.h, TRM RMT chapter.
 * Every TX transition enters the GPIO matrix; every RX transition comes from
 * the authoritative resolved GPIO input subscription. There is no TX/RX copy.
 * Timing is functional modeled timing, not physical cycle metrology.
 */
#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qemu/log.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-clock.h"
#include "hw/timer/esp32s3_rmt.h"
#include "hw/xtensa/esp32s3_reset_domain.h"
#include "qemu/bswap.h"
#include "trace.h"

#define TX_DONE(n) BIT(n)
#define TX_ERROR(n) BIT(4 + (n))
#define TX_THRESHOLD(n) BIT(8 + (n))
#define TX_LOOP(n) BIT(12 + (n))
#define RX_DONE(n) BIT(16 + (n))
#define RX_ERROR(n) BIT(20 + (n))
#define RX_THRESHOLD(n) BIT(24 + (n))
#define TX_DMA_ERROR BIT(28)
#define RX_DMA_ERROR BIT(29)
#define IRQ_MASK 0x3fffffffU
#define TX_STROBES (BIT(0) | BIT(1) | BIT(2) | BIT(23) | BIT(24))
#define RX_STROBES (BIT(1) | BIT(2) | BIT(14) | BIT(15))

static __uint128_t now_fp(void)
{
    return (__uint128_t)qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) << 32;
}

static int64_t deadline_ns(__uint128_t time)
{
    return MIN((time + 0xffffffffU) >> 32, INT64_MAX);
}

static void irq_update(ESP32S3RmtState *s)
{
    qemu_set_irq(s->irq, (s->int_raw & s->int_ena) != 0);
}

static void raise_event(ESP32S3RmtState *s, uint32_t mask)
{
    s->int_raw |= mask;
    irq_update(s);
}

/* The group divider is 1+NUM+A/B; the channel divider zero means 256. */
static uint64_t group_period(ESP32S3RmtState *s)
{
    unsigned sel = (s->sys_conf >> 24) & 3;
    unsigned a = (s->sys_conf >> 12) & 63;
    unsigned b = (s->sys_conf >> 18) & 63;
    unsigned n = ((s->sys_conf >> 4) & 255) + 1;
    Clock *clk = sel == 1 ? s->apb_clk : sel == 2 ? s->rc_fast_clk : s->xtal_clk;
    __uint128_t period;
    if (!s->gate || s->reset_held || !(s->sys_conf & BIT(26)) ||
        (s->sys_conf & BIT(2)) || !sel) {
        return 0;
    }
    period = clock_get(clk);
    if (!period || (a && !b)) {
        return 0;
    }
    period = b ? period * (n * b + a) / b : period * n;
    return period <= UINT64_MAX ? period : 0;
}

static uint64_t channel_period(ESP32S3RmtState *s, unsigned div)
{
    __uint128_t period = (__uint128_t)group_period(s) * (div ? div : 256);
    return period <= UINT64_MAX ? period : 0;
}

static unsigned tx_capacity(ESP32S3RmtTx *t)
{
    unsigned blocks = (t->applied >> 16) & 15;
    return blocks <= 4 - t->index ? blocks * ESP32S3_RMT_SYMBOLS : 0;
}

static unsigned rx_capacity(ESP32S3RmtRx *r)
{
    unsigned blocks = (r->applied0 >> 24) & 15;
    return blocks <= 4 - r->index ? blocks * ESP32S3_RMT_SYMBOLS : 0;
}

static bool tx_output(ESP32S3RmtTx *t, bool enabled, bool level)
{
    ESP32S3RmtState *s = t->parent;
    if (!esp32s3_electrical_set_matrix_drive(s->electrical, 81 + t->index,
                                           enabled, level, false)) {
        timer_del(t->timer);
        t->active = t->armed = t->idle_carrier = t->carrier_running = false;
        if (enabled) {
            raise_event(s, TX_ERROR(t->index));
        }
        return false;
    }
    trace_esp32s3_rmt_edge(t->index, enabled, level, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    return true;
}

static void tx_schedule(ESP32S3RmtTx *t)
{
    __uint128_t next = t->deadline;
    if (!t->active && !t->idle_carrier) {
        return;
    }
    if (t->carrier_running) {
        next = MIN(next, t->carrier_deadline);
    }
    timer_mod(t->timer, deadline_ns(next));
}

static bool tx_carrier_start(ESP32S3RmtTx *t, __uint128_t at);

static bool tx_idle(ESP32S3RmtTx *t, bool marker_level)
{
    bool level = marker_level;
    if (t->applied & BIT(6)) {
        level = (t->applied & BIT(5)) != 0;
    } else if ((t->applied & BIT(3)) && t->parent->silicon_revision <= 2) {
        /* RMT-176: loop idle comes from wrapped data, not the end marker. */
        level = (t->parent->ram[t->index * ESP32S3_RMT_SYMBOLS] & BIT(15)) != 0;
        qemu_log_mask(LOG_GUEST_ERROR, "RMT channel %u: RMT-176 loop idle workaround not enabled\n", t->index);
    }
    t->level = level;
    t->idle_carrier = (t->applied & BIT(21)) && !(t->applied & BIT(20)) &&
                      level == ((t->applied & BIT(22)) != 0) && group_period(t->parent);
    if (t->idle_carrier) {
        t->deadline = (__uint128_t)INT64_MAX << 32;
        if (!tx_carrier_start(t, now_fp())) {
            return false;
        }
        tx_schedule(t);
    } else {
        t->carrier_running = false;
        timer_del(t->timer);
        return tx_output(t, true, level);
    }
    return true;
}

static bool tx_cancel(ESP32S3RmtTx *t, bool release)
{
    if (t->active || t->armed) {
        trace_esp32s3_rmt_tx_stop(t->index, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    }
    timer_del(t->timer);
    t->active = t->armed = t->idle_carrier = false;
    t->word_pending = false;
    t->carrier_running = !release && t->carrier_running &&
                         !(t->applied & BIT(20));
    t->status &= ~(7U << 22);
    if (release) {
        return tx_output(t, false, t->level);
    } else {
        return tx_idle(t, t->level);
    }
}

static void tx_error(ESP32S3RmtTx *t, uint32_t extra)
{
    tx_cancel(t, true);
    raise_event(t->parent, TX_ERROR(t->index) | extra);
}

static bool dma_binding(ESP32S3RmtState *s, unsigned direction,
                        bool *bound, uint32_t *channel)
{
    if (!s->gdma) {
        return false;
    }
    if (!*bound) {
        if (!esp_gdma_get_channel_periph(s->gdma, GDMA_RMT, direction, channel)) {
            return false;
        }
        *bound = true;
    }
    /* Do not jump to another channel when a bound descriptor is stopped,
     * reset or reassigned. The byte pump owns cursor/start validation. EOF
     * finalization also accepts an already-full, legitimately parked link. */
    return (esp_gdma_read_chan_register(s->gdma, direction, *channel,
                                       GDMA_PERI_SEL_REG) & 63) == GDMA_RMT;
}

static bool tx_fetch(ESP32S3RmtTx *t)
{
    ESP32S3RmtState *s = t->parent;
    unsigned capacity = tx_capacity(t);
    if (t->applied & BIT(25)) {
        uint8_t bytes[4];
        if (t->index != 3 ||
            !dma_binding(s, ESP_GDMA_OUT_IDX, &t->dma_bound, &t->dma_channel) ||
            !esp_gdma_read_channel(s->gdma, t->dma_channel, bytes, sizeof(bytes))) {
            tx_error(t, TX_DMA_ERROR);
            return false;
        }
        t->symbol = ldl_le_p(bytes);
    } else {
        if (!capacity) {
            tx_error(t, 0);
            return false;
        }
        if (t->cursor >= capacity) {
            if (!(t->applied & BIT(4))) {
                t->status |= BIT(25);
                tx_error(t, 0);
                return false;
            }
            t->cursor = 0;
        }
        t->symbol = s->ram[t->index * ESP32S3_RMT_SYMBOLS + t->cursor];
    }
    return true;
}

static bool tx_carrier_start(ESP32S3RmtTx *t, __uint128_t at)
{
    bool selected = t->level == ((t->applied & BIT(22)) != 0);
    bool running = (t->applied & BIT(21)) && selected;
    bool continuing = running && t->carrier_running;
    t->carrier_running = running;
    t->carrier_period = group_period(t->parent);
    if (running) {
        if (!continuing) {
            unsigned high = t->carrier >> 16;
            t->carrier_level = true;
            t->carrier_deadline = at + (__uint128_t)t->carrier_period * (high ? high : 65536);
        }
        return tx_output(t, true, t->carrier_level);
    } else {
        return tx_output(t, true, t->level);
    }
}

static void tx_complete_word(ESP32S3RmtTx *t)
{
    ++t->cursor;
    unsigned limit = t->limit & 511;
    if (limit && ++t->threshold_count >= limit) {
        t->threshold_count = 0;
        raise_event(t->parent, TX_THRESHOLD(t->index));
    }
    t->status = (t->status & ~0x3ffU) |
                ((t->index * ESP32S3_RMT_SYMBOLS + t->cursor) & 0x3ff);
}

/* One half-symbol or carrier transition per callback. Even a continuous ring
 * always returns to the main loop; zero-length loop markers are errors. */
static void tx_step(ESP32S3RmtTx *t, __uint128_t at)
{
    ESP32S3RmtState *s = t->parent;
    uint16_t half;
    /* TX_LIM counts data entries sent, not entries whose second half merely
     * started. Keep the current symbol latched until its full duration ends. */
    if (t->word_pending) {
        t->word_pending = false;
        tx_complete_word(t);
    }
    if (!t->half && !tx_fetch(t)) {
        return;
    }
    half = t->half ? t->symbol >> 16 : t->symbol;
    if (!(half & 0x7fff)) {
        if (t->half) {
            /* A terminal zero second half closes the already-emitted first. */
            tx_complete_word(t);
        }
        if (t->applied & BIT(3)) {
            unsigned count = (t->limit >> 9) & 1023;
            if (!t->cursor && !t->half) {
                tx_error(t, 0);
                return;
            }
            ++t->loops;
            if ((t->limit & BIT(19)) && count && t->loops >= count) {
                if (t->limit & BIT(21)) {
                    t->level = (half & BIT(15)) != 0;
                    if (tx_cancel(t, false)) {
                        raise_event(s, TX_LOOP(t->index));
                    }
                    return;
                }
                raise_event(s, TX_LOOP(t->index));
                t->loops = 0;
            }
            t->cursor = t->half = 0;
            /* At most one restart fetch in this callback; an all-marker loop
             * is rejected instead of creating zero-time timer livelock. */
            if (!tx_fetch(t)) {
                return;
            }
            half = t->symbol;
            if (!(half & 0x7fff)) {
                tx_error(t, 0);
                return;
            }
        } else {
            t->level = (half & BIT(15)) != 0;
            if (tx_cancel(t, false)) {
                raise_event(s, TX_DONE(t->index));
            }
            return;
        }
    }
    t->level = (half & BIT(15)) != 0;
    t->deadline = at + (__uint128_t)(half & 0x7fff) * t->period;
    if (!tx_carrier_start(t, at)) {
        return;
    }
    t->half ^= 1;
    t->word_pending = !t->half;
    t->status = (t->status & ~0x3ffU) |
                ((t->index * ESP32S3_RMT_SYMBOLS + t->cursor) & 0x3ff);
    tx_schedule(t);
}

static void tx_timer(void *opaque)
{
    ESP32S3RmtTx *t = opaque;
    __uint128_t now = now_fp();
    if ((!t->active && !t->idle_carrier) || !group_period(t->parent)) {
        return;
    }
    if (t->active && t->deadline <= now) {
        tx_step(t, t->deadline);
    } else if (t->carrier_running && t->carrier_deadline <= now) {
        __uint128_t at = t->carrier_deadline;
        t->carrier_level = !t->carrier_level;
        unsigned ticks = t->carrier_level ? t->carrier >> 16 : t->carrier & 0xffff;
        t->carrier_deadline = at + (__uint128_t)t->carrier_period * (ticks ? ticks : 65536);
        tx_output(t, true, t->carrier_level);
        tx_schedule(t);
    } else {
        tx_schedule(t);
    }
}

static void tx_start(ESP32S3RmtTx *t, __uint128_t at)
{
    t->period = channel_period(t->parent, (t->applied >> 8) & 255);
    t->armed = false;
    t->word_pending = false;
    t->dma_bound = false;
    if (!t->period) {
        tx_error(t, 0);
        return;
    }
    t->active = true;
    t->idle_carrier = false;
    trace_esp32s3_rmt_tx_start(t->index, deadline_ns(at));
    t->half = 0;
    t->status = (t->status & ~(7U << 22)) | BIT(22);
    t->deadline = at;
    /* Start itself produces the first edge, never a completion interrupt. */
    tx_step(t, at);
}

static void rx_cancel(ESP32S3RmtRx *r)
{
    timer_del(r->idle_timer);
    timer_del(r->filter_timer);
    r->active = r->candidate = false;
    r->status &= ~(7U << 22);
}

static void rx_error(ESP32S3RmtRx *r, uint32_t status, uint32_t extra)
{
    r->status |= status;
    rx_cancel(r);
    raise_event(r->parent, RX_ERROR(r->index) | extra);
}

static bool rx_store(ESP32S3RmtRx *r, unsigned ticks, bool level)
{
    ESP32S3RmtState *s = r->parent;
    unsigned capacity = rx_capacity(r);
    unsigned base = (4 + r->index) * ESP32S3_RMT_SYMBOLS;
    uint16_t half = MIN(ticks, 32767) | (level ? BIT(15) : 0);
    if (!(r->applied1 & BIT(3))) {
        rx_error(r, BIT(25), 0);
        return false;
    }
    if (!capacity || r->cursor >= capacity) {
        if (capacity && (r->applied1 & BIT(13))) {
            r->cursor = 0;
        } else {
            rx_error(r, BIT(26), 0);
            return false;
        }
    }
    if (!r->half) {
        s->ram[base + r->cursor] = half;
        r->half = 1;
    } else {
        s->ram[base + r->cursor] |= (uint32_t)half << 16;
        r->half = 0;
    }
    /* A zero-duration marker terminates even a half-full word. */
    if (!r->half || !ticks) {
        uint32_t word = s->ram[base + r->cursor];
        trace_esp32s3_rmt_rx_symbol(r->index, r->cursor, word,
                                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
        if (r->applied0 & BIT(23)) {
            uint8_t bytes[4];
            stl_le_p(bytes, word);
            if (r->index != 3 ||
                !dma_binding(s, ESP_GDMA_IN_IDX, &r->dma_bound, &r->dma_channel) ||
                !esp_gdma_write_channel(s->gdma, r->dma_channel, bytes, sizeof(bytes))) {
                rx_error(r, 0, RX_DMA_ERROR);
                return false;
            }
        }
        r->half = 0;
        ++r->cursor;
        unsigned limit = r->limit & 511;
        if (limit && ++r->threshold_count >= limit) {
            r->threshold_count = 0;
            raise_event(s, RX_THRESHOLD(r->index));
        }
    }
    r->status = (r->status & ~0x3ffU) | ((base + r->cursor) & 0x3ff);
    return true;
}

static __uint128_t rx_counter_fp(ESP32S3RmtRx *r, int64_t ns)
{
    if (!r->period || ns <= r->counter_ns) {
        return r->elapsed_ticks_fp;
    }
    return r->elapsed_ticks_fp +
           (((__uint128_t)(ns - r->counter_ns) << 64) / r->period);
}

static unsigned rx_ticks(ESP32S3RmtRx *r, int64_t ns)
{
    /* Recover integral vectors across the less-than-one-nanosecond QEMU
     * boundary allowance, without shifting an edge before virtual epoch zero. */
    __uint128_t allowance = r->period ?
                            ((__uint128_t)0xffffffffU << 32) / r->period : 0;
    return MIN((rx_counter_fp(r, ns) + allowance) >> 32, 32767);
}

static void rx_schedule_idle(ESP32S3RmtRx *r)
{
    unsigned idle = (r->applied0 >> 8) & 32767;
    if (r->active && r->started && r->valid && r->period) {
        int64_t ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        __uint128_t target = (__uint128_t)(idle + 1) << 32;
        __uint128_t count = rx_counter_fp(r, ns);
        __uint128_t remaining = target > count ? target - count : 0;
        timer_mod(r->idle_timer, deadline_ns(((__uint128_t)ns << 32) +
                                           ((remaining * r->period) >> 32)));
    }
}

static void rx_accept(ESP32S3RmtRx *r, bool level, int64_t ns)
{
    if (!r->active || !r->valid || level == r->level) {
        return;
    }
    if (r->started && !rx_store(r, rx_ticks(r, ns), r->level)) {
        return;
    }
    r->started = true;
    r->level = level;
    r->counter_ns = ns;
    r->elapsed_ticks_fp = 0;
    rx_schedule_idle(r);
}

static void rx_filter_timer(void *opaque)
{
    ESP32S3RmtRx *r = opaque;
    if (r->candidate && r->active && r->valid) {
        r->candidate = false;
        rx_accept(r, r->candidate_level, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    }
}

static void rx_input(void *opaque, bool valid, bool level, int64_t ns)
{
    ESP32S3RmtRx *r = opaque;
    trace_esp32s3_rmt_input(r->index, valid, level, ns);
    bool previous_valid = r->valid;
    r->valid = valid;
    if (!valid) {
        /* Unknown is not an edge and cannot complete a receive transaction. */
        timer_del(r->idle_timer);
        timer_del(r->filter_timer);
        r->candidate = false;
        return;
    }
    if (!r->active || !previous_valid) {
        r->level = level;
        r->counter_ns = ns;
        r->elapsed_ticks_fp = 0;
        if (r->active) {
            rx_schedule_idle(r);
        }
        return;
    }
    if (r->applied1 & BIT(4)) {
        /* Source and filter expiry at equal timestamps may be dispatched in
         * either timer order. A pulse exactly at threshold is not noise. */
        if (r->candidate && ns >= r->qualify_ns) {
            rx_filter_timer(r);
            if (!r->active) {
                return;
            }
        }
        if (level == r->level) {
            r->candidate = false;
            timer_del(r->filter_timer);
            return;
        }
        if (r->candidate && r->candidate_level == level) {
            return;
        }
        r->candidate = true;
        r->candidate_level = level;
        unsigned ticks = (r->applied1 & BIT(4)) ? (r->applied1 >> 5) & 255 : 0;
        r->filter_period = group_period(r->parent);
        __uint128_t delay = (__uint128_t)ticks * r->filter_period;
        /* S3 filter forwards a qualified level after stable group-clock ticks. */
        r->qualify_deadline_fp = ((__uint128_t)ns << 32) + delay;
        r->qualify_ns = deadline_ns(r->qualify_deadline_fp);
        timer_mod(r->filter_timer, r->qualify_ns);
    } else {
        rx_accept(r, level, ns);
    }
}

/* Physical post-settle observer. The graph owns connectivity/voltage; GPIO
 * owns matrix routing and sampled inversion. Never infer an RX pad from TX. */
static void rmt_inputs_changed(void *opaque, uint64_t sample_ns)
{
    ESP32S3RmtState *s = opaque;
    for (unsigned i = 0; i < 4; ++i) {
        unsigned signal = 81 + i;
        unsigned cfg = s->gpio->func_in_sel_cfg[signal];
        unsigned pad = cfg & 63;
        bool level = false;
        bool valid = (cfg & BIT(7)) != 0;
        if (valid && pad != GPIO_FUNC_IN_HIGH && pad != GPIO_FUNC_IN_LOW) {
            /* The canonical checked matrix sample below owns effective input
             * enable (awake/sleep/hold); do not duplicate it from raw FUN_IE. */
            valid = pad < ESP32S3_GPIO_COUNT &&
                    esp32s3_gpio_net_valid(s->gpio, pad);
        }
        valid = valid && esp32s3_gpio_matrix_sample(s->gpio, signal, &level);
        if (valid != s->rx[i].valid || (valid && level != s->rx[i].level) ||
            s->rx[i].candidate) {
            rx_input(&s->rx[i], valid, level, sample_ns);
        }
    }
}

static void rx_idle_timer(void *opaque)
{
    ESP32S3RmtRx *r = opaque;
    if (!r->active || !r->valid) {
        return;
    }
    /* The terminal idle half is a zero-duration end marker. Counting starts
     * on the first input edge, so a disconnected stable line never completes. */
    if (!rx_store(r, 0, r->level)) {
        return;
    }
    rx_cancel(r);
    r->conf1 &= ~BIT(3);
    r->applied1 &= ~BIT(3);
    if (r->applied0 & BIT(23)) {
        if (!r->dma_bound ||
            !dma_binding(r->parent, ESP_GDMA_IN_IDX, &r->dma_bound, &r->dma_channel) ||
            !esp_gdma_finish_rx_channel(r->parent->gdma, r->dma_channel)) {
            rx_error(r, 0, RX_DMA_ERROR);
            return;
        }
    }
    raise_event(r->parent, RX_DONE(r->index));
}

static void rx_start(ESP32S3RmtRx *r)
{
    rx_cancel(r);
    /* Registration/route/IE/reset can change the physical baseline without an
     * edge notification. Prime actual current routing while inactive, never
     * synthesize a leading pulse or obtain input from the TX symbol cache. */
    rmt_inputs_changed(r->parent, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    r->dma_bound = false;
    r->period = channel_period(r->parent, r->applied0 & 255);
    if (!r->period || !rx_capacity(r)) {
        rx_error(r, 0, 0);
        return;
    }
    if (r->applied0 & BIT(28)) {
        rx_error(r, 0, 0);
        qemu_log_mask(LOG_UNIMP, "RMT RX demodulation envelope timing is unsupported\n");
        return;
    }
    if (!(r->applied1 & BIT(3))) {
        rx_error(r, BIT(25), 0);
        return;
    }
    r->active = true;
    r->started = false;
    r->counter_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    r->elapsed_ticks_fp = 0;
    r->status = (r->status & ~(7U << 22)) | BIT(22);
    rx_schedule_idle(r);
}

static void refresh_clocks(ESP32S3RmtState *s)
{
    __uint128_t now = now_fp();
    if (!group_period(s)) {
        /* Cancel every receiver before releasing any transmitter: a release
         * settles the physical graph synchronously and can otherwise pump
         * another RX channel's old DMA cursor during the gate/reset write. */
        for (unsigned i = 0; i < 4; ++i) {
            if (s->rx[i].active) {
                rx_cancel(&s->rx[i]);
                s->int_raw &= ~(RX_DONE(i) | RX_ERROR(i) | RX_THRESHOLD(i));
            }
        }
    }
    for (unsigned i = 0; i < 4; ++i) {
        ESP32S3RmtTx *t = &s->tx[i];
        ESP32S3RmtRx *r = &s->rx[i];
        uint64_t period = channel_period(s, (t->applied >> 8) & 255);
        if (t->active || t->idle_carrier || t->armed) {
            if (!period) {
                tx_cancel(t, true);
                s->int_raw &= ~(TX_DONE(i) | TX_ERROR(i) | TX_THRESHOLD(i) | TX_LOOP(i));
            } else {
                if (t->active && t->deadline > now && t->period) {
                    t->deadline = now + (t->deadline - now) * period / t->period;
                }
                if (t->carrier_running && t->carrier_deadline > now && t->carrier_period) {
                    t->carrier_deadline = now + (t->carrier_deadline - now) * group_period(s) / t->carrier_period;
                }
                t->period = period;
                t->carrier_period = group_period(s);
                tx_schedule(t);
            }
        }
        period = channel_period(s, r->applied0 & 255);
        if (r->active) {
            if (!period) {
                rx_cancel(r);
                s->int_raw &= ~(RX_DONE(i) | RX_ERROR(i) | RX_THRESHOLD(i));
            } else {
                /* Preserve accumulated ticks, not a synthetic negative edge.
                 * A divider increase can require more pre-epoch equivalent
                 * nanoseconds than exist; a tick counter has no such limit. */
                int64_t ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
                r->elapsed_ticks_fp = rx_counter_fp(r, ns);
                r->counter_ns = ns;
                if (r->candidate && r->filter_period) {
                    __uint128_t at = (__uint128_t)ns << 32;
                    __uint128_t left = r->qualify_deadline_fp > at ?
                                       r->qualify_deadline_fp - at : 0;
                    uint64_t filter_period = group_period(s);
                    r->qualify_deadline_fp = at + left * filter_period / r->filter_period;
                    r->filter_period = filter_period;
                    r->qualify_ns = deadline_ns(r->qualify_deadline_fp);
                    timer_mod(r->filter_timer, r->qualify_ns);
                }
                r->period = period;
                rx_schedule_idle(r);
            }
        }
    }
    irq_update(s);
}

static void clock_changed(void *opaque, ClockEvent event)
{
    if (event == ClockUpdate) {
        refresh_clocks(opaque);
    }
}

static void gate_changed(void *opaque, int line, int level)
{
    ESP32S3RmtState *s = opaque;
    s->gate = level != 0;
    refresh_clocks(s);
}

static void reset_held_changed(void *opaque, int line, int level)
{
    ESP32S3RmtState *s = opaque;
    s->reset_held = level != 0;
    refresh_clocks(s);
    if (s->reset_held) {
        s->int_raw = 0;
        irq_update(s);
    }
}

static ESP32S3RmtRx *rx_ram_owner(ESP32S3RmtState *s, unsigned word)
{
    for (unsigned i = 0; i < 4; ++i) {
        ESP32S3RmtRx *r = &s->rx[i];
        unsigned base = (4 + i) * ESP32S3_RMT_SYMBOLS;
        if (r->active && (r->applied1 & BIT(3)) &&
            word >= base && word - base < rx_capacity(r)) {
            return r;
        }
    }
    return NULL;
}

static uint64_t rmt_read(void *opaque, hwaddr addr, unsigned size)
{
    ESP32S3RmtState *s = opaque;
    unsigned i;
    if (addr >= 0x800 && addr < 0xe00) {
        i = (addr - 0x800) / 4;
        ESP32S3RmtRx *r = rx_ram_owner(s, i);
        if (r) {
            r->status |= BIT(27);
            raise_event(s, RX_ERROR(r->index));
            return 0;
        }
        return s->ram[i];
    }
    if (addr < 0x10) {
        return 0;
    }
    if (addr < 0x20) {
        ESP32S3RmtRx *r = &s->rx[(addr - 0x10) / 4];
        if ((s->sys_conf & BIT(0)) || r->fifo >= rx_capacity(r) || (r->applied1 & BIT(3))) {
            r->status |= BIT(27);
            raise_event(s, RX_ERROR(r->index));
            return 0;
        }
        return s->ram[(4 + r->index) * 48 + r->fifo++];
    }
    if (addr < 0x30) {
        return s->tx[(addr - 0x20) / 4].conf;
    }
    if (addr < 0x50) {
        ESP32S3RmtRx *r = &s->rx[(addr - 0x30) / 8];
        return addr & 4 ? r->conf1 : r->conf0;
    }
    if (addr < 0x60) {
        ESP32S3RmtTx *t = &s->tx[(addr - 0x50) / 4];
        return (t->status & ~(0x3ffU << 11)) | ((t->index * 48 + t->fifo) << 11);
    }
    if (addr < 0x70) {
        ESP32S3RmtRx *r = &s->rx[(addr - 0x60) / 4];
        return (r->status & ~(0x3ffU << 11)) | (((4 + r->index) * 48 + r->fifo) << 11);
    }
    if (addr >= 0x80 && addr < 0x90) return s->tx[(addr - 0x80) / 4].carrier;
    if (addr >= 0x90 && addr < 0xa0) return s->rx[(addr - 0x90) / 4].carrier;
    if (addr >= 0xa0 && addr < 0xb0) return s->tx[(addr - 0xa0) / 4].limit;
    if (addr >= 0xb0 && addr < 0xc0) return s->rx[(addr - 0xb0) / 4].limit;
    switch (addr) {
    case 0x70: return s->int_raw;
    case 0x74: return s->int_raw & s->int_ena;
    case 0x78: return s->int_ena;
    case 0xc0: return s->sys_conf;
    case 0xc4: return s->tx_sim;
    case 0xcc: return s->date;
    default: return 0;
    }
}

static void rmt_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    ESP32S3RmtState *s = opaque;
    uint32_t v = value;
    unsigned i;
    if (s->reset_held) {
        return;
    }
    if (addr >= 0x800 && addr < 0xe00) {
        i = (addr - 0x800) / 4;
        ESP32S3RmtRx *r = rx_ram_owner(s, i);
        if (r) {
            rx_error(r, BIT(25), 0);
            return;
        }
        s->ram[i] = v;
        return;
    }
    if (addr < 0x10) {
        ESP32S3RmtTx *t = &s->tx[addr / 4];
        unsigned capacity = ((t->conf >> 16) & 15) * 48;
        if ((s->sys_conf & BIT(0)) || !capacity || capacity > (4 - t->index) * 48 || t->fifo >= capacity) {
            t->status |= BIT(26);
            raise_event(s, TX_ERROR(t->index));
        } else {
            s->ram[t->index * 48 + t->fifo++] = v;
        }
        return;
    }
    if (addr >= 0x20 && addr < 0x30) {
        ESP32S3RmtTx *t = &s->tx[(addr - 0x20) / 4];
        t->conf = v & 0x03ffffffU & ~TX_STROBES;
        if (v & BIT(1)) {
            t->cursor = t->half = t->threshold_count = 0;
            t->word_pending = false;
            t->status &= ~BIT(25);
        }
        if (v & BIT(2)) { t->fifo = 0; t->status &= ~BIT(26); }
        if (v & BIT(24)) {
            t->applied = t->conf;
            if (v & BIT(7)) {
                tx_cancel(t, false);
                t->conf &= ~BIT(7);
                t->applied &= ~BIT(7);
            } else {
                refresh_clocks(s);
                if (!t->active) tx_idle(t, t->level);
            }
        }
        if (v & BIT(0)) {
            if (t->active) tx_cancel(t, false);
            t->armed = true;
            unsigned mask = s->tx_sim & 15;
            if ((s->tx_sim & BIT(4)) && (mask & BIT(t->index))) {
                unsigned armed = 0;
                for (i = 0; i < 4; ++i) if (s->tx[i].armed) armed |= BIT(i);
                if ((armed & mask) == mask) {
                    __uint128_t at = now_fp();
                    for (i = 0; i < 4; ++i) if (mask & BIT(i)) tx_start(&s->tx[i], at);
                }
            } else {
                tx_start(t, now_fp());
            }
        }
        return;
    }
    if (addr >= 0x30 && addr < 0x50) {
        ESP32S3RmtRx *r = &s->rx[(addr - 0x30) / 8];
        if (!(addr & 4)) {
            r->conf0 = v & 0x3fffffffU;
        } else {
            r->conf1 = v & 0xffff & ~RX_STROBES;
            /* The ISR hands RAM to APB without a CONF_UPDATE strobe. */
            r->applied1 = (r->applied1 & ~BIT(3)) | (r->conf1 & BIT(3));
            if (v & BIT(1)) { r->cursor = r->half = r->threshold_count = 0; r->status &= ~(BIT(25) | BIT(26)); }
            if (v & BIT(2)) { r->fifo = 0; r->status &= ~BIT(27); }
            if (v & BIT(15)) {
                bool was_enabled = (r->applied1 & BIT(0)) != 0;
                r->applied0 = r->conf0;
                r->applied1 = r->conf1;
                if (!(v & BIT(0))) rx_cancel(r);
                else if (!was_enabled || !r->active) rx_start(r);
                else refresh_clocks(s);
            }
        }
        return;
    }
    if (addr >= 0x80 && addr < 0x90) { s->tx[(addr - 0x80) / 4].carrier = v; return; }
    if (addr >= 0x90 && addr < 0xa0) { s->rx[(addr - 0x90) / 4].carrier = v; return; }
    if (addr >= 0xa0 && addr < 0xb0) {
        ESP32S3RmtTx *t = &s->tx[(addr - 0xa0) / 4];
        t->limit = v & 0x2fffff;
        if (v & BIT(20)) t->loops = 0;
        return;
    }
    if (addr >= 0xb0 && addr < 0xc0) { s->rx[(addr - 0xb0) / 4].limit = v & 511; return; }
    switch (addr) {
    case 0x70: /* RAW is W1C on S3 as well as the dedicated CLR register. */
    case 0x7c: s->int_raw &= ~(v & IRQ_MASK); irq_update(s); break;
    case 0x78: s->int_ena = v & IRQ_MASK; irq_update(s); break;
    case 0xc0: s->sys_conf = v & 0x87ffffffU; refresh_clocks(s); break;
    case 0xc4: s->tx_sim = v & 31; break;
    case 0xc8: /* Divider phase reset applies at the current timestamp. */
        for (i = 0; i < 4; ++i) {
            if ((v & BIT(i)) && s->tx[i].active) {
                ESP32S3RmtTx *t = &s->tx[i];
                __uint128_t now = now_fp();
                __uint128_t remaining = t->deadline > now ? t->deadline - now : 0;
                __uint128_t ticks = (remaining + t->period - 1) / t->period;
                t->deadline = now + ticks * t->period;
                tx_schedule(&s->tx[i]);
            }
            if ((v & BIT(i + 4)) && s->rx[i].active) {
                ESP32S3RmtRx *r = &s->rx[i];
                int64_t ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
                r->elapsed_ticks_fp = rx_counter_fp(r, ns) &
                                      ~((__uint128_t)0xffffffffU);
                r->counter_ns = ns;
                rx_schedule_idle(&s->rx[i]);
            }
        }
        break;
    case 0xcc: s->date = v & 0x0fffffff; break;
    default: break;
    }
}

static const MemoryRegionOps rmt_ops = {
    .read = rmt_read, .write = rmt_write, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4, .unaligned = false },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static void rmt_reset_hold(Object *obj, ResetType type)
{
    ESP32S3RmtState *s = ESP32S3_RMT(obj);
    if (s->soc_reset && !esp32s3_reset_covers_periph(s->soc_reset)) return;
    /* A TX release can synchronously notify every wired receiver. Disable all
     * capture engines first, before clearing RAM/IRQs or publishing releases. */
    for (unsigned i = 0; i < 4; ++i) {
        rx_cancel(&s->rx[i]);
    }
    s->int_raw = s->int_ena = s->tx_sim = 0;
    s->sys_conf = 0x05000010;
    s->date = 0x02101181;
    memset(s->ram, 0, sizeof(s->ram));
    for (unsigned i = 0; i < 4; ++i) {
        ESP32S3RmtTx *t = &s->tx[i];
        ESP32S3RmtRx *r = &s->rx[i];
        tx_cancel(t, true);
        t->conf = t->applied = 0x00710200;
        t->carrier = 0;
        t->limit = 128;
        t->cursor = t->fifo = t->half = t->threshold_count = t->loops = 0;
        t->dma_bound = false;
        t->status = i * 48;
        r->conf0 = r->applied0 = 0x317fff02;
        r->conf1 = r->applied1 = 0x1e8;
        r->carrier = 0;
        r->limit = 128;
        r->cursor = r->fifo = r->half = r->threshold_count = 0;
        r->dma_bound = false;
        r->status = (4 + i) * 48;
        r->started = false;
    }
    irq_update(s);
}

static void rmt_realize(DeviceState *dev, Error **errp)
{
    ESP32S3RmtState *s = ESP32S3_RMT(dev);
    if (!s->gpio || !s->electrical) {
        error_setg(errp, "RMT requires authoritative GPIO and electrical graph links");
        return;
    }
    if (!esp32s3_electrical_subscribe(s->electrical, rmt_inputs_changed, s)) {
        error_setg(errp, "RMT electrical observer capacity exhausted");
        return;
    }
    rmt_inputs_changed(s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
}

static void rmt_unrealize(DeviceState *dev)
{
    ESP32S3RmtState *s = ESP32S3_RMT(dev);
    esp32s3_electrical_unsubscribe(s->electrical, rmt_inputs_changed, s);
    for (unsigned i = 0; i < 4; ++i) {
        rx_cancel(&s->rx[i]);
    }
    for (unsigned i = 0; i < 4; ++i) {
        tx_cancel(&s->tx[i], true);
    }
    s->int_raw = s->int_ena = 0;
    irq_update(s);
}

static void rmt_finalize(Object *obj)
{
    ESP32S3RmtState *s = ESP32S3_RMT(obj);
    for (unsigned i = 0; i < 4; ++i) {
        timer_free(s->tx[i].timer);
        timer_free(s->rx[i].idle_timer);
        timer_free(s->rx[i].filter_timer);
    }
}

static void rmt_init(Object *obj)
{
    ESP32S3RmtState *s = ESP32S3_RMT(obj);
    memory_region_init_io(&s->iomem, obj, &rmt_ops, s, TYPE_ESP32S3_RMT, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    s->gate = true;
    qdev_init_gpio_in_named(DEVICE(obj), gate_changed, "clock-enable", 1);
    qdev_init_gpio_in_named(DEVICE(obj), reset_held_changed, "reset-held", 1);
    s->apb_clk = qdev_init_clock_in(DEVICE(obj), "apb-clk", clock_changed, s, ClockUpdate);
    s->xtal_clk = qdev_init_clock_in(DEVICE(obj), "xtal-clk", clock_changed, s, ClockUpdate);
    s->rc_fast_clk = qdev_init_clock_in(DEVICE(obj), "rc-fast-clk", clock_changed, s, ClockUpdate);
    for (unsigned i = 0; i < 4; ++i) {
        s->tx[i].parent = s;
        s->tx[i].index = i;
        s->tx[i].timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, tx_timer, &s->tx[i]);
        s->rx[i].parent = s;
        s->rx[i].index = i;
        s->rx[i].idle_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, rx_idle_timer, &s->rx[i]);
        s->rx[i].filter_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, rx_filter_timer, &s->rx[i]);
    }
}

static Property rmt_properties[] = {
    DEFINE_PROP_LINK("gpio", ESP32S3RmtState, gpio, TYPE_ESP32S3_GPIO, ESP32S3GPIOState *),
    DEFINE_PROP_LINK("gdma", ESP32S3RmtState, gdma, TYPE_ESP_GDMA, ESPGdmaState *),
    DEFINE_PROP_LINK("electrical", ESP32S3RmtState, electrical, TYPE_ESP32S3_ELECTRICAL, DeviceState *),
    DEFINE_PROP_LINK("soc-reset", ESP32S3RmtState, soc_reset, TYPE_DEVICE, DeviceState *),
    DEFINE_PROP_UINT8("silicon-revision", ESP32S3RmtState, silicon_revision, 0),
    DEFINE_PROP_END_OF_LIST(),
};

static void rmt_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = rmt_realize;
    dc->unrealize = rmt_unrealize;
    device_class_set_props(dc, rmt_properties);
    RESETTABLE_CLASS(klass)->phases.hold = rmt_reset_hold;
}

static const TypeInfo rmt_info = {
    .name = TYPE_ESP32S3_RMT, .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ESP32S3RmtState), .instance_init = rmt_init,
    .instance_finalize = rmt_finalize,
    .class_init = rmt_class_init,
};

static void rmt_register_types(void)
{
    type_register_static(&rmt_info);
}
type_init(rmt_register_types)
