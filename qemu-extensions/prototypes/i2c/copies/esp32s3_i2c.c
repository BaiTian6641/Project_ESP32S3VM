/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "hw/i2c/esp32s3_i2c.h"
#include "hw/irq.h"
#include "hw/qdev-clock.h"
#include "hw/qdev-properties.h"
#include "hw/xtensa/esp32s3_reset_domain.h"
#include "qemu/log.h"
#include "qemu/host-utils.h"

#define REG(s, a) ((s)->reg[(a) / 4])
#define RX_WM BIT(0)
#define TX_WM BIT(1)
#define RX_OVF BIT(2)
#define END_DETECT BIT(3)
#define BYTE_DONE BIT(4)
#define ARBITRATION BIT(5)
#define TX_UDF BIT(6)
#define COMPLETE BIT(7)
#define TIMEOUT BIT(8)
#define START_INT BIT(9)
#define NACK_INT BIT(10)
#define TX_OVF BIT(11)
#define RX_UDF BIT(12)
#define SCL_ST_TO BIT(13)
#define SCL_MAIN_ST_TO BIT(14)
#define DET_START BIT(15)
#define SLAVE_STRETCH BIT(16)
#define GENERAL_CALL BIT(17)
#define CMD_DONE BIT(31)
#define INT_MASK 0x7ffff

/* Master fast-path sub-phases for the per-bit address byte. The address is
 * transmitted bit by bit against the shared net so a competing master can
 * win arbitration; data bytes keep the timed byte-level fast path. */
#define ADDR_PHASE_LOW  5
#define ADDR_PHASE_RISE 6
#define ADDR_PHASE_NEXT 7
#define ADDR_PHASE_ACK  8

static void i2c_irq(ESP32S3I2CState *s)
{
    uint32_t conf = REG(s, 0x18);
    uint32_t raw = REG(s, 0x20) & ~(RX_WM | TX_WM);
    if (conf & BIT(14)) {
        if (fifo8_num_used(&s->rx) > (conf & 31)) {
            raw |= RX_WM;
        }
        if (fifo8_num_used(&s->tx) < ((conf >> 5) & 31)) {
            raw |= TX_WM;
        }
    }
    REG(s, 0x20) = raw;
    qemu_set_irq(s->irq, s->gate && !s->reset_asserted &&
                 (REG(s, 0x54) & BIT(21)) && !!(raw & REG(s, 0x28)));
}

static void drive(ESP32S3I2CState *s, bool sda, bool scl)
{
    s->sda = sda;
    s->scl = scl;
    if (s->provider.drive) {
        s->provider.drive(s->provider.opaque, s->controller, sda, scl);
    }
}

static void release_slave(ESP32S3I2CState *s)
{
    if (s->service && s->provider.slave_drive) {
        s->provider.slave_drive(s->provider.opaque, s->service, true, true);
    }
}

static void cancel(ESP32S3I2CState *s)
{
    S3I2CService *service = s->service;
    s->generation++;
    timer_del(s->timer);
    s->service = NULL;
    s->executing = s->protocol_open = s->waiting = false;
    s->start_armed = false;
    s->need_address = true;
    s->bit_phase = 0;
    s->addr_bit = 0;
    if (service) {
        s3_i2c_service_cancel(service);
        if (s->provider.slave_drive) {
            s->provider.slave_drive(s->provider.opaque, service, true, true);
        }
    }
    drive(s, true, true);
}

static uint64_t source_hz(ESP32S3I2CState *s)
{
    if (!s->gate || s->reset_asserted || !(REG(s, 0x54) & BIT(21))) {
        return 0;
    }
    return clock_get_hz((REG(s, 0x54) & BIT(20)) ? s->rc_fast : s->xtal);
}

static int64_t cycles_ns(ESP32S3I2CState *s, uint64_t cycles)
{
    uint64_t hz = source_hz(s);
    uint32_t div = REG(s, 0x54);
    uint64_t numerator = (div & 255) + 1;
    uint64_t a = (div >> 8) & 63, b = (div >> 14) & 63;
    /* Fraction is DIV_A / DIV_B in the pinned S3 register contract. */
    uint64_t denominator = b ? b : 1;
    numerator = numerator * denominator + (b ? a : 0);
    if (!hz) {
        return 0;
    }
    return MAX(1, muldiv64(cycles * numerator, 1000000000ULL,
                         hz * denominator));
}

static int64_t bit_ns(ESP32S3I2CState *s)
{
    uint32_t high = REG(s, 0x38);
    uint64_t cycles = (REG(s, 0) & 511) + 1 + (high & 511) +
                      ((high >> 9) & 127);
    return cycles_ns(s, cycles);
}

static int64_t scl_high_ns(ESP32S3I2CState *s)
{
    uint32_t high = REG(s, 0x38);
    return cycles_ns(s, (high & 511) + ((high >> 9) & 127));
}

static void schedule(ESP32S3I2CState *s, int64_t delay)
{
    timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + MAX(1, delay));
}

static void fail(ESP32S3I2CState *s, uint32_t irq)
{
    cancel(s);
    fifo8_reset(&s->rx);
    REG(s, 0x20) |= irq;
    i2c_irq(s);
}

static bool ready_lines(ESP32S3I2CState *s, int64_t now, bool idle,
                        bool *sampled_sda)
{
    bool sda = false, scl = false;
    uint64_t generation = s->generation;
    bool valid = s->provider.sample &&
                 s->provider.sample(s->provider.opaque, s->controller, &sda, &scl);
    if (s->generation != generation) {
        return false;
    }
    if (valid && scl && (!idle || sda)) {
        if (sampled_sda) {
            *sampled_sda = sda;
        }
        s->waiting = false;
        return true;
    }
    if (!s->waiting) {
        s->waiting = true;
        s->wait_start = now;
    }
    uint64_t cycles = 1ULL << (REG(s, 0x0c) & 31);
    int64_t timeout = cycles_ns(s, cycles);
    if ((REG(s, 0x0c) & BIT(5)) && now - s->wait_start >= timeout) {
        fail(s, TIMEOUT);
    } else {
        schedule(s, bit_ns(s));
    }
    return false;
}

