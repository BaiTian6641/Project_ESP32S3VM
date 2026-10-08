/* SPDX-License-Identifier: GPL-2.0-or-later
 * ESP32-S3 UART: independent console transport and physical matrix frames.
 * Copyright (c) 2023 Espressif Systems (Shanghai) Co. Ltd.
 * All progression uses virtual time and fixed per-controller storage.
 */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-clock.h"
#include "hw/char/esp32s3_uart.h"
#include "hw/misc/esp32s3_electrical.h"
#include "hw/xtensa/esp32s3_reset_domain.h"
#include "qemu/bitops.h"
#include "chardev/char-fe.h"
/* Pin the trace barrier to this tree's log API: qemu_log_unlock() flushes the
 * held stream, so trylock/unlock is the durable-barrier equivalent of the older
 * qemu_log_flush() helper. */
static void esp32s3_uart_log_barrier(void)
{
    FILE *logfile = qemu_log_trylock();

    if (logfile) {
        qemu_log_unlock(logfile);
    }
}


#define B(n) (1u << (n))
#define REG(s, off) ((s)->regs[(off) / 4])
#define CORE(s, off) ((s)->core[(off) / 4])
#define CONF(s) CORE(s, 0x20)
#define IRQ_RX_FULL B(0)
#define IRQ_TX_EMPTY B(1)
#define IRQ_PARITY B(2)
#define IRQ_FRAME B(3)
#define IRQ_OVERFLOW B(4)
#define IRQ_BREAK B(7)
#define IRQ_TIMEOUT B(8)
#define IRQ_TX_BRK_DONE B(12)
#define IRQ_TX_DONE B(14)
#define IRQ_RS_PARITY B(15)
#define IRQ_RS_FRAME B(16)
#define IRQ_RS_CLASH B(17)
#define IRQ_AT B(18)
enum {
    UART_SYNC_ALL = -1,
    UART_REFRESH_CLOCKS = -2,
    UART_WAIT_NONE = 0,
    UART_WAIT_BEFORE_START = 1,
    UART_WAIT_AFTER_STOP = 2,
};
static const unsigned rx_signal[3] = {12, 15, 18};
static const unsigned tx_signal[3] = {12, 15, 18};
static const unsigned rts_signal[3] = {13, 16, 19};
static const unsigned cts_signal[3] = {13, 16, 19};
static const unsigned dtr_signal[3] = {14, 17, 20};

static void start_tx(ESP32S3UARTState *s);
static void update(ESP32S3UARTState *s);
static bool rx_sample(ESP32S3UARTState *s, bool *level);
static void observe_edge(ESP32S3UARTState *s, bool previous, bool level,
                         uint64_t duration, uint64_t same_edge_duration);

/* Rational bit period in ns. Intermediate products are bounded by register
 * widths (12-bit UART divider, 8-bit source divider and 6-bit fraction). */
static void timing(ESP32S3UARTState *s)
{
    uint32_t cfg = CORE(s, 0x78);
    unsigned sel = (cfg >> 20) & 3;
    Clock *clock = sel == 1 ? s->apb : sel == 2 ? s->rc_fast : s->xtal;
    uint64_t hz = sel ? clock_get_hz(clock) : 0;
    unsigned a = (cfg >> 6) & 63, b = cfg & 63;
    uint64_t denom = b ? b : 1;
    uint64_t divider = (((cfg >> 12) & 255) + 1) * denom + (b ? a : 0);
    uint64_t uart_div = ((CORE(s, 0x14) & 4095) << 4) |
                        ((CORE(s, 0x14) >> 20) & 15);
    s->bit_num = NANOSECONDS_PER_SECOND * divider * uart_div;
    s->bit_den = hz * denom * 16;
    s->parent.baud_rate = s->bit_num && s->bit_den ?
        MAX(1, ((__uint128_t)s->bit_den * NANOSECONDS_PER_SECOND) / s->bit_num) : 0;
}

static bool running(ESP32S3UARTState *s, bool tx)
{
    unsigned cfg = CORE(s, 0x78);
    if (CONF(s) & B(16)) {
        bool transmit = CONF(s) & B(10);
        if ((tx && !transmit) || (!tx && transmit && !(CONF(s) & B(9)))) {
            return false;
        }
    }
    if (!tx && (CORE(s, 0x4c) & B(0)) && !(CORE(s, 0x4c) & B(3)) &&
        (s->tx_active || s->break_active || s->tx_idle_active)) {
        return false;
    }
    return s->gate && !s->held_reset && s->bit_num && s->bit_den &&
           (cfg & B(22)) && (cfg & (tx ? B(24) : B(25))) &&
           !(cfg & (B(23) | (tx ? B(26) : B(27))));
}

static uint64_t after_bits(ESP32S3UARTState *s, uint64_t origin,
                           uint64_t half_bits)
{
    /* Divide before multiplying long timeout counts; frames are <= 32 half
     * bits, and UART timeout/AT counts <= 65535 bits. */
    uint64_t den = s->bit_den * 2;
    uint64_t whole = s->bit_num / den, rem = s->bit_num % den;
    return origin + whole * half_bits +
           (rem * half_bits + den - 1) / den;
}

static uint64_t after_sixteenths(ESP32S3UARTState *s, uint64_t origin,
                                unsigned ticks)
{
    uint64_t den = s->bit_den * 16;
    return origin + (s->bit_num / den) * ticks +
           ((s->bit_num % den) * ticks + den - 1) / den;
}

static bool sample(ESP32S3UARTState *s, unsigned signal, bool *level)
{
    if (!s->gpio || !s->electrical) {
        return false;
    }
    unsigned cfg = s->gpio->func_in_sel_cfg[signal];
    bool consuming = s->rx_active || s->sampling_idle ||
        (signal == cts_signal[s->index] && (CONF(s) & B(15))) ||
        (signal == rx_signal[s->index] && s->tx_active &&
         (CORE(s, 0x4c) & (B(0) | B(3))) == (B(0) | B(3)));
    if (cfg & B(7)) {
        unsigned pad = cfg & 63;
        if (pad < ESP32S3_GPIO_COUNT && !esp32s3_gpio_net_valid(s->gpio, pad)) {
            if (consuming) {
                esp32s3_gpio_consume_unknown(s->gpio, pad);
            }
            return false;
        }
        return esp32s3_gpio_matrix_sample(s->gpio, signal, level);
    }
    /* The direct per-signal routes are chip-specific UART inputs. */
    static const int rx_pad[3] = {44, 18, -1};
    static const int cts_pad[3] = {16, 20, -1};
    int pad = signal == rx_signal[s->index] ? rx_pad[s->index] : cts_pad[s->index];
    if (pad < 0) {
        return false;
    }
    ESP32S3GpioDriveSnapshot snapshot;
    esp32s3_gpio_get_drive_snapshot(s->gpio, pad, &snapshot);
    unsigned mux = s->index == 0 && signal == rx_signal[0] ? 0 : 2;
    if (!snapshot.ie || snapshot.mcu_sel != mux) {
        return false;
    }
    bool valid;
    double voltage;
    if (!esp32s3_electrical_line(s->electrical, pad,
            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), &voltage, &valid, level) ||
        !valid) {
        if (consuming) {
            esp32s3_gpio_consume_unknown(s->gpio, pad);
        }
        return false;
    }
    return true;
}

