/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Functional powered demodulated NEC source, not RMT/receiver demodulation.
 * Vishay NEC format (leader, byte/complement pairs and whole-frame variant):
 * https://www.vishay.com/docs/80071/dataform.pdf, pp. 2-3.
 * Active-low demodulated OUT and separate VS/GND/OUT receiver terminals:
 * https://www.vishay.com/docs/82491/tsop382.pdf, pp. 1-3.
 * The explicit functional profile uses 560/1690 us, not a claim about optical
 * carrier, AGC, receiver tolerances, or measured silicon pulse metrology.
 */
#include "qemu/osdep.h"
#include "hw/timer/esp32s3_rmt_nec.h"

#define NEC_FIRST_DELAY_NS UINT64_C(100000000)
#define NEC_LEAD_MARK_NS   UINT64_C(9000000)
#define NEC_LEAD_SPACE_NS  UINT64_C(4500000)
#define NEC_BIT_MARK_NS    UINT64_C(560000)
#define NEC_ZERO_SPACE_NS  UINT64_C(560000)
#define NEC_ONE_SPACE_NS   UINT64_C(1690000)
/* Complement pairs always contain sixteen zero bits and sixteen one bits. */
#define NEC_FRAME_NS       UINT64_C(67980000)

static bool nec_add_ns(uint64_t base, uint64_t delta, uint64_t *result)
{
    if (base > INT64_MAX || delta > (uint64_t)INT64_MAX - base) {
        return false;
    }
    *result = base + delta;
    return true;
}

static void nec_idle(Esp32S3RmtNec *s)
{
    s->phase = ESP32S3_RMT_NEC_WAIT;
    s->bit_index = 0;
    s->level = true;
    s->next_deadline_ns = -1;
}

static void nec_anchor(Esp32S3RmtNec *s, uint64_t base_ns)
{
    s->anchor_valid = nec_add_ns(base_ns, NEC_FIRST_DELAY_NS,
                                 &s->first_frame_ns);
}

/* Division jumps over arbitrarily many missed frames without a catch-up loop.
 * Check the product before forming the absolute deadline, and reserve room
 * for the whole frame, so no terminal can become stuck in a final low mark. */
static void nec_schedule(Esp32S3RmtNec *s, uint64_t now_ns, bool allow_now)
{
    const uint64_t latest_start = (uint64_t)INT64_MAX - NEC_FRAME_NS;
    uint64_t periods = 0;

    s->next_deadline_ns = -1;
    if (!s->configured || !s->anchor_valid || now_ns > INT64_MAX ||
        s->first_frame_ns > latest_start) {
        return;
    }
    if (now_ns > s->first_frame_ns ||
        (now_ns == s->first_frame_ns && !allow_now)) {
        periods = (now_ns - s->first_frame_ns) / s->period_ns + 1;
    }
    if (periods > (latest_start - s->first_frame_ns) / s->period_ns) {
        return;
    }
    s->next_deadline_ns = s->first_frame_ns + periods * s->period_ns;
}

static void nec_count(uint64_t *count)
{
    if (*count != UINT64_MAX) {
        (*count)++;
    }
}

static void nec_late_abort(Esp32S3RmtNec *s, uint64_t now_ns)
{
    nec_count(&s->frame_abort_count);
    nec_idle(s);
    nec_schedule(s, now_ns, false);
}

static Esp32S3RmtNecOutput nec_output(const Esp32S3RmtNec *s)
{
    return (Esp32S3RmtNecOutput) {
        .output_enable = s->configured && s->power_known && s->powered,
        .level = s->level,
        .deadline_ns = s->next_deadline_ns,
    };
}

bool esp32s3_rmt_nec_init(Esp32S3RmtNec *s, uint32_t address,
                         uint32_t command, uint32_t period_ms)
{
    *s = (Esp32S3RmtNec) {
        .next_deadline_ns = -1,
        .level = true,
    };
    return esp32s3_rmt_nec_configure(s, address, command, period_ms, 0);
}

bool esp32s3_rmt_nec_configure(Esp32S3RmtNec *s, uint32_t address,
                              uint32_t command, uint32_t period_ms,
                              uint64_t now_ns)
{
    if (address > 255 || command > 255 ||
        period_ms < 100 || period_ms > 60000) {
        return false;
    }
    if (s->configured && s->address == address && s->command == command &&
        s->period_ms == period_ms) {
        return true;
    }

    nec_idle(s);
    s->address = address;
    s->command = command;
    s->period_ms = period_ms;
    s->period_ns = (uint64_t)period_ms * UINT64_C(1000000);
    s->frame_word = address | ((address ^ 255u) << 8) |
                    (command << 16) | ((command ^ 255u) << 24);
    s->configured = true;
    if (s->have_epoch) {
        nec_anchor(s, now_ns);
        if (s->power_known && s->powered) {
            nec_schedule(s, now_ns, false);
        }
    }
    return true;
}