static void advance_command(ESP32S3I2CState *s)
{
    REG(s, 0x58 + s->command_index * 4) |= CMD_DONE;
    s->command_index++;
    s->remaining = 0;
    s->bit_phase = 0;
}

/* --------------------------------------------------------------------- */
/* Native slave edge FSM. Every transition is derived from the electrically
 * published SDA/SCL levels of this controller's own routed pads; there is
 * no address-only shortcut and no second line store. Bits are counted 0..7
 * for the data bits and 9 for the acknowledge bit of every byte. */

static bool slave_ready(ESP32S3I2CState *s)
{
    return !(REG(s, 4) & BIT(4)) && s->gate && !s->reset_asserted &&
           source_hz(s) != 0;
}

static void slave_drive_lines(ESP32S3I2CState *s)
{
    drive(s, !s->slave_sda_low, !s->slave_stretch);
}

static void slave_raise(ESP32S3I2CState *s, uint32_t irq)
{
    REG(s, 0x20) |= irq;
    i2c_irq(s);
}

/* Full reset: configuration changes, gating and cancellation paths. */
static void slave_reset_fsm(ESP32S3I2CState *s)
{
    s->slave_state = S3_I2C_SLV_IDLE;
    s->slave_bit = 0;
    s->slave_byte = 0;
    s->slave_matched = false;
    s->slave_hdr_matched = false;
    s->slave_rw = false;
    s->slave_out_armed = false;
    s->slave_nacked = false;
    s->slave_sda_low = false;
    s->slave_sda_ack = false;
    s->slave_stretch = false;
    s->slave_cause = S3_I2C_STRETCH_NONE;
    s->slave_rw_point = 0;
    s->slave_filter_deadline = 0;
    s->slave_protect_deadline = 0;
    s->slave_low_since = 0;
    drive(s, true, true);
}

/* End of transfer (STOP or timeout): the received-data counter and the
 * direction remain readable for software until the next START. */
static void slave_end_transfer(ESP32S3I2CState *s)
{
    s->slave_state = S3_I2C_SLV_IDLE;
    s->slave_bit = 0;
    s->slave_byte = 0;
    s->slave_matched = false;
    s->slave_hdr_matched = false;
    s->slave_out_armed = false;
    s->slave_sda_low = false;
    s->slave_sda_ack = false;
    s->slave_stretch = false;
    s->slave_cause = S3_I2C_STRETCH_NONE;
    s->slave_protect_deadline = 0;
    drive(s, true, true);
}

/* Glitch filter: a raw level change becomes an accepted edge only after it
 * has persisted for the programmed module-clock cycles. Candidates that
 * return to the accepted level first are glitches and are discarded. */
static bool filter_level(ESP32S3I2CState *s, bool scl_line, bool raw,
                         int64_t now)
{
    uint32_t cfg = REG(s, 0x50);
    bool en = scl_line ? !!(cfg & BIT(8)) : !!(cfg & BIT(9));
    unsigned thres = scl_line ? (cfg & 15) : ((cfg >> 4) & 15);
    bool *accepted = scl_line ? &s->sl_scl : &s->sl_sda;
    bool *candidate = scl_line ? &s->sl_scl_raw : &s->sl_sda_raw;
    int64_t *since = scl_line ? &s->sl_scl_since : &s->sl_sda_since;
    if (!en || !thres) {
        *accepted = raw;
        return raw;
    }
    if (raw == *accepted) {
        /* The excursion ended before proving itself: reject the glitch. */
        *candidate = raw;
        return raw;
    }
    if (raw != *candidate) {
        *candidate = raw;
        *since = now;
        int64_t threshold = cycles_ns(s, thres);
        if (threshold <= 0) {
            *accepted = raw;
            return raw;
        }
        if (s->slave_filter_deadline < now + threshold) {
            s->slave_filter_deadline = now + threshold;
        }
        return *accepted;
    }
    /* The deadline timer is the acceptance authority for persisting
     * candidates; until then the accepted level is unchanged. */
    return *accepted;
}

static void slave_arm_out(ESP32S3I2CState *s)
{
    if (fifo8_is_empty(&s->tx)) {
        if (REG(s, 0x84) & BIT(10)) {
            s->slave_stretch = true;
            s->slave_cause = S3_I2C_STRETCH_TX;
            slave_raise(s, SLAVE_STRETCH);
            slave_drive_lines(s);
        } else {
            /* No stretching and no data: an unserved read sends 0xff. */
            s->slave_byte = 0xff;
            s->slave_bit = 0;
            s->slave_out_armed = true;
        }
        return;
    }
    s->slave_byte = fifo8_pop(&s->tx);
    if (REG(s, 4) & BIT(6)) {
        s->slave_byte = revbit8(s->slave_byte);
    }
    s->slave_bit = 0;
    s->slave_out_armed = true;
    i2c_irq(s);
}

/* Drive the acknowledge bit for the byte just completed (ninth bit). */
static void slave_ack_bit(ESP32S3I2CState *s, bool ack_low)
{
    s->slave_bit = 9;
    s->slave_sda_low = ack_low;
    s->slave_sda_ack = true;
    slave_drive_lines(s);
}