static void drive(ESP32S3UARTState *s, bool logical)
{
    if ((CONF(s) & B(16)) && !(CONF(s) & B(10))) {
        logical = !!(CONF(s) & B(12));
    }
    s->tx_level = logical;
    bool level = logical ^ !!(CONF(s) & B(22));
    if (s->trace) {
        qemu_log("esp32s3-uart%u event=tx-drive ns=%" PRIu64 " level=%u\n",
                 s->index, (uint64_t)qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), level);
        esp32s3_uart_log_barrier();
    }
    if (s->electrical) {
        esp32s3_electrical_set_matrix_drive(s->electrical, tx_signal[s->index],
                                           true, level, false);
    }
    if ((CONF(s) & B(14)) || ((CONF(s) & B(16)) && (CONF(s) & B(9)))) {
        esp32s3_uart_net_changed(s);
    }
}

static void update(ESP32S3UARTState *s)
{
    unsigned rx = fifo8_num_used(&s->parent.rx_fifo);
    unsigned tx = fifo8_num_used(&s->parent.tx_fifo);
    uint32_t raw = REG(s, 4) & ~(IRQ_RX_FULL | IRQ_TX_EMPTY | IRQ_TX_DONE);
    if (rx && rx >= (CORE(s, 0x24) & 1023)) {
        raw |= IRQ_RX_FULL;
    }
    if (tx <= ((CORE(s, 0x24) >> 10) & 1023)) {
        raw |= IRQ_TX_EMPTY;
    }
    if (!tx && !s->tx_active && !s->break_active && !s->tx_idle_active) {
        raw |= IRQ_TX_DONE;
    }
    REG(s, 4) = raw;
    REG(s, 8) = raw & REG(s, 12);
    qemu_set_irq(s->parent.irq, REG(s, 8) != 0);
    bool rts = CONF(s) & B(6) ? false : true;
    if (CORE(s, 0x24) & B(22)) {
        rts = rx >= ((CORE(s, 0x60) >> 7) & 1023);
    }
    rts ^= !!(CONF(s) & B(23));
    if (s->rts_level != rts || !s->finalized) {
        s->rts_level = rts;
        if (s->electrical) {
            esp32s3_electrical_set_matrix_drive(s->electrical,
                rts_signal[s->index], true, rts, false);
        }
    }
    bool dtr = (CORE(s, 0x4c) & B(0)) ?
        s->tx_active || s->break_active || s->tx_idle_active : !(CONF(s) & B(7));
    dtr ^= !!(CONF(s) & B(24));
    if (s->dtr_level != dtr || !s->finalized) {
        s->dtr_level = dtr;
        if (s->electrical) {
            esp32s3_electrical_set_matrix_drive(s->electrical,
                dtr_signal[s->index], true, dtr, false);
        }
    }
    if (s->dma_notify && !s->notifying) {
        s->notifying = true;
        s->dma_notify(s->dma_opaque);
        s->notifying = false;
    }
}

unsigned esp32s3_uart_tx_free(ESP32S3UARTState *s)
{
    return s->tx_capacity > fifo8_num_used(&s->parent.tx_fifo) ?
        s->tx_capacity - fifo8_num_used(&s->parent.tx_fifo) : 0;
}

unsigned esp32s3_uart_rx_used(ESP32S3UARTState *s)
{
    return fifo8_num_used(&s->parent.rx_fifo);
}

void esp32s3_uart_set_dma_notify(ESP32S3UARTState *s,
                               void (*notify)(void *), void *opaque)
{
    s->dma_notify = notify;
    s->dma_opaque = opaque;
}

void esp32s3_uart_set_rx_event(ESP32S3UARTState *s,
                             void (*event)(void *, unsigned, bool, bool, unsigned),
                             void *opaque)
{
    s->rx_event = event;
    s->rx_event_opaque = opaque;
}

bool esp32s3_uart_dma_write(ESP32S3UARTState *s, uint8_t byte)
{
    if (!esp32s3_uart_tx_free(s) || (CORE(s, 0x60) & B(27))) {
        return false;
    }
    fifo8_push(&s->parent.tx_fifo, byte);
    s->tx_wptr = (s->tx_wptr + 1) % MAX(1, s->tx_capacity);
    start_tx(s);
    update(s);
    return true;
}

bool esp32s3_uart_dma_read(ESP32S3UARTState *s, uint8_t *byte)
{
    if (!fifo8_num_used(&s->parent.rx_fifo) || (CORE(s, 0x60) & B(27))) {
        return false;
    }
    *byte = fifo8_pop(&s->parent.rx_fifo);
    s->rx_rptr = (s->rx_rptr + 1) % MAX(1, s->rx_capacity);
    if (!fifo8_num_used(&s->parent.rx_fifo)) {
        timer_del(&s->timeout_timer);
    }
    update(s);
    qemu_chr_fe_accept_input(&s->parent.chr);
    return true;
}

static void timeout_cb(void *opaque)
{
    ESP32S3UARTState *s = opaque;
    if (running(s, false) && (CORE(s, 0x24) & B(23)) &&
        fifo8_num_used(&s->parent.rx_fifo)) {
        REG(s, 4) |= IRQ_TIMEOUT;
        update(s);
    }
}

static void arm_timeout(ESP32S3UARTState *s)
{
    timer_del(&s->timeout_timer);
    if (running(s, false) && (CORE(s, 0x24) & B(23)) &&
        fifo8_num_used(&s->parent.rx_fifo)) {
        unsigned bits = (CORE(s, 0x60) >> 17) & 1023;
        if (bits) {
            timer_mod_ns(&s->timeout_timer,
                after_bits(s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), bits * 2));
        }
    }
}

static void idle_cb(void *opaque)
{
    ESP32S3UARTState *s = opaque;
    if (running(s, false) && !s->rx_active && s->rx_event) {
        bool level;
        s->sampling_idle = true;
        bool valid = rx_sample(s, &level);
        s->sampling_idle = false;
        bool idle = valid && ((CONF(s) & B(16)) ?
            (level ^ !!(CONF(s) & B(13))) : level);
        if (!idle) {
            s->idle_waiting = true;
            return;
        }
        s->idle_waiting = false;
        s->rx_event(s->rx_event_opaque, s->index, false, false,
                    fifo8_num_used(&s->parent.rx_fifo));
    }
}