void esp32s3_rmt_nec_power(Esp32S3RmtNec *s, bool known, bool powered,
                          uint64_t power_on_ns, uint64_t epoch,
                          uint64_t now_ns)
{
    bool was_available = s->power_known && s->powered;
    bool new_epoch = known && (!s->have_epoch || s->power_epoch != epoch);

    s->power_known = known;
    s->powered = known && powered;
    if (new_epoch) {
        s->have_epoch = true;
        s->power_on_ns = power_on_ns;
        s->power_epoch = epoch;
        nec_idle(s);
        nec_anchor(s, power_on_ns);
    }
    if (!s->powered) {
        nec_idle(s);
        return;
    }
    if (new_epoch) {
        nec_schedule(s, now_ns, true);
    } else if (!was_available) {
        nec_idle(s);
        nec_schedule(s, now_ns, false);
    }
}

int64_t esp32s3_rmt_nec_deadline(const Esp32S3RmtNec *s)
{
    return s->next_deadline_ns;
}

Esp32S3RmtNecOutput esp32s3_rmt_nec_step(Esp32S3RmtNec *s, uint64_t now_ns)
{
    Esp32S3RmtNecPhase next_phase;
    uint64_t due_ns, duration_ns, next_ns;
    uint8_t next_bit = s->bit_index;
    bool next_level;

    if (!s->configured || !s->power_known || !s->powered ||
        s->next_deadline_ns < 0 || now_ns < (uint64_t)s->next_deadline_ns) {
        return nec_output(s);
    }
    if (now_ns > INT64_MAX) {
        nec_late_abort(s, now_ns);
        return nec_output(s);
    }
    due_ns = s->next_deadline_ns;
    switch (s->phase) {
    case ESP32S3_RMT_NEC_WAIT:
        next_phase = ESP32S3_RMT_NEC_LEAD_MARK;
        duration_ns = NEC_LEAD_MARK_NS;
        next_level = false;
        break;
    case ESP32S3_RMT_NEC_LEAD_MARK:
        next_phase = ESP32S3_RMT_NEC_LEAD_SPACE;
        duration_ns = NEC_LEAD_SPACE_NS;
        next_level = true;
        break;
    case ESP32S3_RMT_NEC_LEAD_SPACE:
        next_phase = ESP32S3_RMT_NEC_BIT_MARK;
        next_bit = 0;
        duration_ns = NEC_BIT_MARK_NS;
        next_level = false;
        break;
    case ESP32S3_RMT_NEC_BIT_MARK:
        next_phase = ESP32S3_RMT_NEC_BIT_SPACE;
        duration_ns = (s->frame_word >> s->bit_index) & 1 ?
                      NEC_ONE_SPACE_NS : NEC_ZERO_SPACE_NS;
        next_level = true;
        break;
    case ESP32S3_RMT_NEC_BIT_SPACE:
        if (s->bit_index == 31) {
            next_phase = ESP32S3_RMT_NEC_FINAL_MARK;
        } else {
            next_phase = ESP32S3_RMT_NEC_BIT_MARK;
            next_bit++;
        }
        duration_ns = NEC_BIT_MARK_NS;
        next_level = false;
        break;
    case ESP32S3_RMT_NEC_FINAL_MARK:
        if (!nec_add_ns(s->frame_start_ns, s->period_ns, &next_ns)) {
            nec_count(&s->frame_count);
            nec_idle(s);
        } else if (next_ns <= now_ns) {
            nec_late_abort(s, now_ns);
        } else {
            nec_count(&s->frame_count);
            nec_idle(s);
            if (next_ns <= (uint64_t)INT64_MAX - NEC_FRAME_NS) {
                s->next_deadline_ns = next_ns;
            }
        }
        return nec_output(s);
    default:
        g_assert_not_reached();
    }

    /* Do not publish a mark whose following edge is already past, or drive
     * two contradictory levels in one timer callback. Aborting returns only
     * idle-high and a strictly-future whole-frame deadline. */
    if (!nec_add_ns(due_ns, duration_ns, &next_ns) || next_ns <= now_ns) {
        nec_late_abort(s, now_ns);
        return nec_output(s);
    }
    if (s->phase == ESP32S3_RMT_NEC_WAIT) {
        s->frame_start_ns = due_ns;
    }
    s->phase = next_phase;
    s->bit_index = next_bit;
    s->level = next_level;
    s->next_deadline_ns = next_ns;
    return nec_output(s);
}