/* Completed received address byte (or the 10-bit second address byte). */
static void slave_byte_complete(ESP32S3I2CState *s)
{
    uint32_t ctr = REG(s, 4);
    uint32_t sadr = REG(s, 0x10);
    uint8_t ours = sadr & 0x7f;

    if (s->slave_state == S3_I2C_SLV_ADDR2) {
        /* 10-bit low address byte; the header already matched. */
        if (!s->slave_hdr_matched ||
            s->slave_byte != ((sadr >> 7) & 0xff)) {
            s->slave_state = S3_I2C_SLV_IDLE;
            return;
        }
        s->slave_matched = true;
        s->slave_rw = false;
        s->slave_state = S3_I2C_SLV_IN;
        slave_ack_bit(s, true);
        return;
    }

    uint8_t addr7 = s->slave_byte >> 1;
    bool rw = s->slave_byte & 1;

    if (!addr7 && (ctr & BIT(14))) {
        /* General call: write-only broadcast with the enable bit set. */
        s->slave_matched = true;
        s->slave_rw = false;
        slave_raise(s, GENERAL_CALL);
        s->slave_state = S3_I2C_SLV_IN;
        slave_ack_bit(s, true);
        return;
    }
    if ((sadr & BIT(31)) && (addr7 & 0x78) == 0x78 && addr7 == ours) {
        if (rw) {
            if (s->slave_matched) {
                /* Repeated START read header completes a 10-bit read. */
                s->slave_rw = true;
                s->slave_state = S3_I2C_SLV_OUT;
                s->slave_out_armed = false;
                if (REG(s, 0x84) & BIT(10)) {
                    s->slave_stretch = true;
                    s->slave_cause = S3_I2C_STRETCH_ADDR;
                    slave_raise(s, SLAVE_STRETCH);
                }
                slave_ack_bit(s, true);
                if (!s->slave_stretch) {
                    slave_arm_out(s);
                }
                return;
            }
            if (ctr & BIT(13)) {
                /* A 10-bit read without a matching write phase is rejected. */
                s->slave_state = S3_I2C_SLV_IDLE;
                return;
            }
        }
        /* 10-bit header: expect the second address byte next. */
        s->slave_hdr_matched = true;
        s->slave_state = S3_I2C_SLV_ADDR2;
        slave_ack_bit(s, true);
        return;
    }
    if (!(sadr & BIT(31)) && addr7 == ours) {
        s->slave_matched = true;
        if (rw) {
            s->slave_rw = true;
            s->slave_state = S3_I2C_SLV_OUT;
            s->slave_out_armed = false;
            if (REG(s, 0x84) & BIT(10)) {
                /* Stretch at address match until the TX side is served. */
                s->slave_stretch = true;
                s->slave_cause = S3_I2C_STRETCH_ADDR;
                slave_raise(s, SLAVE_STRETCH);
            }
            slave_ack_bit(s, true);
            if (!s->slave_stretch) {
                slave_arm_out(s);
            }
            return;
        }
        s->slave_rw = false;
        s->slave_state = S3_I2C_SLV_IN;
        slave_ack_bit(s, true);
        return;
    }
    /* Another device's address: stay silent and keep listening. */
    s->slave_state = S3_I2C_SLV_IDLE;
}

/* Completed received data byte (master writing to this slave). */
static void slave_in_byte_done(ESP32S3I2CState *s)
{
    uint8_t byte = s->slave_byte;
    if (REG(s, 4) & BIT(7)) {
        byte = revbit8(byte);
    }
    if (fifo8_is_full(&s->rx)) {
        slave_raise(s, RX_OVF);
        if (REG(s, 0x84) & BIT(10)) {
            s->slave_stretch = true;
            s->slave_cause = S3_I2C_STRETCH_RX;
            slave_raise(s, SLAVE_STRETCH);
        }
        /* RX_FULL_ACK_LEVEL selects the acknowledge level while full. */
        slave_ack_bit(s, !(REG(s, 4) & BIT(3)));
        return;
    }
    fifo8_push(&s->rx, byte);
    s->slave_rw_point++;
    i2c_irq(s);
    s->slave_byte = 0;
    slave_ack_bit(s, true);
}