static void arm_idle(ESP32S3UARTState *s)
{
    if (running(s, false)) {
        unsigned bits = CORE(s, 0x48) & 1023;
        timer_mod_ns(&s->idle_timer,
            after_bits(s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), MAX(1, bits) * 2));
    }
}

static void at_cb(void *opaque)
{
    ESP32S3UARTState *s = opaque;
    if (s->at_pending && !s->rx_active && running(s, false)) {
        REG(s, 4) |= IRQ_AT;
        s->at_pending = false;
        update(s);
    }
}

static bool received(ESP32S3UARTState *s, uint8_t byte, bool external)
{
    uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    bool queued = false;
    if (external && (CORE(s, 0x4c) & B(0)) && s->tx_active &&
        !(CORE(s, 0x4c) & B(3))) {
        return false;
    }
    if (fifo8_num_used(&s->parent.rx_fifo) >= s->rx_capacity ||
        (CORE(s, 0x60) & B(27))) {
        REG(s, 4) |= IRQ_OVERFLOW;
    } else {
        fifo8_push(&s->parent.rx_fifo, byte);
        queued = true;
        s->rx_wptr = (s->rx_wptr + 1) % MAX(1, s->rx_capacity);
    }
    unsigned command = CORE(s, 0x5c) & 255;
    unsigned count = (CORE(s, 0x5c) >> 8) & 255;
    uint64_t idle = s->frame_rx_start > s->last_rx_end ?
                    s->frame_rx_start - s->last_rx_end : 0;
    unsigned required = s->at_count ? CORE(s, 0x58) & 65535 :
                                   CORE(s, 0x50) & 65535;
    uint64_t guard = after_bits(s, 0, required * 2);
    bool guard_ok = s->at_count ? idle <= guard : idle >= guard;
    if (count && byte == command && guard_ok && !s->rx_error) {
        if (++s->at_count >= count) {
            s->at_pending = true;
            timer_mod_ns(&s->at_timer,
                after_bits(s, now, (CORE(s, 0x54) & 65535) * 2));
        }
    } else {
        s->at_count = 0;
    }
    s->last_rx_end = now;
    arm_timeout(s);
    update(s);
    return queued;
}

static unsigned stop_half(uint32_t conf)
{
    unsigned stop = (conf >> 4) & 3;
    return stop == 2 ? 3 : stop == 3 ? 4 : 2;
}

static bool parity(uint8_t byte, unsigned bits, bool odd)
{
    return (ctpop8(byte & ((1u << bits) - 1)) & 1) ^ odd;
}

static unsigned next_tx_tick(ESP32S3UARTState *s)
{
    unsigned scale = s->tx_irda ? 16 : 2;
    unsigned stop = (1 + s->tx_data_bits + s->tx_parity) * scale;
    unsigned end = stop + s->tx_stop_half * scale / 2;
    unsigned next = s->tx_tick + 1;
    if (s->tx_irda) {
        bool zero = s->tx_tick < stop &&
                    !(s->tx_frame & (1u << (s->tx_tick / 16)));
        unsigned phase = s->tx_tick % 16;
        unsigned base = s->tx_tick - phase;
        next = zero && phase < 8 ? base + 8 :
               zero && phase < 8 + s->tx_irda_pulse_ticks ?
                   base + 8 + s->tx_irda_pulse_ticks : base + 16;
    }
    return MIN(next, end);
}

static bool begin_tx_idle(ESP32S3UARTState *s)
{
    unsigned bits = (CORE(s, 0x48) >> 10) & 1023;
    if (!bits || fifo8_num_used(&s->parent.tx_fifo) ||
        ((CONF(s) & B(8)) && !s->break_sent)) {
        return false;
    }
    s->tx_idle_active = true;
    timer_mod_ns(&s->tx_timer,
        after_bits(s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), bits * 2));
    return true;
}

static void start_wire_frame(ESP32S3UARTState *s)
{
    s->tx_origin = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    drive(s, s->tx_irda ? !!(CONF(s) & B(12)) : false);
    s->tx_next_tick = next_tx_tick(s);
    timer_mod_ns(&s->tx_timer, s->tx_irda ?
        after_sixteenths(s, s->tx_origin, s->tx_next_tick) :
        after_bits(s, s->tx_origin, s->tx_next_tick));
}

static void finish_tx_frame(ESP32S3UARTState *s)
{
    s->tx_active = false;
    bool idle = begin_tx_idle(s);
    drive(s, (CONF(s) & B(16)) ? !!(CONF(s) & B(12)) : true);
    if (!idle) {
        start_tx(s);
    }
    update(s);
}

static void tx_cb(void *opaque)
{
    ESP32S3UARTState *s = opaque;
    if (!running(s, true)) {
        return;
    }
    if (s->tx_wait_phase == UART_WAIT_BEFORE_START) {
        s->tx_wait_phase = UART_WAIT_NONE;
        start_wire_frame(s);
        return;
    }
    if (s->tx_wait_phase == UART_WAIT_AFTER_STOP) {
        s->tx_wait_phase = UART_WAIT_NONE;
        finish_tx_frame(s);
        return;
    }
    if (s->tx_idle_active) {
        s->tx_idle_active = false;
        start_tx(s);
        update(s);
        return;
    }
    if (s->break_active) {
        s->break_active = false;
        REG(s, 4) |= IRQ_TX_BRK_DONE;
        bool idle = begin_tx_idle(s);
        drive(s, (CONF(s) & B(16)) ? !!(CONF(s) & B(12)) : true);
        if (!idle) {
            start_tx(s);
        }
        update(s);
        return;
    }
    if (!s->tx_active) {
        start_tx(s);
        return;
    }
    unsigned scale = s->tx_irda ? 16 : 2;
    unsigned stop_start = (1 + s->tx_data_bits + s->tx_parity) * scale;
    unsigned frame_end = stop_start + s->tx_stop_half * scale / 2;
    s->tx_tick = s->tx_next_tick;
    if (s->tx_tick >= frame_end) {
        if (qemu_chr_fe_backend_open(&s->parent.chr)) {
            /* Host console observes a completed frame but never drives nets. */
            qemu_chr_fe_write(&s->parent.chr, &s->tx_byte, 1);
        }
        if (s->tx_post_delay) {
            s->tx_wait_phase = UART_WAIT_AFTER_STOP;
            drive(s, true);
            timer_mod_ns(&s->tx_timer,
                after_bits(s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), 2));
            update(s);
        } else {
            finish_tx_frame(s);
        }
        return;
    }
    bool logical = s->tx_tick < stop_start ?
        !!(s->tx_frame & (1u << (s->tx_tick / scale))) : true;
    if (s->tx_irda) {
        unsigned phase = s->tx_tick % 16;
        logical = !logical && phase >= 8 && phase < 8 + s->tx_irda_pulse_ticks;
        logical ^= !!(CONF(s) & B(12));
    }
    drive(s, logical);
    s->tx_next_tick = next_tx_tick(s);
    timer_mod_ns(&s->tx_timer, s->tx_irda ?
        after_sixteenths(s, s->tx_origin, s->tx_next_tick) :
        after_bits(s, s->tx_origin, s->tx_next_tick));
}