static void slave_fsm(ESP32S3I2CState *s, bool prev_sda, bool prev_scl)
{
    bool sda = s->sl_sda, scl = s->sl_scl;

    if (prev_sda && !sda && scl) {
        /* START or repeated START: SDA falls while SCL is high. */
        if (s->slave_state == S3_I2C_SLV_IDLE) {
            s->slave_matched = false;
            s->slave_hdr_matched = false;
            s->slave_rw_point = 0;
        }
        s->slave_state = S3_I2C_SLV_ADDR;
        s->slave_bit = 0;
        s->slave_byte = 0;
        s->slave_sda_low = false;
        s->slave_nacked = false;
        slave_drive_lines(s);
        slave_raise(s, DET_START);
        return;
    }
    if (!prev_sda && sda && scl) {
        /* STOP: SDA rises while SCL is high. */
        if (s->slave_state != S3_I2C_SLV_IDLE || s->slave_stretch ||
            s->slave_matched) {
            slave_end_transfer(s);
            slave_raise(s, COMPLETE);
        }
        return;
    }
    if (prev_scl && !scl) {
        /* SCL falling edge. */
        if (s->slave_state == S3_I2C_SLV_ADDR ||
            s->slave_state == S3_I2C_SLV_ADDR2) {
            if (s->slave_bit == 8) {
                slave_byte_complete(s);
            } else if (s->slave_bit == 9 && s->slave_sda_ack) {
                s->slave_sda_ack = false;
                s->slave_sda_low = false;
                s->slave_bit = 0;
                s->slave_byte = 0;
                slave_drive_lines(s);
            }
            return;
        }
        if (s->slave_state == S3_I2C_SLV_IN) {
            if (s->slave_bit == 8) {
                slave_in_byte_done(s);
            } else if (s->slave_bit == 9 && s->slave_sda_ack) {
                s->slave_sda_ack = false;
                s->slave_sda_low = false;
                s->slave_bit = 0;
                s->slave_byte = 0;
                slave_drive_lines(s);
            }
            return;
        }
        if (s->slave_state == S3_I2C_SLV_OUT) {
            if (s->slave_sda_ack) {
                /* End of the address-byte acknowledge bit. */
                s->slave_sda_ack = false;
                s->slave_sda_low = false;
                if (s->slave_out_armed && s->slave_bit < 8) {
                    s->slave_sda_low =
                        !((s->slave_byte >> (7 - s->slave_bit)) & 1);
                }
                slave_drive_lines(s);
                return;
            }
            if (s->slave_bit == 8) {
                /* End of our data byte: open the master acknowledge window. */
                s->slave_bit = 9;
                s->slave_sda_low = false;
                slave_drive_lines(s);
                return;
            }
            if (s->slave_bit == 9) {
                /* The master finished acknowledging our byte. */
                if (s->slave_nacked) {
                    s->slave_state = S3_I2C_SLV_IDLE;
                    return;
                }
                slave_arm_out(s);
                if (s->slave_stretch || !s->slave_out_armed) {
                    return;
                }
                /* Drive the first bit of the newly served byte below. */
            } else if (!s->slave_out_armed) {
                return;
            }
            if (s->slave_out_armed && s->slave_bit < 8) {
                bool bit = (s->slave_byte >> (7 - s->slave_bit)) & 1;
                s->slave_sda_low = !bit;
                slave_drive_lines(s);
            }
        }
        return;
    }
    if (!prev_scl && scl) {
        /* SCL rising edge: sample. */
        switch (s->slave_state) {
        case S3_I2C_SLV_ADDR:
        case S3_I2C_SLV_ADDR2:
        case S3_I2C_SLV_IN:
            if (s->slave_bit < 8) {
                s->slave_byte = (s->slave_byte << 1) | sda;
                s->slave_bit++;
            }
            break;
        case S3_I2C_SLV_OUT:
            if (s->slave_sda_ack) {
                break;
            }
            if (s->slave_bit < 8) {
                s->slave_bit++;
            } else if (s->slave_bit == 9) {
                /* Master acknowledge (low) continues the read; NACK ends it. */
                s->slave_nacked = sda;
            }
            break;
        default:
            break;
        }
        return;
    }
}

/* Slave-mode deadline timer: glitch-filter acceptance, stretch protection
 * and the SCL-stuck-low watchdog are the only slave time authorities. */
static void slave_timer(ESP32S3I2CState *s, int64_t now)
{
    if (!slave_ready(s)) {
        slave_reset_fsm(s);
        return;
    }
    /* Accept glitch candidates whose persistence outlived the threshold;
     * an accepted edge advances the FSM exactly like a published frame. */
    if (s->slave_filter_deadline && now >= s->slave_filter_deadline) {
        s->slave_filter_deadline = 0;
        bool sda = false, scl = false;
        if (s->provider.sample &&
            s->provider.sample(s->provider.opaque, s->controller, &sda, &scl)) {
            bool prev_sda = s->sl_sda, prev_scl = s->sl_scl;
            if (sda != s->sl_sda && sda == s->sl_sda_raw) {
                s->sl_sda = sda;
            }
            if (scl != s->sl_scl && scl == s->sl_scl_raw) {
                s->sl_scl = scl;
            }
            if (prev_sda != s->sl_sda || prev_scl != s->sl_scl) {
                slave_fsm(s, prev_sda, prev_scl);
            }
        }
    }
    /* Stretch protection: release an over-held SCL and end the transfer. */
    if (s->slave_stretch && s->slave_protect_deadline &&
        now >= s->slave_protect_deadline) {
        bool was_in = s->slave_state != S3_I2C_SLV_IDLE;
        slave_end_transfer(s);
        if (was_in) {
            slave_raise(s, SCL_MAIN_ST_TO);
        }
    }
    /* SCL stuck low by the far side beyond SCL_ST_TIME_OUT. */
    if (s->slave_state != S3_I2C_SLV_IDLE && !s->sl_scl &&
        !s->slave_stretch && s->slave_low_since) {
        unsigned st = REG(s, 0x78) & 0x1f;
        if (st) {
            int64_t limit = cycles_ns(s, 1ULL << st);
            if (limit > 0 && now - s->slave_low_since >= limit) {
                slave_end_transfer(s);
                slave_raise(s, SCL_ST_TO);
            }
        }
    }
}

static void slave_rearm(ESP32S3I2CState *s, int64_t now)
{
    int64_t when = 0;
    if (s->slave_filter_deadline > now) {
        when = s->slave_filter_deadline;
    }
    if (s->slave_stretch) {
        if (!s->slave_protect_deadline) {
            unsigned protect = REG(s, 0x84) & 0x3ff;
            if (protect) {
                int64_t limit = cycles_ns(s, protect);
                if (limit > 0) {
                    s->slave_protect_deadline = now + limit;
                }
            }
        }
        if (s->slave_protect_deadline > now) {
            when = when ? MIN(when, s->slave_protect_deadline)
                        : s->slave_protect_deadline;
        }
    }
    if (when > now) {
        timer_mod(s->timer, when);
    }
}

void esp32s3_i2c_frame(ESP32S3I2CState *s)
{
    if (s->executing) {
        bool rising = s->bit_phase == ADDR_PHASE_RISE && s->waiting;
        if (rising || s->bit_phase == ADDR_PHASE_NEXT) {
            bool sda = false, scl = false;
            if (s->provider.sample &&
                s->provider.sample(s->provider.opaque, s->controller,
                                   &sda, &scl) &&
                (rising ? scl : !scl)) {
                schedule(s, 1);
            }
        }
        return;
    }
    if (!slave_ready(s)) {
        if (s->slave_state != S3_I2C_SLV_IDLE || s->slave_stretch) {
            slave_reset_fsm(s);
        }
        return;
    }
    bool sda = false, scl = false;
    if (!s->provider.sample ||
        !s->provider.sample(s->provider.opaque, s->controller, &sda, &scl)) {
        if (s->slave_state != S3_I2C_SLV_IDLE || s->slave_stretch) {
            /* Unknown routing or power cannot continue a live transfer. */
            slave_reset_fsm(s);
        }
        return;
    }
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    bool prev_sda = s->sl_sda, prev_scl = s->sl_scl;
    s->sl_sda = filter_level(s, false, sda, now);
    s->sl_scl = filter_level(s, true, scl, now);
    if (prev_scl && !s->sl_scl) {
        /* Track when the accepted SCL level last fell. */
        s->slave_low_since = now;
    }
    slave_fsm(s, prev_sda, prev_scl);
    slave_rearm(s, now);
}

/* Release an active stretch once the missing resource is available. */
static void slave_kick(ESP32S3I2CState *s)
{
    if (!s->slave_stretch) {
        return;
    }
    switch (s->slave_cause) {
    case S3_I2C_STRETCH_ADDR:
    case S3_I2C_STRETCH_TX:
        if (!fifo8_is_empty(&s->tx)) {
            s->slave_stretch = false;
            s->slave_cause = S3_I2C_STRETCH_NONE;
            s->slave_protect_deadline = 0;
            if (s->slave_state == S3_I2C_SLV_OUT && !s->slave_out_armed) {
                slave_arm_out(s);
            }
            slave_drive_lines(s);
        }
        break;
    case S3_I2C_STRETCH_RX:
        if (!fifo8_is_full(&s->rx)) {
            s->slave_stretch = false;
            s->slave_cause = S3_I2C_STRETCH_NONE;
            s->slave_protect_deadline = 0;
            slave_drive_lines(s);
        }
        break;
    default:
        break;
    }
}

/* --------------------------------------------------------------------- */
/* Timed transaction fast path: every byte samples the same resolved pads and
 * endpoint graph as routing, never a controller/address response cache. This
 * path is not an edge/fast-equivalence or glitch-filter qualification. */