static void start_tx(ESP32S3UARTState *s)
{
    if (s->tx_active || s->break_active || s->tx_idle_active || !running(s, true)) {
        return;
    }
    if (!fifo8_num_used(&s->parent.tx_fifo) &&
        (!(CONF(s) & B(8)) || s->break_sent)) {
        return;
    }
    if (CONF(s) & B(15)) {
        s->cts_valid = sample(s, cts_signal[s->index], &s->cts_level);
        if (!s->cts_valid || (s->cts_level ^ !!(CONF(s) & B(20)))) {
            return;
        }
    }
    if ((CORE(s, 0x4c) & B(0)) && !(CORE(s, 0x4c) & B(4)) && s->rx_active) {
        return;
    }
    if (!fifo8_num_used(&s->parent.tx_fifo)) {
        if ((CONF(s) & B(8)) && !s->break_sent) {
            s->break_active = true;
            s->break_sent = true;
            drive(s, false);
            update(s);
            unsigned bits = CORE(s, 0x44) & 255;
            timer_mod_ns(&s->tx_timer,
                after_bits(s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), MAX(1, bits) * 2));
        }
        return;
    }
    s->tx_byte = fifo8_pop(&s->parent.tx_fifo);
    s->tx_rptr = (s->tx_rptr + 1) % MAX(1, s->tx_capacity);
    s->tx_data_bits = 5 + ((CONF(s) >> 2) & 3);
    s->tx_byte &= (1u << s->tx_data_bits) - 1;
    s->tx_parity = CONF(s) & B(1);
    s->tx_stop_half = stop_half(CONF(s));
    s->tx_frame = (s->tx_byte & ((1u << s->tx_data_bits) - 1)) << 1;
    if (s->tx_parity && parity(s->tx_byte, s->tx_data_bits, CONF(s) & B(0))) {
        s->tx_frame |= 1u << (s->tx_data_bits + 1);
    }
    s->tx_active = true;
    s->tx_tick = 0;
    s->tx_wait_phase = UART_WAIT_NONE;
    s->tx_irda = CONF(s) & B(16);
    s->tx_irda_pulse_ticks = (CONF(s) & B(11)) ? 3 : 2;
    s->tx_post_delay = (CORE(s, 0x4c) & (B(0) | B(2))) == (B(0) | B(2));
    if ((CORE(s, 0x4c) & (B(0) | B(1))) == (B(0) | B(1))) {
        s->tx_wait_phase = UART_WAIT_BEFORE_START;
        drive(s, true);
        timer_mod_ns(&s->tx_timer,
            after_bits(s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), 2));
    }
    if ((CORE(s, 0x4c) & B(0)) && !(CORE(s, 0x4c) & B(3))) {
        s->rx_active = false;
        s->rx_valid = false;
        timer_del(&s->rx_timer);
        timer_del(&s->idle_timer);
        s->remaining[1] = s->remaining[5] = 0;
    }
    update(s);
    if (s->tx_wait_phase == UART_WAIT_NONE) {
        start_wire_frame(s);
    }
}

static bool rx_sample(ESP32S3UARTState *s, bool *level)
{
    if ((CONF(s) & B(16)) && (CONF(s) & B(9))) {
        *level = !s->tx_level;
        return true;
    }
    if (CONF(s) & B(14)) {
        *level = s->tx_level;
        return true;
    }
    if (!sample(s, rx_signal[s->index], level)) {
        return false;
    }
    *level ^= !!(CONF(s) & B(19));
    return true;
}

static void rx_cb(void *opaque)
{
    ESP32S3UARTState *s = opaque;
    bool level;
    if (!running(s, false) || !s->rx_active) {
        return;
    }
    if (!rx_sample(s, &level)) {
        /* The strict raw provider reports/pause unknown before consumption. */
        return;
    }
    if (s->rx_irda) {
        level = !s->irda_zero;
        s->irda_zero = false;
    }
    if (s->trace) {
        qemu_log("esp32s3-uart%u event=rx-sample ns=%" PRIu64 " bit=%u level=%u\n",
                 s->index, (uint64_t)qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
                 s->rx_step, level);
        esp32s3_uart_log_barrier();
    }
    s->rx_all_low &= !level;
    if (s->rx_step == 0) {
        if (level) {
            s->rx_active = false;
            return;
        }
    } else if (s->rx_step <= s->rx_data_bits) {
        if (level) {
            s->rx_byte |= 1u << (s->rx_step - 1);
        }
    } else if (s->rx_parity && s->rx_step == s->rx_data_bits + 1) {
        if (level != parity(s->rx_byte, s->rx_data_bits, CONF(s) & B(0))) {
            s->rx_error = true;
            REG(s, 4) |= IRQ_PARITY;
            if ((CORE(s, 0x4c) & B(0)) && s->tx_active) {
                REG(s, 4) |= IRQ_RS_PARITY;
            }
        }
    } else {
        if (!level) {
            s->rx_error = true;
            REG(s, 4) |= IRQ_FRAME;
            if ((CORE(s, 0x4c) & B(0)) && s->tx_active) {
                REG(s, 4) |= IRQ_RS_FRAME;
            }
        }
        if (s->rx_all_low) {
            REG(s, 4) |= IRQ_BREAK;
        }
        unsigned first_stop = s->rx_data_bits + 1 + s->rx_parity;
        if (s->rx_step == first_stop && s->rx_stop_half > 2) {
            ++s->rx_step;
            s->rx_next_half = first_stop * 2 + s->rx_stop_half - 1;
            timer_mod_ns(&s->rx_timer, after_bits(s, s->rx_origin, s->rx_next_half));
            return;
        }
        s->rx_active = false;
        bool queued = false;
        if (!s->rx_error || !(CONF(s) & B(26))) {
            queued = received(s, s->rx_byte, true);
        }
        if (s->rx_all_low && s->rx_event) {
            s->rx_event(s->rx_event_opaque, s->index, true, queued,
                        fifo8_num_used(&s->parent.rx_fifo));
        }
        arm_idle(s);
        update(s);
        start_tx(s);
        return;
    }
    ++s->rx_step;
    s->rx_next_half = s->rx_step * 2 + 1;
    timer_mod_ns(&s->rx_timer, after_bits(s, s->rx_origin, s->rx_next_half));
}

static void rx_start_frame(ESP32S3UARTState *s, uint64_t now, bool irda)
{
    timer_del(&s->at_timer);
    timer_del(&s->idle_timer);
    s->remaining[5] = 0;
    s->idle_waiting = false;
    s->at_pending = false;
    s->rx_active = true;
    s->rx_origin = now;
    s->frame_rx_start = now;
    s->rx_step = s->rx_byte = 0;
    s->rx_error = false;
    s->rx_all_low = true;
    s->rx_data_bits = 5 + ((CONF(s) >> 2) & 3);
    s->rx_parity = CONF(s) & B(1);
    s->rx_stop_half = stop_half(CONF(s));
    s->rx_irda = irda;
    timer_mod_ns(&s->rx_timer, after_bits(s, now, 1));
    s->rx_next_half = 1;
}

static uint32_t rx_route_signature(ESP32S3UARTState *s)
{
    /* Any routing change invalidates the level history: a stale baseline
     * sampled from the previous pad otherwise swallows the first start bit
     * on the new pad (level == rx_level, so no falling edge is seen). */
    if (!s->gpio) {
        return 0;
    }
    unsigned cfg = s->gpio->func_in_sel_cfg[rx_signal[s->index]];
    uint32_t sig = cfg ^ ((uint32_t)rx_signal[s->index] << 16);
    if (!(cfg & B(7))) {
        static const int rx_pad[3] = {44, 18, -1};
        int pad = rx_pad[s->index];
        if (pad >= 0) {
            ESP32S3GpioDriveSnapshot snap;
            esp32s3_gpio_get_drive_snapshot(s->gpio, pad, &snap);
            sig ^= (snap.ie ? 0x100u : 0) | ((uint32_t)snap.mcu_sel << 9);
        }
    }
    return sig;
}

void esp32s3_uart_net_changed(void *opaque)
{
    ESP32S3UARTState *s = opaque;
    bool level;
    if (!running(s, false)) {
        start_tx(s);
        return;
    }
    bool valid = rx_sample(s, &level);
    bool irda = CONF(s) & B(16);
    if (irda && valid) {
        level ^= !!(CONF(s) & B(13));
        if (!level && (!s->rx_valid || s->rx_level)) {
            s->irda_zero = true;
        }
    }
    uint32_t route = rx_route_signature(s);
    bool rerouted = route != s->rx_route;
    s->rx_route = route;
    uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (valid && !level && !s->rx_active && (rerouted || !s->rx_valid)) {
        /* First valid sample on a (re)routed input that is already low:
         * hardware samples the line as it finds it, so a low line is a start
         * condition. The stale baseline must not swallow this falling edge;
         * no edge is recorded because no transition was observed. */
        rx_start_frame(s, now, irda);
    }
    if (valid && s->rx_valid && level != s->rx_level) {
        uint64_t previous = level ? s->rise_ns : s->fall_ns;
        observe_edge(s, s->rx_level, level, now - s->edge_ns,
                     previous ? now - previous : 0);
        s->edge_ns = now;
        if (level) {
            s->rise_ns = now;
        } else {
            s->fall_ns = now;
        }
        if (!level && !s->rx_active) {
            rx_start_frame(s, now, irda);
        }
    }
    s->rx_valid = valid;
    if (valid) {
        s->rx_level = level;
    }
    if (valid && !s->rx_active && s->idle_waiting && level) {
        s->idle_waiting = false;
        arm_idle(s);
    }

    if (valid && s->rx_active && !timer_pending(&s->rx_timer) && !s->remaining[1]) {
        timer_mod_ns(&s->rx_timer, now);
    }
    if ((CORE(s, 0x4c) & B(0)) && (CORE(s, 0x4c) & B(3)) &&
        s->tx_active && valid && level != s->tx_level) {
        REG(s, 4) |= IRQ_RS_CLASH;
        update(s);
    }
    start_tx(s);
}
static void frame_notify(void *opaque, uint64_t sample_ns)
{
    esp32s3_uart_net_changed(opaque);
}

bool esp32s3_uart_bind(ESP32S3UARTState *s, ESP32S3GPIOState *gpio,
                      DeviceState *electrical)
{
    if (s->electrical) {
        esp32s3_electrical_unsubscribe(s->electrical, frame_notify, s);
    }
    /* gpio/electrical are strong link properties whose release unrefs whatever
     * the field holds at teardown, so binding must take a reference first. */
    if (s->gpio != gpio) {
        if (s->gpio) {
            object_unref(OBJECT(s->gpio));
        }
        object_ref(OBJECT(gpio));
        s->gpio = gpio;
    }
    if (s->electrical != electrical) {
        if (s->electrical) {
            object_unref(OBJECT(s->electrical));
        }
        object_ref(OBJECT(electrical));
        s->electrical = electrical;
    }
    if (!esp32s3_electrical_subscribe(electrical, frame_notify, s)) {
        return false;
    }
    drive(s, s->tx_level);
    s->finalized = false;
    update(s);
    s->finalized = true;
    esp32s3_uart_net_changed(s);
    return true;
}

static unsigned reference_cycles(ESP32S3UARTState *s, uint64_t ns)
{
    unsigned cfg = CORE(s, 0x78), sel = (cfg >> 20) & 3;
    Clock *clock = sel == 1 ? s->apb : sel == 2 ? s->rc_fast : s->xtal;
    unsigned b = cfg & 63, a = (cfg >> 6) & 63;
    unsigned denominator = b ? b : 1;
    unsigned divider = (((cfg >> 12) & 255) + 1) * denominator + (b ? a : 0);
    __uint128_t cycles = (__uint128_t)ns * clock_get_hz(clock) * denominator;
    cycles /= (uint64_t)divider * NANOSECONDS_PER_SECOND;
    return cycles ? MIN(cycles - 1, 4095) : 0;
}

static void observe_edge(ESP32S3UARTState *s, bool previous, bool level,
                         uint64_t duration, uint64_t same_edge_duration)
{
    if (!(CONF(s) & B(27))) {
        return;
    }
    unsigned off = previous ? 0x2c : 0x28;
    REG(s, off) = MIN(REG(s, off), reference_cycles(s, duration));
    REG(s, 0x30) = MIN(1023, REG(s, 0x30) + 1);
    if (same_edge_duration) {
        off = level ? 0x70 : 0x74;
        REG(s, off) = MIN(REG(s, off), reference_cycles(s, same_edge_duration));
    }
}

static void console_edge(ESP32S3UARTState *s, bool level)
{
    if (level != s->console_level) {
        uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        observe_edge(s, s->console_level, level, now - s->console_edge_ns, 0);
        s->console_edge_ns = now;
        s->console_level = level;
    }
}