static void step(void *opaque)
{
    ESP32S3I2CState *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint64_t generation = s->generation;
    if (!s->executing) {
        slave_timer(s, now);
        return;
    }
    if (!source_hz(s)) {
        return;
    }
    if (s->command_index >= 8) {
        fail(s, TIMEOUT);
        return;
    }
    uint32_t cmd = s->active_cmd[s->command_index];
    unsigned opcode = (cmd >> 11) & 7;
    if (s->bit_phase == 1) {
        drive(s, true, true);
        if (s->generation != generation) {
            return;
        }
        if (!ready_lines(s, now, true, NULL)) {
            return;
        }
        s->bit_phase = 2;
        if (opcode == 6) {
            schedule(s, cycles_ns(s, REG(s, 0x44) & 511));
            return;
        }
    }
    if (opcode == 6) {
        if (s->protocol_open && !s->bit_phase) {
            drive(s, true, false);
            if (s->generation != generation) {
                return;
            }
            s->bit_phase = 1;
            schedule(s, cycles_ns(s, (REG(s, 0) & 511) + 1));
            return;
        }
        if (!ready_lines(s, now, !s->start_armed, NULL)) {
            return;
        }
        s->start_armed = false;
        s->restart = s->protocol_open;
        s->need_address = true;
        s->protocol_open = true;
        drive(s, false, true);
        if (s->generation != generation) {
            return;
        }
        advance_command(s);
        schedule(s, cycles_ns(s, (REG(s, 0x40) & 511) + 1 +
                                      (REG(s, 0x44) & 511)));
        return;
    }
    if (opcode == 2 || opcode == 4) {
        if (opcode == 2 && !s->protocol_open &&
            !ready_lines(s, now, true, NULL)) {
            return;
        }
        if (opcode == 2 && !s->bit_phase) {
            drive(s, false, false);
            if (s->generation != generation) {
                return;
            }
            s->bit_phase = 1;
            schedule(s, cycles_ns(s, (REG(s, 0x48) & 511) +
                                      (REG(s, 0x4c) & 511)));
            return;
        }
        advance_command(s);
        s->executing = false;
        if (opcode == 2) {
            if (s->service) {
                s3_i2c_service_stop(s->service, now);
            }
            release_slave(s);
            if (s->generation != generation) {
                return;
            }
            s->service = NULL;
            s->protocol_open = false;
            s->need_address = true;
            REG(s, 0x20) |= COMPLETE;
        } else {
            REG(s, 0x20) |= END_DETECT;
        }
        i2c_irq(s);
        return;
    }
    if (opcode != 1 && opcode != 3) {
        fail(s, TIMEOUT);
        return;
    }
    if (!s->remaining) {
        s->remaining = cmd & 255;
        if (!s->remaining) {
            advance_command(s);
            schedule(s, 1);
            return;
        }
    }
    /* Per-bit address phase: a released-high SDA read low on the shared net
     * is a lost arbitration against a competing master. */
    if (opcode == 1 && s->need_address && s->bit_phase >= ADDR_PHASE_LOW &&
        s->bit_phase <= ADDR_PHASE_ACK) {
        if (s->bit_phase == ADDR_PHASE_LOW) {
            bool bit = (s->addr_byte >> (7 - s->addr_bit)) & 1;
            drive(s, bit, false);
            if (s->generation != generation) {
                return;
            }
            s->bit_phase = ADDR_PHASE_RISE;
            schedule(s, cycles_ns(s, (REG(s, 0) & 511) + 1));
            return;
        }
        if (s->bit_phase == ADDR_PHASE_RISE) {
            bool sda = false;
            drive(s, s->sda, true);
            if (s->generation != generation ||
                !ready_lines(s, now, false, &sda)) {
                return;
            }
            if ((REG(s, 4) & BIT(9)) && s->sda && !sda) {
                fail(s, ARBITRATION);
                return;
            }
            s->bit_phase = ADDR_PHASE_NEXT;
            schedule(s, scl_high_ns(s));
            return;
        }
        if (s->bit_phase == ADDR_PHASE_NEXT) {
            s->addr_bit++;
            if (s->addr_bit < 8) {
                drive(s, true, false);
                if (s->generation != generation) {
                    return;
                }
                s->bit_phase = ADDR_PHASE_LOW;
                schedule(s, 1);
                return;
            }
            /* Ninth bit: the addressed endpoint acknowledge window. */
            drive(s, true, false);
            if (s->generation != generation) {
                return;
            }
            s->bit_phase = ADDR_PHASE_ACK;
            schedule(s, cycles_ns(s, (REG(s, 0) & 511) + 1));
            return;
        }
        /* ADDR_PHASE_ACK: the service-level acknowledge is modeled below;
         * the line stays released for the ninth clock. */
        drive(s, true, true);
        if (s->generation != generation) {
            return;
        }
        s->bit_phase = 2;
        schedule(s, scl_high_ns(s));
        return;
    }
    if (!s->bit_phase) {
        if (!s->protocol_open && !ready_lines(s, now, true, NULL)) {
            return;
        }
        if (opcode == 1 && fifo8_is_empty(&s->tx)) {
            fail(s, TX_UDF);
            return;
        }
        if (opcode == 3 && fifo8_is_full(&s->rx)) {
            fail(s, RX_OVF);
            return;
        }
        if (opcode == 1 && s->need_address) {
            /* The address byte is transmitted bit by bit (see above). */
            s->byte = fifo8_pop(&s->tx);
            if (REG(s, 4) & BIT(6)) {
                s->byte = revbit8(s->byte);
            }
            s->addr_byte = s->byte;
            s->addr_bit = 0;
            s->bit_phase = ADDR_PHASE_LOW;
            schedule(s, 1);
            return;
        }
        /* Hold-master conversion stretches this byte in virtual time. */
        if (opcode == 3 && s->service) {
            int64_t ready = s3_i2c_service_ready_ns(s->service);
            if (ready > now) {
                uint64_t limit = 1ULL << (REG(s, 0x0c) & 31);
                int64_t timeout = cycles_ns(s, limit);
                if ((REG(s, 0x0c) & BIT(5)) && ready - now > timeout) {
                    s->waiting = true;
                    s->wait_start = now;
                    s->bit_phase = 3;
                    schedule(s, timeout);
                } else {
                    s->bit_phase = 4;
                    schedule(s, ready - now);
                }
                if (s->provider.slave_drive) {
                    s->provider.slave_drive(s->provider.opaque, s->service, true, false);
                }
                return;
            }
        }
        drive(s, true, false);
        if (s->generation != generation) {
            return;
        }
        s->bit_phase = 1;
        schedule(s, bit_ns(s) * 9);
        return;
    }
    if (s->bit_phase == 3) {
        fail(s, TIMEOUT);
        return;
    }
    if (s->bit_phase == 4) {
        release_slave(s);
        if (s->generation != generation) {
            return;
        }
        s->bit_phase = 0;
        schedule(s, 1);
        return;
    }
    bool ack = false;
    if (!s->need_address && s->service) {
        bool collision = false;
        S3I2CService *reachable = s->provider.resolve ?
            s->provider.resolve(s->provider.opaque, s->controller, s->address,
                                &collision) : NULL;
        if (s->generation != generation) {
            return;
        }
        if (collision || reachable != s->service) {
            fail(s, collision ? ARBITRATION : NACK_INT);
            return;
        }
    }
    if (opcode == 1) {
        if (s->need_address) {
            /* The address byte was already consumed bit by bit. */
            s->byte = s->addr_byte;
            bool collision = false;
            release_slave(s);
            if (s->generation != generation) {
                return;
            }
            s->address = s->byte >> 1;
            S3I2CService *candidate = s->provider.resolve ?
                s->provider.resolve(s->provider.opaque, s->controller, s->byte >> 1,
                                    &collision) : NULL;
            if (s->generation != generation) {
                return;
            }
            s->service = candidate;
            if (collision) {
                fail(s, ARBITRATION);
                return;
            }
            s->reading = s->byte & 1;
            ack = s->service && s3_i2c_service_address(s->service, s->reading,
                                                       s->restart, now);
            s->need_address = false;
        } else {
            s->byte = fifo8_pop(&s->tx);
            if (REG(s, 4) & BIT(6)) {
                s->byte = revbit8(s->byte);
            }
            ack = s->service && !s->reading &&
                  s3_i2c_service_write(s->service, s->byte, now);
        }
        s->nack = !ack;
        if ((cmd & BIT(8)) && s->nack != !!(cmd & BIT(9))) {
            fail(s, NACK_INT);
            return;
        }
    } else {
        if (!s->service || !s->reading ||
            !s3_i2c_service_read(s->service, &s->byte, now)) {
            fail(s, NACK_INT);
            return;
        }
        fifo8_push(&s->rx, (REG(s, 4) & BIT(7)) ? revbit8(s->byte) : s->byte);
        s3_i2c_service_read_ack(s->service, !!(cmd & BIT(10)));
    }
    REG(s, 0x20) |= BYTE_DONE;
    if (!--s->remaining) {
        advance_command(s);
    } else {
        s->bit_phase = 0;
    }
    i2c_irq(s);
    schedule(s, 1);
}