static void console_cb(void *opaque)
{
    ESP32S3UARTState *s = opaque;
    if (!running(s, false)) {
        return;
    }
    if (s->console_active && ++s->console_step >= 10) {
        s->console_active = false;
        s->frame_rx_start = s->console_origin;
        s->rx_error = false;
        received(s, s->console_byte, false);
        qemu_chr_fe_accept_input(&s->parent.chr);
    }
    if (!s->console_active && fifo8_num_used(&s->console_rx)) {
        s->console_byte = fifo8_pop(&s->console_rx);
        s->console_step = 0;
        s->console_active = true;
        s->console_origin = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    }
    if (s->console_active) {
        /* An explicit host-console sender, 8N1 at console-baud. These input
         * events can be measured by ROM autobaud but never drive a pad/net. */
        bool level = s->console_step ? s->console_step >= 9 ||
            !!(s->console_byte & (1u << (s->console_step - 1))) : false;
        console_edge(s, level);
        uint64_t ticks = (uint64_t)(s->console_step + 1) * NANOSECONDS_PER_SECOND;
        timer_mod_ns(&s->console_timer,
            s->console_origin + (ticks + s->console_baud - 1) / s->console_baud);
    }
}

static int console_can_receive(void *opaque)
{
    ESP32S3UARTState *s = opaque;
    return fifo8_num_free(&s->console_rx);
}

static void console_receive(void *opaque, const uint8_t *buf, int size)
{
    ESP32S3UARTState *s = opaque;
    for (int i = 0; i < size && fifo8_num_free(&s->console_rx); ++i) {
        fifo8_push(&s->console_rx, buf[i]);
    }
    if (!s->console_active) {
        console_cb(s);
    }
}

static void console_event(void *opaque, QEMUChrEvent event)
{
    ESP32S3UARTState *s = opaque;
    if (event == CHR_EVENT_BREAK) {
        REG(s, 4) |= IRQ_BREAK;
        update(s);
    }
}

static uint64_t uart_read(void *opaque, hwaddr addr, unsigned size)
{
    ESP32S3UARTState *s = opaque;
    uint8_t byte;
    if (addr >= sizeof(s->regs) || (addr & 3)) {
        return 0;
    }
    switch (addr) {
    case 0: return esp32s3_uart_dma_read(s, &byte) ? byte : 0;
    case 0x1c:
        return fifo8_num_used(&s->parent.rx_fifo) |
            (fifo8_num_used(&s->parent.tx_fifo) << 16) |
            (s->cts_level ? B(14) : 0) | (s->rx_level ? B(15) : 0) |
            (s->dtr_level ? B(29) : 0) | (s->rts_level ? B(30) : 0) |
            (s->tx_level ? B(31) : 0);
    case 0x64:
        return ((s->index * 128 + s->tx_wptr) & 1023) |
            (((s->index * 128 + s->tx_rptr) & 1023) << 11);
    case 0x68:
        return ((512 + s->index * 128 + s->rx_rptr) & 1023) |
            (((512 + s->index * 128 + s->rx_wptr) & 1023) << 11);
    case 0x6c:
        return (s->rx_active ? 1 : 0) |
            (s->tx_active || s->break_active || s->tx_idle_active ? 0x10 : 0);
    default: return REG(s, addr);
    }
}

static void fifo_reset(ESP32S3UARTState *s, bool tx)
{
    if (tx) {
        fifo8_reset(&s->parent.tx_fifo);
        s->tx_rptr = s->tx_wptr = 0;
        s->tx_active = s->break_active = s->tx_idle_active = false;
        s->tx_wait_phase = UART_WAIT_NONE;
        s->tx_post_delay = false;
        timer_del(&s->tx_timer);
        drive(s, (CONF(s) & B(16)) ? !!(CONF(s) & B(12)) : true);
    } else {
        fifo8_reset(&s->parent.rx_fifo);
        s->rx_rptr = s->rx_wptr = 0;
        s->rx_active = false;
        s->idle_waiting = false;
        timer_del(&s->rx_timer);
        timer_del(&s->timeout_timer);
        timer_del(&s->idle_timer);
        s->remaining[5] = 0;
        REG(s, 4) &= ~IRQ_TIMEOUT;
    }
}

static uint64_t gcd(uint64_t a, uint64_t b)
{
    while (b) {
        uint64_t next = a % b;
        a = b;
        b = next;
    }
    return a;
}

static void retime(ESP32S3UARTState *s, QEMUTimer *timer, unsigned slot,
                   bool active, bool enabled, uint64_t old_num, uint64_t old_den)
{
    if (!active) {
        timer_del(timer);
        s->remaining[slot] = 0;
        return;
    }
    if (enabled && timer_pending(timer) &&
        (slot == 2 || (old_num == s->bit_num && old_den == s->bit_den))) {
        return;
    }
    uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (timer_pending(timer)) {
        s->remaining[slot] = MAX(1, timer_expire_time_ns(timer) - (int64_t)now);
        s->remaining_num[slot] = old_num;
        s->remaining_den[slot] = old_den;
        timer_del(timer);
    }
    if (!enabled || !s->remaining[slot]) {
        return;
    }
    if (slot == 2) {
        timer_mod_ns(timer, now + s->remaining[slot]);
        s->remaining[slot] = 0;
        uint64_t ticks = (uint64_t)(s->console_step + 1) * NANOSECONDS_PER_SECOND;
        s->console_origin = timer_expire_time_ns(timer) -
            (ticks + s->console_baud - 1) / s->console_baud;
        return;
    }
    uint64_t n = s->bit_num, d = s->bit_den;
    uint64_t on = s->remaining_num[slot], od = s->remaining_den[slot];
    uint64_t factor = gcd(n, on);
    n /= factor;
    on /= factor;
    factor = gcd(od, d);
    od /= factor;
    d /= factor;
    __uint128_t numerator = (__uint128_t)s->remaining[slot] * n * od;
    __uint128_t denominator = (__uint128_t)on * d;
    uint64_t duration = (numerator + denominator - 1) / denominator;
    timer_mod_ns(timer, now + MAX(1, duration));
    s->remaining[slot] = 0;
}

static void synchronize(ESP32S3UARTState *s, int changed)
{
    uint64_t old_num = s->bit_num, old_den = s->bit_den;
    bool autobaud_start = (changed == UART_SYNC_ALL || changed == 0x20) &&
                          !(CONF(s) & B(27)) && (REG(s, 0x20) & B(27));
    if (changed == UART_SYNC_ALL) {
        memcpy(s->core, s->regs, sizeof(s->core));
    } else if (changed >= 0) {
        CORE(s, changed) = REG(s, changed);
    }
    timing(s);
    if (autobaud_start) {
        REG(s, 0x28) = REG(s, 0x2c) = REG(s, 0x70) = REG(s, 0x74) = 4095;
        REG(s, 0x30) = 0;
        s->edge_ns = s->console_edge_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    }
    unsigned rx_size = ((CORE(s, 0x60) >> 1) & 7) * 128;
    unsigned tx_size = ((CORE(s, 0x60) >> 4) & 7) * 128;
    s->rx_capacity = MIN(rx_size, 512 - s->index * 128);
    s->tx_capacity = MIN(tx_size, 1024 - s->index * 128);
    if (s->fifo_shared) {
        if (s->parent.rx_fifo.capacity != s->rx_capacity) {
            fifo_reset(s, false);
            s->parent.rx_fifo.capacity = s->rx_capacity;
        }
        if (s->parent.tx_fifo.capacity != s->tx_capacity) {
            fifo_reset(s, true);
            s->parent.tx_fifo.capacity = s->tx_capacity;
        }
    }
    if (!(CONF(s) & B(8))) {
        s->break_sent = false;
    }
    if (CONF(s) & B(17)) {
        fifo_reset(s, false);
    }
    if (CONF(s) & B(18)) {
        fifo_reset(s, true);
    }
    if (CORE(s, 0x78) & (B(23) | B(26))) {
        fifo_reset(s, true);
    }
    if (CORE(s, 0x78) & (B(23) | B(27))) {
        fifo_reset(s, false);
    }
    retime(s, &s->tx_timer, 0, s->tx_active || s->break_active || s->tx_idle_active,
           running(s, true), old_num, old_den);
    retime(s, &s->rx_timer, 1, s->rx_active, running(s, false), old_num, old_den);
    retime(s, &s->console_timer, 2, s->console_active,
           running(s, false), old_num, old_den);
    retime(s, &s->timeout_timer, 3,
           (CORE(s, 0x24) & B(23)) && fifo8_num_used(&s->parent.rx_fifo) != 0,
           running(s, false), old_num, old_den);
    retime(s, &s->at_timer, 4, s->at_pending, running(s, false), old_num, old_den);
    retime(s, &s->idle_timer, 5, timer_pending(&s->idle_timer) || s->remaining[5],
           running(s, false), old_num, old_den);
    if (timer_pending(&s->tx_timer) && s->tx_active && s->tx_wait_phase == UART_WAIT_NONE) {
        uint64_t offset = s->tx_irda ?
            after_sixteenths(s, 0, s->tx_next_tick) :
            after_bits(s, 0, s->tx_next_tick);
        s->tx_origin = timer_expire_time_ns(&s->tx_timer) - offset;
    }
    if (timer_pending(&s->rx_timer)) {
        s->rx_origin = timer_expire_time_ns(&s->rx_timer) -
                       after_bits(s, 0, s->rx_next_half);
    }
    drive(s, s->tx_active || s->break_active ? s->tx_level :
             (CONF(s) & B(16)) ? !!(CONF(s) & B(12)) : true);
    start_tx(s);
    if (running(s, false)) {
        if (!timer_pending(&s->timeout_timer) && !s->remaining[3]) {
            arm_timeout(s);
        }
        if (fifo8_num_used(&s->console_rx) && !s->console_active) {
            console_cb(s);
        }
    }
    update(s);
}

static void uart_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    ESP32S3UARTState *s = opaque;
    if (addr >= sizeof(s->regs) || (addr & 3)) {
        return;
    }
    switch (addr) {
    case 0:
        esp32s3_uart_dma_write(s, value);
        return;
    case 4: case 8: case 0x1c: case 0x28: case 0x2c: case 0x30:
    case 0x64: case 0x68: case 0x6c: case 0x70: case 0x74:
        return;
    case 0x10:
        REG(s, 4) &= ~value;
        if (value & IRQ_TIMEOUT) {
            arm_timeout(s);
        }
        update(s);
        return;
    case 0x80:
        REG(s, addr) = value & ~B(31);
        if (value & (B(30) | B(31))) {
            synchronize(s, UART_SYNC_ALL);
        }
        return;
    default:
        REG(s, addr) = value;
        if (addr == 12) {
            update(s);
        } else if (REG(s, 0x80) & B(30)) {
            synchronize(s, addr);
        }
    }
}

static void reset_hold(Object *obj, ResetType type)
{
    ESP32S3UARTState *s = ESP32S3_UART(obj);
    ESP32S3UARTClass *klass = ESP32S3_UART_GET_CLASS(obj);
    if (s->parent.soc_reset && !esp32s3_reset_covers_periph(s->parent.soc_reset)) {
        return;
    }
    if (klass->parent_phases.hold) {
        klass->parent_phases.hold(obj, type);
    }
    memset(s->regs, 0, sizeof(s->regs));
    REG(s, 0x14) = 694;
    REG(s, 0x18) = 8;
    REG(s, 0x20) = B(25) | B(28) | 0x1c;
    REG(s, 0x24) = 96 | (96 << 10);
    REG(s, 0x28) = REG(s, 0x2c) = REG(s, 0x70) = REG(s, 0x74) = 4095;
    REG(s, 0x44) = 10;
    REG(s, 0x48) = 256 | (256 << 10);
    REG(s, 0x50) = REG(s, 0x54) = 2305;
    REG(s, 0x58) = 11;
    REG(s, 0x5c) = 43 | (3 << 8);
    REG(s, 0x60) = 2 | 16 | (10 << 17);
    REG(s, 0x78) = (1 << 12) | (3 << 20) | B(22) | B(24) | B(25);
    REG(s, 0x7c) = 0x02008270;
    REG(s, 0x80) = 1280 | B(30);
    timer_del(&s->tx_timer);
    timer_del(&s->rx_timer);
    timer_del(&s->timeout_timer);
    timer_del(&s->at_timer);
    timer_del(&s->console_timer);
    timer_del(&s->idle_timer);
    fifo8_reset(&s->console_rx);
    s->console_active = s->tx_active = s->rx_active = s->break_active = false;
    s->tx_idle_active = false;
    s->tx_wait_phase = UART_WAIT_NONE;
    s->tx_post_delay = false;
    s->rx_valid = false;
    s->tx_level = true;
    s->at_count = 0;
    s->at_pending = false;
    s->rx_rptr = s->rx_wptr = s->tx_rptr = s->tx_wptr = 0;
    memset(s->remaining, 0, sizeof(s->remaining));
    s->sampling_idle = s->idle_waiting = false;
    s->break_sent = false;
    s->console_level = true;
    s->console_edge_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    synchronize(s, UART_SYNC_ALL);
}

static void gate_input(void *opaque, int n, int level)
{
    ESP32S3UARTState *s = opaque;
    s->gate = level != 0;
    synchronize(s, UART_REFRESH_CLOCKS);
}