static uint64_t read_reg(void *opaque, hwaddr addr, unsigned size)
{
    ESP32S3I2CState *s = opaque;
    if (addr == 8) {
        unsigned main_state, scl_state;
        bool busy = s->executing || s->slave_state != S3_I2C_SLV_IDLE;
        if (s->executing) {
            main_state = s->need_address ? 1 : s->reading ? 3 : 4;
        } else {
            main_state = (s->slave_state == S3_I2C_SLV_ADDR ||
                          s->slave_state == S3_I2C_SLV_ADDR2) ? 1 :
                         s->slave_state == S3_I2C_SLV_IN ? 3 :
                         s->slave_state == S3_I2C_SLV_OUT ? 4 : 0;
        }
        scl_state = !busy ? 0 : s->scl ? 5 : 3;
        return s->nack | (s->slave_rw << 1) |
               (!!(REG(s, 0x20) & ARBITRATION) << 3) |
               (busy << 4) | (s->slave_matched << 5) |
               (fifo8_num_used(&s->rx) << 8) |
               ((s->slave_stretch ? s->slave_cause : S3_I2C_STRETCH_NONE) << 14) |
               (fifo8_num_used(&s->tx) << 18) |
               (main_state << 24) | (scl_state << 28);
    }
    if (addr == 0x14) {
        return s->rx.head | (((s->rx.head + s->rx.num) & 31) << 5) |
               (s->tx.head << 10) | (((s->tx.head + s->tx.num) & 31) << 15) |
               ((s->slave_rw_point & 0xff) << 22);
    }
    if (addr == 0x1c) {
        if (fifo8_is_empty(&s->rx)) {
            REG(s, 0x20) |= RX_UDF;
            i2c_irq(s);
            return 0;
        }
        uint8_t value = fifo8_pop(&s->rx);
        slave_kick(s);
        i2c_irq(s);
        return value;
    }
    if (addr == 0x20 || addr == 0x2c) {
        i2c_irq(s);
        return addr == 0x20 ? REG(s, 0x20) : REG(s, 0x20) & REG(s, 0x28);
    }
    if (addr == 0x24) {
        return 0;
    }
    return REG(s, addr);
}

static void write_reg(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    ESP32S3I2CState *s = opaque;
    if (s->reset_asserted) {
        return;
    }
    switch (addr) {
    case 8: case 0x14: case 0x2c:
        return;
    case 0x20: case 0x24:
        REG(s, 0x20) &= ~(value & INT_MASK);
        break;
    case 0x28:
        REG(s, addr) = value & INT_MASK;
        break;
    case 0x1c:
        if (fifo8_is_full(&s->tx)) {
            REG(s, 0x20) |= TX_OVF;
        } else {
            fifo8_push(&s->tx, value);
            slave_kick(s);
        }
        break;
    case 0x18:
        if (value & BIT(12)) {
            fifo8_reset(&s->rx);
        }
        if (value & BIT(13)) {
            fifo8_reset(&s->tx);
        }
        REG(s, addr) = value & ~(BIT(12) | BIT(13));
        break;
    case 0x10:
        REG(s, addr) = value & (BIT(31) | 0x7fff);
        break;
    case 0x50:
        REG(s, addr) = value & 0x3ff;
        s->sl_sda_raw = s->sl_sda;
        s->sl_scl_raw = s->sl_scl;
        break;
    case 0x84:
        if (value & BIT(11)) {
            /* slave_scl_stretch_clr: release the current stretch. */
            s->slave_stretch = false;
            s->slave_cause = S3_I2C_STRETCH_NONE;
            s->slave_protect_deadline = 0;
            if (s->slave_state == S3_I2C_SLV_OUT && !s->slave_out_armed) {
                slave_arm_out(s);
            }
            slave_drive_lines(s);
        }
        REG(s, addr) = value & ~BIT(11) & 0x3fff;
        break;
    case 4:
        if (value & BIT(10)) {
            cancel(s);
        }
        if ((value & BIT(4)) && !(REG(s, addr) & BIT(4)) &&
            s->slave_state != S3_I2C_SLV_IDLE) {
            /* Leaving slave mode abandons any live slave transfer. */
            slave_reset_fsm(s);
        }
        REG(s, addr) = value & ~(BIT(5) | BIT(10) | BIT(11));
        drive(s, s->sda, s->scl);
        if ((value & BIT(5)) && !s->executing && (value & BIT(4)) && source_hz(s)) {
            memcpy(s->active_cmd, &s->reg[0x58 / 4], sizeof(s->active_cmd));
            for (unsigned i = 0; i < 8; i++) {
                REG(s, 0x58 + i * 4) &= ~CMD_DONE;
            }
            s->executing = true;
            bool sda = false, scl = false;
            s->start_armed = ((s->active_cmd[0] >> 11) & 7) == 6 &&
                s->provider.sample &&
                s->provider.sample(s->provider.opaque, s->controller,
                                   &sda, &scl) && sda && scl;
            s->command_index = s->remaining = s->bit_phase = 0;
            REG(s, 0x20) |= START_INT;
            schedule(s, 1);
        }
        break;
    case 0x54:
        value &= 0x3fffff;
        if (REG(s, addr) != value) {
            cancel(s);
            if (s->slave_state != S3_I2C_SLV_IDLE) {
                slave_reset_fsm(s);
            }
            REG(s, 0x20) = 0;
            REG(s, 0x28) = 0;
        }
        REG(s, addr) = value & 0x3fffff;
        drive(s, s->sda, s->scl);
        break;
    default:
        REG(s, addr) = value;
        break;
    }
    i2c_irq(s);
}