static void reset_input(void *opaque, int n, int level)
{
    ESP32S3UARTState *s = opaque;
    s->held_reset = level != 0;
    if (level) {
        reset_hold(OBJECT(s), RESET_TYPE_COLD);
    } else {
        synchronize(s, UART_REFRESH_CLOCKS);
    }
}

static void clock_change(void *opaque, ClockEvent event)
{
    ESP32S3UARTState *s = opaque;
    synchronize(s, UART_REFRESH_CLOCKS);
}

static void realize(DeviceState *dev, Error **errp)
{
    ESP32S3UARTState *s = ESP32S3_UART(dev);
    ESP32S3UARTClass *klass = ESP32S3_UART_GET_CLASS(dev);
    if (s->index > 2 || !s->console_baud) {
        error_setg(errp, "ESP32-S3 UART needs index0..2 and nonzero console-baud");
        return;
    }
    klass->parent_realize(dev, errp);
    if (errp && *errp) {
        return;
    }
    /* The six FIFO windows alias the one physical 1024-byte RAM. Overlap
     * reaches the same bytes; expanded windows never gain private storage. */
    if (!s->fifo_memory) {
        error_setg(errp, "ESP32-S3 UART requires shared fifo-memory link");
        return;
    }
    fifo8_destroy(&s->parent.rx_fifo);
    fifo8_destroy(&s->parent.tx_fifo);
    s->parent.rx_fifo = (Fifo8) {
        .data = s->fifo_memory->bytes + 512 + s->index * 128,
        .capacity = 128,
    };
    s->parent.tx_fifo = (Fifo8) {
        .data = s->fifo_memory->bytes + s->index * 128,
        .capacity = 128,
    };
    s->fifo_shared = true;
    qemu_chr_fe_set_handlers(&s->parent.chr, console_can_receive, console_receive,
                            console_event, NULL, s, NULL, true);
    s->finalized = true;
}

static void init(Object *obj)
{
    ESP32S3UARTState *s = ESP32S3_UART(obj);
    /* Parent creates 0x7c MMIO; S3 extends through ID/REG_UPDATE at0x80. */
    memory_region_set_size(&s->parent.iomem, sizeof(s->regs));
    s->apb = qdev_init_clock_in(DEVICE(obj), "apb", clock_change, s, ClockUpdate);
    s->xtal = qdev_init_clock_in(DEVICE(obj), "xtal", clock_change, s, ClockUpdate);
    s->rc_fast = qdev_init_clock_in(DEVICE(obj), "rc-fast", clock_change, s, ClockUpdate);
    clock_set_hz(s->apb, 80000000);
    clock_set_hz(s->xtal, 40000000);
    clock_set_hz(s->rc_fast, 17500000);
    s->gate = true;
    timer_init_ns(&s->tx_timer, QEMU_CLOCK_VIRTUAL, tx_cb, s);
    timer_init_ns(&s->rx_timer, QEMU_CLOCK_VIRTUAL, rx_cb, s);
    timer_init_ns(&s->timeout_timer, QEMU_CLOCK_VIRTUAL, timeout_cb, s);
    timer_init_ns(&s->at_timer, QEMU_CLOCK_VIRTUAL, at_cb, s);
    timer_init_ns(&s->console_timer, QEMU_CLOCK_VIRTUAL, console_cb, s);
    timer_init_ns(&s->idle_timer, QEMU_CLOCK_VIRTUAL, idle_cb, s);
    fifo8_create(&s->console_rx, 1024);
    qdev_init_gpio_in_named(DEVICE(obj), gate_input, "clock-enable", 1);
    qdev_init_gpio_in_named(DEVICE(obj), reset_input, "reset", 1);
}

static void finalize(Object *obj)
{
    ESP32S3UARTState *s = ESP32S3_UART(obj);
    if (s->electrical) {
        esp32s3_electrical_unsubscribe(s->electrical, frame_notify, s);
    }
    timer_del(&s->tx_timer);
    timer_del(&s->rx_timer);
    timer_del(&s->timeout_timer);
    timer_del(&s->at_timer);
    timer_del(&s->console_timer);
    timer_del(&s->idle_timer);
    fifo8_destroy(&s->console_rx);
}

static Property properties[] = {
    DEFINE_PROP_UINT32("index", ESP32S3UARTState, index, 0),
    DEFINE_PROP_UINT32("console-baud", ESP32S3UARTState, console_baud, 115200),
    DEFINE_PROP_LINK("gpio", ESP32S3UARTState, gpio, TYPE_ESP32S3_GPIO, ESP32S3GPIOState *),
    DEFINE_PROP_LINK("fifo-memory", ESP32S3UARTState, fifo_memory,
                     TYPE_ESP32S3_UART_MEMORY, ESP32S3UARTMemory *),
    DEFINE_PROP_LINK("electrical", ESP32S3UARTState, electrical, TYPE_ESP32S3_ELECTRICAL, DeviceState *),
    DEFINE_PROP_END_OF_LIST(),
};

/* Trace selection is a diagnostic toggle: a device property would be rejected
 * once the controller is realized, so it is a class property. */
static bool trace_get(Object *obj, Error **errp)
{
    return ESP32S3_UART(obj)->trace;
}

static void trace_set(Object *obj, bool value, Error **errp)
{
    ESP32S3_UART(obj)->trace = value;
}

static void class_init(ObjectClass *oc, void *data)
{
    ESP32S3UARTClass *klass = ESP32S3_UART_CLASS(oc);
    ESP32UARTClass *parent = ESP32_UART_CLASS(oc);
    DeviceClass *dc = DEVICE_CLASS(oc);
    parent->defer_fifo_create = true;
    parent->uart_read = uart_read;
    parent->uart_write = uart_write;
    device_class_set_parent_realize(dc, realize, &klass->parent_realize);
    resettable_class_set_parent_phases(RESETTABLE_CLASS(oc), NULL, reset_hold,
                                      NULL, &klass->parent_phases);
    device_class_set_props(dc, properties);
    object_class_property_add_bool(oc, "trace", trace_get, trace_set);
    object_class_property_set_description(oc, "trace",
        "Emit per-edge physical TX/RX trace events");
}

static const TypeInfo info = {
    .name = TYPE_ESP32S3_UART,
    .parent = TYPE_ESP32_UART,
    .instance_size = sizeof(ESP32S3UARTState),
    .instance_init = init,
    .instance_finalize = finalize,
    .class_size = sizeof(ESP32S3UARTClass),
    .class_init = class_init,
};

static const TypeInfo memory_info = {
    .name = TYPE_ESP32S3_UART_MEMORY,
    .parent = TYPE_OBJECT,
    .instance_size = sizeof(ESP32S3UARTMemory),
};

static void register_types(void)
{
    type_register_static(&info);
    type_register_static(&memory_info);
}
type_init(register_types)