static void reset_registers(ESP32S3I2CState *s)
{
    cancel(s);
    fifo8_reset(&s->tx);
    fifo8_reset(&s->rx);
    memset(s->reg, 0, sizeof(s->reg));
    REG(s, 4) = 3;
    REG(s, 0x0c) = 16;
    REG(s, 0x18) = 11 | (4 << 5) | BIT(14);
    REG(s, 0x40) = REG(s, 0x44) = REG(s, 0x48) = REG(s, 0x4c) = 8;
    REG(s, 0x50) = BIT(8) | BIT(9);
    REG(s, 0x54) = BIT(21);
    REG(s, 0x78) = REG(s, 0x7c) = 16;
    REG(s, 0xf8) = 537330177;
    s->need_address = true;
    s->nack = false;
    s->slave_nacked = false;
    slave_reset_fsm(s);
    s->sl_sda = s->sl_scl = true;
    i2c_irq(s);
}

static void reset_hold(Object *obj, ResetType type)
{
    ESP32S3I2CState *s = ESP32S3_I2C(obj);
    if (!s->soc_reset || esp32s3_reset_covers_periph(s->soc_reset)) {
        reset_registers(s);
    }
}

static void gate_input(void *opaque, int n, int level)
{
    ESP32S3I2CState *s = opaque;
    s->gate = !!level;
    if (!level) {
        cancel(s);
        if (s->slave_state != S3_I2C_SLV_IDLE || s->slave_stretch) {
            slave_reset_fsm(s);
        }
        REG(s, 0x20) = 0;
        i2c_irq(s);
    } else {
        drive(s, s->sda, s->scl);
        i2c_irq(s);
    }
}

static void reset_input(void *opaque, int n, int level)
{
    ESP32S3I2CState *s = opaque;
    if (level) {
        reset_registers(s);
    }
    s->reset_asserted = !!level;
    drive(s, s->sda, s->scl);
}

static void clock_changed(ESP32S3I2CState *s)
{
    if (s->executing || s->protocol_open) {
        cancel(s);
        REG(s, 0x20) = 0;
        REG(s, 0x28) = 0;
        i2c_irq(s);
    } else {
        if (s->slave_state != S3_I2C_SLV_IDLE || s->slave_stretch) {
            slave_reset_fsm(s);
            s->sl_sda = s->sl_scl = true;
        }
        drive(s, s->sda, s->scl);
    }
}

static void xtal_changed(void *opaque, ClockEvent event)
{
    ESP32S3I2CState *s = opaque;
    if (!(REG(s, 0x54) & BIT(20))) {
        clock_changed(s);
    }
}

static void rc_fast_changed(void *opaque, ClockEvent event)
{
    ESP32S3I2CState *s = opaque;
    if (REG(s, 0x54) & BIT(20)) {
        clock_changed(s);
    }
}

void esp32s3_i2c_invalidate(ESP32S3I2CState *s)
{
    fail(s, NACK_INT);
}

void esp32s3_i2c_bind(ESP32S3I2CState *s, const S3I2CProvider *provider)
{
    cancel(s);
    if (s->slave_state != S3_I2C_SLV_IDLE || s->slave_stretch) {
        slave_reset_fsm(s);
    }
    s->provider = *provider;
    drive(s, true, true);
}

static const MemoryRegionOps ops = {
    .read = read_reg, .write = write_reg, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4, .unaligned = false },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static const Property properties[] = {
    DEFINE_PROP_UINT32("controller", ESP32S3I2CState, controller, 0),
    DEFINE_PROP_LINK("soc-reset", ESP32S3I2CState, soc_reset, TYPE_DEVICE, DeviceState *),
    DEFINE_PROP_END_OF_LIST(),
};

static void init(Object *obj)
{
    ESP32S3I2CState *s = ESP32S3_I2C(obj);
    memory_region_init_io(&s->iomem, obj, &ops, s, TYPE_ESP32S3_I2C, 0x200);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    fifo8_create(&s->tx, 32);
    fifo8_create(&s->rx, 32);
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, step, s);
    s->xtal = qdev_init_clock_in(DEVICE(obj), "xtal-clk", xtal_changed, s, ClockUpdate);
    s->rc_fast = qdev_init_clock_in(DEVICE(obj), "rc-fast-clk", rc_fast_changed, s, ClockUpdate);
    qdev_init_gpio_in_named(DEVICE(obj), gate_input, "clk-gate", 1);
    qdev_init_gpio_in_named(DEVICE(obj), reset_input, "reset", 1);
    s->slave_cause = S3_I2C_STRETCH_NONE;
    s->sl_sda = s->sl_scl = true;
}

static void finalize(Object *obj)
{
    ESP32S3I2CState *s = ESP32S3_I2C(obj);
    timer_free(s->timer);
    fifo8_destroy(&s->rx);
    fifo8_destroy(&s->tx);
}

static void class_init(ObjectClass *klass, void *data)
{
    device_class_set_props(DEVICE_CLASS(klass), properties);
    RESETTABLE_CLASS(klass)->phases.hold = reset_hold;
}

static const TypeInfo type = {
    .name = TYPE_ESP32S3_I2C, .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ESP32S3I2CState), .instance_init = init,
    .instance_finalize = finalize, .class_init = class_init,
};
static void register_types(void)
{
    type_register_static(&type);
}
type_init(register_types)
