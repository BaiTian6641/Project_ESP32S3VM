/* SPDX-License-Identifier: GPL-2.0-or-later */
/* ESP32-S3 only: IDF 6.1 mcpwm_{reg,struct}.h and S3 mcpwm_ll.h.
 * Event priorities/topology: ESP32-S3 TRM v1.8, chapter 36,
 * https://documentation.espressif.com/esp32-s3_technical_reference_manual_en.pdf
 * Three independent timers, three operators and six routed outputs per group.
 * No host pin backend, parallel GPIO output store, or per-edge allocation. */
#include "qemu/osdep.h"
#include "hw/misc/esp32s3_mcpwm.h"
#include "hw/misc/esp32s3_electrical.h"
#include "hw/xtensa/esp32s3_reset_domain.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-clock.h"
#include "hw/irq.h"
#include "qapi/error.h"
#include "qemu/host-utils.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "sysemu/runstate.h"

#define NS UINT64_C(1000000000)
#define R(s, a) ((s)->reg[(a) / 4])
#define TREG(t, a) (4 + 16 * (t) + (a))
#define OREG(o, a) (0x3c + 0x38 * (o) + (a))
#define SYNCI 0x34
#define TIMERSEL 0x38
#define FAULT 0xe4
#define CAPCFG 0xe8
#define CAPPHASE 0xec
#define CAPCH(c) (0xf0 + 4 * (c))
#define CAPVALUE(c) (0xfc + 4 * (c))
#define CAPSTATUS 0x108
#define UPDATE 0x10c
#define ENA 0x110
#define RAW 0x114
#define ST 0x118
#define CLR 0x11c
/* Events in generator register order; SYNC is a shadow-transfer event. */
#define TEZ BIT(0)
#define TEP BIT(1)
#define TEA BIT(2)
#define TEB BIT(3)
#define TR0 BIT(4)
#define TR1 BIT(5)
#define SYNC BIT(6)
static void schedule(ESP32S3McpwmState *s);
static void publish(ESP32S3McpwmState *s);
static void inputs(ESP32S3McpwmState *s);
static void advance(ESP32S3McpwmState *s, int64_t now);

static bool live(ESP32S3McpwmState *s)
{
    return s->enabled && !s->reset_held && !s->dependency;
}
static void irq_update(ESP32S3McpwmState *s)
{
    R(s, ST) = R(s, RAW) & R(s, ENA);
    qemu_set_irq(s->irq, R(s, ST) != 0);
}
static void dependency(ESP32S3McpwmState *s, const char *reason)
{
    if (!s->dependency) {
        qemu_log_mask(LOG_GUEST_ERROR, "MCPWM%u dependency: %s\n", s->group, reason);
    }
    s->dependency = true;
    timer_del(s->event);
    vm_stop(RUN_STATE_PAUSED);
}
static unsigned selected_timer(ESP32S3McpwmState *s, unsigned o)
{
    return (R(s, TIMERSEL) >> (2 * o)) & 3;
}
static uint64_t timer_div(ESP32S3McpwmState *s, unsigned t)
{
    return ((R(s, 0) & 255) + 1) * (uint64_t)(s->timer[t].prescale + 1);
}
static uint64_t delay_div(ESP32S3McpwmState *s, unsigned o)
{
    unsigned t = selected_timer(s, o);
    return (R(s, OREG(o, 0x1c)) & BIT(17)) && t < 3 ?
           timer_div(s, t) : (R(s, 0) & 255) + 1;
}
static uint64_t carrier_unit(ESP32S3McpwmState *s, unsigned o)
{
    return NS * ((R(s, 0) & 255) + 1) *
           (((R(s, OREG(o, 0x28)) >> 1) & 15) + 1);
}
static void action(bool *level, unsigned a)
{
    if (a == 1) {
        *level = false;
    } else if (a == 2) {
        *level = true;
    } else if (a == 3) {
        *level = !*level;
    }
}
static bool update_due(unsigned method, unsigned event, bool force)
{
    return force || (!method || (!(method & 8) &&
           (((method & 1) && (event & TEZ)) ||
            ((method & 2) && (event & TEP)) ||
            ((method & 4) && (event & SYNC)))));
}
static void shadow_operator(ESP32S3McpwmState *s, unsigned o,
                            unsigned event, bool force)
{
    S3McpwmOperator *p = &s->oper[o];
    if (!force && (!(R(s, UPDATE) & 1) ||
                  !(R(s, UPDATE) & BIT(2 + 2 * o)))) {
        return;
    }
    for (unsigned g = 0; g < 2; g++) {
        if (update_due((R(s, OREG(o, 0)) >> (4 * g)) & 15, event, force)) {
            p->compare[g] = R(s, OREG(o, 4 + 4 * g));
            R(s, OREG(o, 0)) &= ~BIT(8 + g);
        }
        if (update_due(R(s, OREG(o, 0xc)) & 15, event, force)) {
            p->actions[g] = R(s, OREG(o, 0x14 + 4 * g));
        }
        if (update_due((R(s, OREG(o, 0x1c)) >> (4 * g)) & 15, event, force)) {
            p->delay[g] = R(s, OREG(o, 0x20 + 4 * g));
        }
    }
    unsigned method = R(s, OREG(o, 0x10)) & 63;
    unsigned force_events = (event & 15) | ((event & SYNC) ? BIT(4) : 0);
    if (force || !method || (!(method & 32) && (method & force_events))) {
        uint32_t next = R(s, OREG(o, 0x10)) & 0x3c0;
        for (unsigned g = 0; g < 2; g++) {
            if ((next ^ p->force) & (3U << (6 + 2 * g))) {
                p->carry[g] = s->event_carry;
            }
        }
        p->force = next;
    }
}
static void shadow_timer(ESP32S3McpwmState *s, unsigned t,
                         unsigned event, bool force)
{
    unsigned method = (R(s, TREG(t, 0)) >> 24) & 3;
    if (force || ((R(s, UPDATE) & 1) &&
        (!method || ((method & 1) && (event & TEZ)) ||
         ((method & 2) && (event & SYNC))))) {
        s->timer[t].period = R(s, TREG(t, 0)) >> 8;
    }
}
static void generator_event(ESP32S3McpwmState *s, unsigned o,
                            unsigned event, bool down)
{
    S3McpwmOperator *p = &s->oper[o];
    /* TRM tables 36.3-3/4 arbitrate independently for each output.
     * A no-action field contributes no set/clear/toggle request. */
    static const unsigned up_order[6] = {1, 4, 5, 3, 2, 0};
    static const unsigned down_order[6] = {0, 4, 5, 3, 2, 1};
    const unsigned *order = down ? down_order : up_order;
    for (unsigned g = 0; g < 2; g++) {
        for (unsigned k = 0; k < 6; k++) {
            unsigned a = (p->actions[g] >>
                          (2 * order[k] + (down ? 12 : 0))) & 3;
            if ((event & BIT(order[k])) && a) {
                bool before = p->generator[g];
                action(&p->generator[g], a);
                if (before != p->generator[g]) {
                    p->carry[g] = s->event_carry;
                }
                break;
            }
        }
    }
}
static bool fault_brake(ESP32S3McpwmState *s, unsigned o, bool ost)
{
    unsigned f = (R(s, FAULT) >> 6) & 7;
    unsigned cfg = R(s, OREG(o, 0x2c)) >> (ost ? 4 : 0);
    return ((f & 1) && (cfg & 8)) || ((f & 2) && (cfg & 4)) ||
           ((f & 4) && (cfg & 2));
}
static void brake(ESP32S3McpwmState *s, unsigned o, bool ost)
{
    S3McpwmOperator *p = &s->oper[o];
    bool *active = ost ? &p->ost : &p->cbc;
    if (*active) {
        return;
    }
    unsigned t = selected_timer(s, o);
    bool down = t < 3 && s->timer[t].down;
    *active = true;
    R(s, RAW) |= BIT(o + (ost ? 24 : 21));
    for (unsigned g = 0; g < 2; g++) {
        /* Fault handler follows deadtime/carrier. Seed toggle/no-action
         * from the real final output, not the generator before deadtime. */
        bool *level = ost ? &p->ost_level[g] : &p->cbc_level[g];
        *level = p->brake_level[g];
        action(level, (R(s, OREG(o, 0x2c)) >>
               (8 + 8 * g + (ost ? 4 : 0) + (down ? 0 : 2))) & 3);
    }
}
static unsigned trigger_events(ESP32S3McpwmState *s, unsigned o, unsigned source)
{
    unsigned event = 0, cfg = R(s, OREG(o, 0xc));
    for (unsigned k = 0; k < 2; k++) {
        if (((cfg >> (4 + 3 * k)) & 7) == source) {
            event |= BIT(4 + k);
        }
    }
    return event;
}
/* Fixed three-node visited mask bounds arbitrary guest sync feedback loops. */
static void sync_source(ESP32S3McpwmState *s, unsigned source, unsigned visited)
{
    if ((R(s, CAPCFG) & 2) && ((R(s, CAPCFG) >> 2) & 7) == source) {
        s->capture_count = R(s, CAPPHASE);
        s->capture_phase = 0;
    }
    for (unsigned t = 0; t < 3; t++) {
        if (!(visited & BIT(t)) && ((R(s, SYNCI) >> (3 * t)) & 7) == source) {
            uint32_t cfg = R(s, TREG(t, 8));
            if (cfg & 1) {
                unsigned mode = (R(s, TREG(t, 4)) >> 3) & 3;
                S3McpwmTimer *p = &s->timer[t];
                p->count = cfg >> 4;
                p->down = mode == 2 || (mode == 3 && (cfg & BIT(20)));
                p->phase = s->event_carry;
                s->sync_mask |= BIT(t);
                shadow_timer(s, t, SYNC, false);
                for (unsigned o = 0; o < 3; o++) {
                    if (selected_timer(s, o) == t) {
                        shadow_operator(s, o, SYNC, false);
                        generator_event(s, o, trigger_events(s, o, 3), p->down);
                    }
                }
            }
            if (((cfg >> 2) & 3) == 0) {
                sync_source(s, t + 1, visited | BIT(t));
            }
        }
    }
}
static void sample_faults(ESP32S3McpwmState *s)
{
    for (unsigned f = 0; f < 3; f++) {
        if ((s->fault_pending & BIT(f)) && (R(s, FAULT) & BIT(f)) &&
            s->input_valid[3 + f]) {
            bool active = s->input[3 + f] == !!(R(s, FAULT) & BIT(f + 3));
            bool old = R(s, FAULT) & BIT(f + 6);
            R(s, FAULT) = (R(s, FAULT) & ~BIT(f + 6)) |
                          (active ? BIT(f + 6) : 0);
            if (active != old) {
                R(s, RAW) |= BIT(f + (active ? 9 : 12));
            }
            if (active && !old) {
                for (unsigned o = 0; o < 3; o++) {
                    s->fault_events[o] |= trigger_events(s, o, f);
                    if (R(s, OREG(o, 0x2c)) & BIT(7 - f)) {
                        brake(s, o, true);
                    }
                }
            }
        }
    }
    s->fault_pending = 0;
}
static void timer_event(ESP32S3McpwmState *s, unsigned t, bool allow_stop)
{
    S3McpwmTimer *p = &s->timer[t];
    unsigned event = 0;
    if (p->count == 0) {
        event |= TEZ;
        R(s, RAW) |= BIT(t + 3);
    }
    if (p->count == p->period) {
        event |= TEP;
        R(s, RAW) |= BIT(t + 6);
    }
    shadow_timer(s, t, event, false);
    for (unsigned o = 0; o < 3; o++) {
        if (selected_timer(s, o) == t) {
            S3McpwmOperator *op = &s->oper[o];
            shadow_operator(s, o, event, false);
            unsigned oe = event | s->fault_events[o];
            s->fault_events[o] = 0;
            for (unsigned c = 0; c < 2; c++) {
                if (p->count == op->compare[c]) {
                    oe |= BIT(c + 2);
                    R(s, RAW) |= BIT(o + 3 * c + 15);
                }
            }
            shadow_operator(s, o, oe, false);
            generator_event(s, o, oe, p->down);
            if (((R(s, OREG(o, 0x30)) >> 1) & event & 3) &&
                !fault_brake(s, o, false)) {
                op->cbc = false;
            }
        }
    }
    unsigned synco = (R(s, TREG(t, 8)) >> 2) & 3;
    if ((synco == 1 && (event & TEZ)) || (synco == 2 && (event & TEP))) {
        sync_source(s, t + 1, BIT(t));
    }
    unsigned command = R(s, TREG(t, 4)) & 7;
    if (allow_stop && (((command == 0 || command == 3) && (event & TEZ)) ||
        ((command == 1 || command == 4) && (event & TEP)))) {
        p->running = false;
        p->phase = 0;
        /* Start-and-stop commands self-clear to their stop condition. */
        R(s, TREG(t, 4)) = (R(s, TREG(t, 4)) & ~7U) |
                          ((command == 1 || command == 4) ? 1 : 0);
        R(s, RAW) |= BIT(t);
    }
}
static unsigned next_distance(ESP32S3McpwmState *s, unsigned t)
{
    S3McpwmTimer *p = &s->timer[t];
    unsigned d;
    if (p->down) {
        d = p->count > p->period ? p->count - p->period : (p->count ? p->count : 1);
    } else {
        d = p->count < p->period ? p->period - p->count :
            p->count == p->period ? 1 : 0x10000 - p->count;
    }
    for (unsigned o = 0; o < 3; o++) {
        if (selected_timer(s, o) == t) {
            for (unsigned c = 0; c < 2; c++) {
                unsigned v = s->oper[o].compare[c];
                if (v <= p->period && ((p->down && v < p->count) ||
                    (!p->down && v > p->count))) {
                    d = MIN(d, p->down ? p->count - v : v - p->count);
                }
            }
        }
    }
    return MAX(d, 1);
}
static bool timer_live(ESP32S3McpwmState *s, unsigned t)
{
    return s->timer[t].running && (R(s, TREG(t, 4)) & 0x18);
}
static void delay_input(S3McpwmDelay *p, bool input, bool rising,
                        uint64_t duration, uint64_t carry)
{
    if (input != p->input) {
        p->input = input;
        p->carry = carry;
        if (input == rising) {
            p->remaining = duration - MIN(carry, duration - 1);
            p->pending = true;
        } else {
            /* Opposite edge cancels a pulse shorter than its dead time. */
            p->pending = false;
            p->output = input;
        }
    }
}
static unsigned process_outputs(ESP32S3McpwmState *s)
{
    unsigned changed = 0;
    for (unsigned o = 0; o < 3; o++) {
        S3McpwmOperator *p = &s->oper[o];
        uint32_t dt = R(s, OREG(o, 0x1c));
        bool gen[2];
        for (unsigned g = 0; g < 2; g++) {
            unsigned force = (p->force >> (6 + 2 * g)) & 3;
            gen[g] = force == 1 ? false : force == 2 ? true : p->generator[g];
        }
        delay_input(&p->edge[1], gen[(dt >> 11) & 1], true,
                    NS * delay_div(s, o) * (p->delay[1] + 1),
                    p->carry[(dt >> 11) & 1]);
        bool fed_input = (dt & BIT(8)) ? p->edge[1].output : gen[(dt >> 12) & 1];
        delay_input(&p->edge[0], fed_input, false,
                    NS * delay_div(s, o) * (p->delay[0] + 1),
                    (dt & BIT(8)) ? p->edge[1].carry : p->carry[(dt >> 12) & 1]);
        bool path[2] = {
            (dt & BIT(15)) ? gen[0] : p->edge[1].output ^ !!(dt & BIT(13)),
            (dt & BIT(16)) ? gen[1] : p->edge[0].output ^ !!(dt & BIT(14))
        };
        uint64_t path_carry[2] = {
            (dt & BIT(15)) ? p->carry[0] : p->edge[1].carry,
            (dt & BIT(16)) ? p->carry[1] : p->edge[0].carry
        };
        uint32_t cc = R(s, OREG(o, 0x28));
        for (unsigned g = 0; g < 2; g++) {
            bool value = path[g ^ !!(dt & BIT(9 + g))];
            S3McpwmCarrier *c = &p->carrier[g];
            if (cc & 1) {
                value ^= !!(cc & BIT(13));
                if (value != c->input) {
                    c->input = value;
                    c->output = value;
                    c->remaining = value ? carrier_unit(s, o) * 8 *
                                           (((cc >> 8) & 15) + 1) : 0;
                    if (value) {
                        c->remaining -= MIN(path_carry[g ^ !!(dt & BIT(9 + g))],
                                            c->remaining - 1);
                    }
                }
                value = c->input && c->output;
                value ^= !!(cc & BIT(12));
            } else {
                c->input = c->output = false;
                c->remaining = 0;
            }
            value = p->ost ? p->ost_level[g] : p->cbc ? p->cbc_level[g] : value;
            changed |= s->publish_all || value != p->brake_level[g] ?
                       BIT(2 * o + g) : 0;
            p->brake_level[g] = value;
        }
        /* OST status describes a still-active qualified fault, not the
         * retained output override. Keep that override until FH_CLR_OST:
         * the documented public recovery first tests OST_ON after exit. */
        R(s, OREG(o, 0x34)) = p->cbc |
                             ((p->ost && fault_brake(s, o, true)) << 1);
    }
    return changed;
}
static void publish(ESP32S3McpwmState *s)
{
    if (!s->realized || s->updating || !s->electrical) {
        return;
    }
    s->updating = true;
    unsigned changed = process_outputs(s);
    if (changed) {
        esp32s3_electrical_begin_update(s->electrical);
        for (unsigned n = 0; n < 6; n++) {
            if ((changed & BIT(n)) &&
                !esp32s3_electrical_set_matrix_drive(s->electrical,
                    160 + 6 * s->group + n, true,
                    s->oper[n / 2].brake_level[n % 2], false)) {
                dependency(s, "electrical output authority rejected signal");
            }
        }
        esp32s3_electrical_end_update(s->electrical);
    }
    s->publish_all = false;
    s->updating = false;
    /* Reentrant frames are consumed in the next finite timer delta, not by
     * recursively publishing inside the electrical observer stack. */
    irq_update(s);
}
static void capture(ESP32S3McpwmState *s, unsigned c, bool falling)
{
    if (!(R(s, CAPCH(c)) & 1)) {
        return;
    }
    R(s, CAPVALUE(c)) = s->capture_count;
    R(s, CAPSTATUS) = (R(s, CAPSTATUS) & ~BIT(c)) | (falling ? BIT(c) : 0);
    R(s, RAW) |= BIT(27 + c);
}
static bool sample(ESP32S3McpwmState *s, unsigned signal, bool *value)
{
    unsigned cfg = s->gpio->func_in_sel_cfg[signal], pad = cfg & 63;
    if (!(cfg & BIT(7))) {
        dependency(s, "consumed input bypasses modelled matrix");
        return false;
    }
    if (pad < ESP32S3_GPIO_COUNT) {
        bool valid, level;
        double voltage;
        if (!esp32s3_electrical_line(s->electrical, pad, s->last_ns,
                                   &voltage, &valid, &level) || !valid) {
            esp32s3_gpio_consume_unknown(s->gpio, pad);
            dependency(s, "consumed pad is floating, contended or unknown");
            return false;
        }
    }
    if (!esp32s3_gpio_matrix_sample(s->gpio, signal, value)) {
        dependency(s, "consumed matrix input has no enabled physical route");
        return false;
    }
    return true;
}
static void inputs(ESP32S3McpwmState *s)
{
    if (!s->enabled || s->reset_held || !s->electrical || !s->gpio) {
        return;
    }
    /* Only enabled consumers demand a physical input; disabled registers
     * do not spuriously consume the GPIO matrix reset bypass selections. */
    for (unsigned i = 0; i < 9; i++) {
        unsigned kind = i / 3, ch = i % 3;
        bool consume = false;
        if (kind == 0) {
            for (unsigned t = 0; t < 3; t++) {
                uint32_t sync = R(s, TREG(t, 8));
                consume |= s->pll_hz && ((sync & 1) || ((sync >> 2) & 3) == 0) &&
                           ((R(s, SYNCI) >> (3 * t)) & 7) == ch + 4;
            }
            consume |= s->apb_hz && (R(s, CAPCFG) & 2) &&
                       ((R(s, CAPCFG) >> 2) & 7) == ch + 4;
        } else if (kind == 1) {
            consume = s->pll_hz && (R(s, FAULT) & BIT(ch));
        } else {
            consume = s->apb_hz && (R(s, CAPCH(ch)) & 1) && (R(s, CAPCFG) & 1);
        }
        if (!consume) {
            s->input_valid[i] = false;
            continue;
        }
        bool value;
        if (!sample(s, (s->group ? 169 : 160) + i, &value)) {
            s->input_valid[i] = false;
            continue;
        }
        if (kind == 0) {
            value ^= !!(R(s, SYNCI) & BIT(ch + 9));
        } else if (kind == 2) {
            value ^= !!(R(s, CAPCH(ch)) & BIT(11));
        }
        bool changed = s->input_valid[i] && value != s->input[i];
        bool first = !s->input_valid[i];
        s->input_valid[i] = true;
        s->input[i] = value;
        if (kind == 0 && changed && value) {
            sync_source(s, ch + 4, 0);
        } else if (kind == 1) {
            if (first || changed) {
                s->fault_pending |= BIT(ch);
                s->fault_remaining = NS * ((R(s, 0) & 255) + 1) - s->group_phase;
            }
        } else if (kind == 2 && changed) {
            unsigned div = ((R(s, CAPCH(ch)) >> 3) & 255) + 1;
            bool output = value;
            if (div > 1) {
                if (value && ++s->capture_prescale[ch] == div) {
                    s->capture_prescale[ch] = 0;
                    s->capture_divided[ch] = true;
                } else if (value) {
                    s->capture_divided[ch] = false;
                }
                output = value && s->capture_divided[ch];
            }
            if ((value && output && (R(s, CAPCH(ch)) & BIT(2))) ||
                (!value && s->capture_divided[ch] && (R(s, CAPCH(ch)) & BIT(1))) ||
                (div == 1 && !value && (R(s, CAPCH(ch)) & BIT(1)))) {
                capture(s, ch, !value);
            }
        }
    }
    for (unsigned o = 0; o < 3; o++) {
        if (fault_brake(s, o, false)) {
            brake(s, o, false);
        }
    }
    irq_update(s);
}
static void notify(void *opaque, uint64_t timestamp)
{
    ESP32S3McpwmState *s = opaque;
    if (s->updating) {
        s->input_pending = true;
        s->event_carry = 0;
        inputs(s);
        return;
    }
    advance(s, timestamp);
    /* A valid new frame can resolve a paused dependency. */
    s->dependency = false;
    s->input_pending = false;
    s->event_carry = 0;
    inputs(s);
    publish(s);
    schedule(s);
}
static void advance(ESP32S3McpwmState *s, int64_t now)
{
    uint64_t elapsed = now > s->last_ns ? now - s->last_ns : 0;
    s->last_ns = now;
    if (!elapsed || !live(s)) {
        return;
    }
    if (R(s, CAPCFG) & 1) {
        uint64_t whole = elapsed / NS * s->apb_hz;
        uint64_t part = (elapsed % NS) * s->apb_hz + s->capture_phase;
        s->capture_count += whole + part / NS;
        s->capture_phase = part % NS;
    }
    unsigned group_div = (R(s, 0) & 255) + 1;
    uint64_t source_whole = (elapsed / NS * s->pll_hz) % group_div;
    s->group_phase = (source_whole * NS + (elapsed % NS) * s->pll_hz +
                      s->group_phase) % (NS * group_div);
    /* The one persistent timer is always scheduled at the earliest event.
     * Thus this interval cannot jump over a timer compare/endpoint. */
    uint64_t amount = elapsed * s->pll_hz;
    if (s->fault_pending) {
        if (amount >= s->fault_remaining) {
            s->event_carry = amount - s->fault_remaining;
            s->fault_remaining = 0;
            sample_faults(s);
        } else {
            s->fault_remaining -= amount;
        }
    }
    for (unsigned o = 0; o < 3; o++) {
        for (unsigned k = 0; k < 2; k++) {
            S3McpwmDelay *d = &s->oper[o].edge[k];
            if (d->pending) {
                if (amount >= d->remaining) {
                    d->carry = amount - d->remaining;
                    d->remaining = 0;
                    d->pending = false;
                    d->output = d->input;
                } else {
                    d->remaining -= amount;
                }
            }
            S3McpwmCarrier *c = &s->oper[o].carrier[k];
            if (c->remaining) {
                if (amount >= c->remaining) {
                    uint64_t carry = amount - c->remaining;
                    unsigned duty = (R(s, OREG(o, 0x28)) >> 5) & 7;
                    c->output = !c->output;
                    c->remaining = carrier_unit(s, o) *
                                   (c->output ? duty : 8 - duty);
                    if (!duty) {
                        c->output = false;
                        c->remaining = 0;
                    } else {
                        c->remaining -= MIN(carry, c->remaining - 1);
                    }
                } else {
                    c->remaining -= amount;
                }
            }
        }
    }
    uint32_t timer_events = 0;
    s->sync_mask = 0;
    for (unsigned t = 0; t < 3; t++) {
        S3McpwmTimer *p = &s->timer[t];
        if (timer_live(s, t)) {
            uint64_t unit = NS * timer_div(s, t);
            uint64_t total = p->phase + amount;
            unsigned ticks = total / unit;
            p->phase = total % unit;
            if (ticks) {
                unsigned mode = (R(s, TREG(t, 4)) >> 3) & 3;
                if (mode == 3) {
                    /* S3 endpoints are UTEZ/DTEP (not UTEP/DTEZ). */
                    if (p->down && p->count == 0) {
                        p->down = false;
                    } else if (!p->down && p->count == p->period) {
                        p->down = true;
                    }
                }
                if (p->down) {
                    p->count = p->count ? p->count - ticks : p->period;
                } else {
                    p->count = p->count == p->period ? 0 : p->count + ticks;
                }
                if (mode == 3) {
                    if (p->count == 0) {
                        p->down = false;
                    } else if (p->count == p->period) {
                        p->down = true;
                    }
                }
                timer_events |= BIT(t);
            }
        }
    }
    /* Advance all counters before dispatch, so timer-to-timer sync never
     * charges the elapsed interval a second time to a reloaded timer. */
    for (unsigned t = 0; t < 3; t++) {
        if ((timer_events & BIT(t)) && !(s->sync_mask & BIT(t))) {
            s->event_carry = s->timer[t].phase;
            timer_event(s, t, true);
        }
    }
    /* Group-clock fault triggers also reach operators with frozen timers. */
    for (unsigned o = 0; o < 3; o++) {
        if (s->fault_events[o]) {
            unsigned t = selected_timer(s, o);
            generator_event(s, o, s->fault_events[o], t < 3 && s->timer[t].down);
            s->fault_events[o] = 0;
        }
    }
}
static void schedule(ESP32S3McpwmState *s)
{
    timer_del(s->event);
    if (!live(s) || !s->pll_hz) {
        return;
    }
    uint64_t remaining = UINT64_MAX;
    if (s->fault_pending) {
        remaining = s->fault_remaining;
    }
    if (s->input_pending) {
        timer_mod(s->event, s->last_ns + 1);
        return;
    }
    for (unsigned t = 0; t < 3; t++) {
        if (timer_live(s, t)) {
            remaining = MIN(remaining, NS * timer_div(s, t) *
                            next_distance(s, t) - s->timer[t].phase);
        }
    }
    for (unsigned o = 0; o < 3; o++) {
        for (unsigned g = 0; g < 2; g++) {
            if (s->oper[o].edge[g].pending) {
                remaining = MIN(remaining, s->oper[o].edge[g].remaining);
            }
            if (s->oper[o].carrier[g].remaining) {
                remaining = MIN(remaining, s->oper[o].carrier[g].remaining);
            }
        }
    }
    if (remaining != UINT64_MAX) {
        uint64_t ns = remaining / s->pll_hz + !!(remaining % s->pll_hz);
        timer_mod(s->event, s->last_ns + MAX(ns, 1));
    }
}
static void event(void *opaque)
{
    ESP32S3McpwmState *s = opaque;
    advance(s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    s->input_pending = false;
    s->event_carry = 0;
    inputs(s);
    publish(s);
    schedule(s);
}
static uint32_t mask(unsigned a)
{
    static const uint32_t tm[4] = {0x3ffffff, 0x1f, 0x1fffff, 0};
    static const uint32_t op[14] = {
        0x3ff, 0xffff, 0xffff, 0x3ff, 0xffff, 0xffffff, 0xffffff,
        0x3ffff, 0xffff, 0xffff, 0x3fff, 0xffffff, 0x1f, 0
    };
    if (!a) {
        return 0xff;
    }
    if (a < 0x34) {
        return tm[((a - 4) % 16) / 4];
    }
    if (a == SYNCI) {
        return 0xfff;
    }
    if (a == TIMERSEL) {
        return 0x3f;
    }
    if (a < FAULT) {
        return op[((a - 0x3c) % 0x38) / 4];
    }
    switch (a) {
    case FAULT: return 0x3f;
    case CAPCFG: return 0x3f;
    case CAPPHASE: return UINT32_MAX;
    case 0xf0: case 0xf4: case 0xf8: return 0x1fff;
    case UPDATE: return 0xff;
    case ENA: case RAW: case CLR: return 0x3fffffff;
    case 0x120: return 1;
    case 0x124: return 0xfffffff;
    default: return 0;
    }
}
static uint64_t read_reg(void *opaque, hwaddr addr, unsigned size)
{
    ESP32S3McpwmState *s = opaque;
    advance(s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    if (addr >= 0x128 || (addr & 3) || size != 4 || s->reset_held) {
        return 0;
    }
    if (addr >= 0x10 && addr <= 0x30 && ((addr - 0x10) % 16 == 0)) {
        unsigned t = (addr - 0x10) / 16;
        S3McpwmTimer *p = &s->timer[t];
        unsigned value = p->count;
        /* LL count getter explicitly compensates the next-count status. */
        if (R(s, TREG(t, 4)) & 0x18) {
            value = p->down ? (value ? value - 1 : p->period) :
                    (value == p->period ? 0 : (value + 1) & 0xffff);
        }
        return value | (p->down << 16);
    }
    irq_update(s);
    schedule(s);
    return addr == CLR ? 0 : R(s, addr);
}
static void write_reg(void *opaque, hwaddr addr, uint64_t data, unsigned size)
{
    ESP32S3McpwmState *s = opaque;
    advance(s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    s->event_carry = 0;
    if (addr >= 0x128 || (addr & 3) || size != 4 || s->reset_held) {
        return;
    }
    uint32_t old = R(s, addr), value = data & mask(addr);
    if (!mask(addr)) {
        return;
    }
    uint32_t old_delay_div[3];
    for (unsigned o = 0; o < 3; o++) {
        old_delay_div[o] = delay_div(s, o);
    }
    if (addr == CLR || addr == RAW) {
        R(s, RAW) &= ~value;
    } else if (addr == FAULT) {
        R(s, addr) = value | (old & 0x1c0);
        for (unsigned f = 0; f < 3; f++) {
            if ((old ^ value) & (BIT(f) | BIT(f + 3))) {
                s->input_valid[3 + f] = false;
            }
            if (!(value & BIT(f))) {
                if (old & BIT(f + 6)) {
                    R(s, RAW) |= BIT(f + 12);
                }
                R(s, addr) &= ~BIT(f + 6);
                s->fault_pending &= ~BIT(f);
            }
        }
    } else {
        R(s, addr) = value;
    }
    if (addr == 0) {
        unsigned olddiv = (old & 255) + 1, newdiv = value + 1;
        s->group_phase = muldiv64(s->group_phase, newdiv, olddiv);
        s->fault_remaining = muldiv64(s->fault_remaining, newdiv, olddiv);
        for (unsigned t = 0; t < 3; t++) {
            s->timer[t].phase = muldiv64(s->timer[t].phase, newdiv, olddiv);
        }
        for (unsigned o = 0; o < 3; o++) {
            for (unsigned g = 0; g < 2; g++) {
                s->oper[o].carrier[g].remaining = muldiv64(
                    s->oper[o].carrier[g].remaining, newdiv, olddiv);
            }
        }
    } else if (addr >= 4 && addr < SYNCI) {
        unsigned t = (addr - 4) / 16, off = (addr - 4) % 16;
        S3McpwmTimer *p = &s->timer[t];
        if (off == 0) {
            shadow_timer(s, t, 0, false);
        } else if (off == 4) {
            unsigned command = value & 7, mode = (value >> 3) & 3;
            if (command >= 2 && command <= 4 && !p->running) {
                uint64_t olddiv = timer_div(s, t);
                p->prescale = R(s, TREG(t, 0)) & 255;
                p->phase = muldiv64(p->phase, timer_div(s, t), olddiv);
                p->running = true;
                p->down = mode == 2;
                if (mode == 2 && !p->count) {
                    p->count = p->period;
                }
                if (mode) {
                    timer_event(s, t, false);
                }
            } else if (mode == 1) {
                p->down = false;
            } else if (mode == 2) {
                p->down = true;
            }
        } else if (off == 8 && ((old ^ value) & BIT(1))) {
            if (value & 1) {
                p->count = value >> 4;
                unsigned mode = (R(s, TREG(t, 4)) >> 3) & 3;
                p->down = mode == 2 || (mode == 3 && (value & BIT(20)));
                p->phase = 0;
                shadow_timer(s, t, SYNC, false);
                for (unsigned o = 0; o < 3; o++) {
                    if (selected_timer(s, o) == t) {
                        shadow_operator(s, o, SYNC, false);
                        generator_event(s, o, trigger_events(s, o, 3), p->down);
                    }
                }
            }
            sync_source(s, t + 1, BIT(t));
        }
    } else if (addr >= 0x3c && addr < FAULT) {
        unsigned o = (addr - 0x3c) / 0x38, off = (addr - 0x3c) % 0x38;
        S3McpwmOperator *p = &s->oper[o];
        if (off == 0) {
            R(s, addr) = (value & 255) | (old & 0x300 & ~(data & 0x300));
        } else if (off == 4 || off == 8) {
            R(s, OREG(o, 0)) |= BIT(8 + (off / 4 - 1));
        } else if (off == 0x10) {
            for (unsigned g = 0; g < 2; g++) {
                if ((old ^ value) & BIT(g ? 13 : 10)) {
                    unsigned a = (value >> (g ? 14 : 11)) & 3;
                    if (a == 1 || a == 2) {
                        p->generator[g] = a == 2;
                        p->carry[g] = 0;
                    }
                }
            }
        } else if (off == 0x2c) {
            /* Enabling an OST handler on an already-active physical fault
             * is a new activation; a clear on unchanged level is not. */
            for (unsigned f = 0; f < 3; f++) {
                if ((value & ~old & BIT(7 - f)) &&
                    (R(s, FAULT) & BIT(f + 6))) {
                    brake(s, o, true);
                }
            }
        } else if (off == 0x30) {
            if ((value & 1) && !(old & 1)) {
                p->ost = false;
            }
            if (((old ^ value) & BIT(3)) && (R(s, OREG(o, 0x2c)) & 1)) {
                brake(s, o, false);
            }
            if (((old ^ value) & BIT(4)) && (R(s, OREG(o, 0x2c)) & BIT(4))) {
                brake(s, o, true);
            }
        } else if (off == 0x28) {
            /* Preserve fractional progress when the carrier divisor changes. */
            unsigned od = ((old >> 1) & 15) + 1, nd = ((value >> 1) & 15) + 1;
            for (unsigned g = 0; g < 2; g++) {
                p->carrier[g].remaining = muldiv64(p->carrier[g].remaining, nd, od);
            }
        }
        shadow_operator(s, o, 0, false);
    } else if (addr == UPDATE) {
        bool global = (old ^ value) & BIT(1);
        for (unsigned t = 0; t < 3; t++) {
            shadow_timer(s, t, 0, global);
        }
        for (unsigned o = 0; o < 3; o++) {
            shadow_operator(s, o, 0, global || ((old ^ value) & BIT(3 + 2 * o)));
        }
    } else if (addr == CAPCFG) {
        if ((value & BIT(5)) && (value & BIT(1))) {
            s->capture_count = R(s, CAPPHASE);
            s->capture_phase = 0;
        }
        R(s, addr) &= ~BIT(5);
    } else if (addr >= 0xf0 && addr <= 0xf8) {
        unsigned c = (addr - 0xf0) / 4;
        if ((old ^ value) & 0xff9) {
            s->capture_prescale[c] = 0;
            s->capture_divided[c] = false;
            s->input_valid[6 + c] = false;
        }
        if (value & BIT(12)) {
            capture(s, c, false);
        }
        R(s, addr) &= ~BIT(12);
    }
    for (unsigned o = 0; o < 3; o++) {
        unsigned div = delay_div(s, o);
        if (div != old_delay_div[o]) {
            for (unsigned g = 0; g < 2; g++) {
                s->oper[o].edge[g].remaining = muldiv64(
                    s->oper[o].edge[g].remaining, div, old_delay_div[o]);
            }
        }
    }
    inputs(s);
    publish(s);
    schedule(s);
}
static const MemoryRegionOps ops = {
    .read = read_reg, .write = write_reg, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {.min_access_size = 4, .max_access_size = 4, .unaligned = false},
    .impl = {.min_access_size = 4, .max_access_size = 4},
};
static void reset_device(DeviceState *dev)
{
    ESP32S3McpwmState *s = ESP32S3_MCPWM(dev);
    timer_del(s->event);
    memset(s->reg, 0, sizeof(s->reg));
    memset(s->timer, 0, sizeof(s->timer));
    memset(s->oper, 0, sizeof(s->oper));
    memset(s->input_valid, 0, sizeof(s->input_valid));
    memset(s->capture_prescale, 0, sizeof(s->capture_prescale));
    memset(s->capture_divided, 0, sizeof(s->capture_divided));
    s->capture_count = s->capture_phase = 0;
    s->group_phase = s->fault_remaining = s->fault_pending = 0;
    memset(s->fault_events, 0, sizeof(s->fault_events));
    s->sync_mask = 0;
    s->dependency = false;
    s->input_pending = false;
    s->last_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    for (unsigned t = 0; t < 3; t++) {
        R(s, TREG(t, 0)) = 255 << 8;
        s->timer[t].period = 255;
        R(s, OREG(t, 0x10)) = 32;
        R(s, OREG(t, 0x1c)) = BIT(15) | BIT(16);
    }
    R(s, UPDATE) = 0x55;
    R(s, 0x124) = 34632240;
    s->publish_all = true;
    publish(s);
    irq_update(s);
}
static void reset(DeviceState *dev)
{
    ESP32S3McpwmState *s = ESP32S3_MCPWM(dev);
    if (!s->soc_reset || esp32s3_reset_covers_periph(s->soc_reset)) {
        reset_device(dev);
    }
}
static void gate(void *opaque, int n, int level)
{
    ESP32S3McpwmState *s = opaque;
    advance(s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    s->enabled = level;
    if (!level) {
        /* External pads may change while the consumer clock is stopped.
         * Resume establishes input baselines, never replays those changes
         * as a capture/sync edge. Enabled faults still sample their level. */
        memset(s->input_valid, 0, sizeof(s->input_valid));
    } else {
        inputs(s);
    }
    publish(s);
    schedule(s);
}
static void held(void *opaque, int n, int level)
{
    ESP32S3McpwmState *s = opaque;
    advance(s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    s->reset_held = level;
    if (level) {
        reset_device(DEVICE(s));
    }
    publish(s);
    schedule(s);
}
static void reset_input(void *opaque, int n, int level)
{
    if (level) {
        reset_device(DEVICE(opaque));
    }
}
static void mcpwm_clock_update(void *opaque, ClockEvent ev)
{
    ESP32S3McpwmState *s = opaque;
    if (ev == ClockPreUpdate) {
        advance(s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    } else {
        s->pll_hz = clock_get_hz(s->pll_clk);
        s->apb_hz = clock_get_hz(s->apb_clk);
        publish(s);
        schedule(s);
    }
}
static void realize(DeviceState *dev, Error **errp)
{
    ESP32S3McpwmState *s = ESP32S3_MCPWM(dev);
    if (s->group > 1 || !s->gpio || !s->electrical) {
        error_setg(errp, "MCPWM requires group 0/1 and gpio/electrical links");
        return;
    }
    if (!esp32s3_electrical_subscribe(s->electrical, notify, s)) {
        error_setg(errp, "MCPWM electrical subscriber capacity exhausted");
        return;
    }
    s->subscribed = true;
    s->pll_hz = clock_get_hz(s->pll_clk);
    s->apb_hz = clock_get_hz(s->apb_clk);
    s->realized = true;
    reset_device(dev);
}
static void unrealize(DeviceState *dev)
{
    ESP32S3McpwmState *s = ESP32S3_MCPWM(dev);
    if (s->subscribed) {
        esp32s3_electrical_unsubscribe(s->electrical, notify, s);
        s->subscribed = false;
    }
    s->enabled = s->realized = false;
    timer_del(s->event);
    esp32s3_electrical_begin_update(s->electrical);
    for (unsigned n = 0; n < 6; n++) {
        esp32s3_electrical_set_matrix_drive(s->electrical,
            160 + 6 * s->group + n, false, false, false);
    }
    esp32s3_electrical_end_update(s->electrical);
}
static void init(Object *obj)
{
    ESP32S3McpwmState *s = ESP32S3_MCPWM(obj);
    memory_region_init_io(&s->iomem, obj, &ops, s, TYPE_ESP32S3_MCPWM, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    s->event = timer_new_ns(QEMU_CLOCK_VIRTUAL, event, s);
    s->apb_clk = qdev_init_clock_in(DEVICE(obj), "apb-clk", mcpwm_clock_update, s,
                                   ClockPreUpdate | ClockUpdate);
    s->pll_clk = qdev_init_clock_in(DEVICE(obj), "pll-f160m-clk", mcpwm_clock_update, s,
                                   ClockPreUpdate | ClockUpdate);
    qdev_init_gpio_in_named(DEVICE(obj), gate, "clock-enable", 1);
    qdev_init_gpio_in_named(DEVICE(obj), held, "reset-held", 1);
    qdev_init_gpio_in_named(DEVICE(obj), reset_input, "reset", 1);
}
static void finalize(Object *obj)
{
    timer_free(ESP32S3_MCPWM(obj)->event);
}
static Property properties[] = {
    DEFINE_PROP_UINT32("group", ESP32S3McpwmState, group, 0),
    DEFINE_PROP_LINK("gpio", ESP32S3McpwmState, gpio, TYPE_ESP32S3_GPIO,
                     ESP32S3GPIOState *),
    DEFINE_PROP_LINK("electrical", ESP32S3McpwmState, electrical,
                     TYPE_ESP32S3_ELECTRICAL, DeviceState *),
    DEFINE_PROP_LINK("soc-reset", ESP32S3McpwmState, soc_reset,
                     TYPE_DEVICE, DeviceState *),
    DEFINE_PROP_END_OF_LIST(),
};
static void class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = realize;
    dc->unrealize = unrealize;
    device_class_set_legacy_reset(dc, reset);
    device_class_set_props(dc, properties);
}
static const TypeInfo info = {
    .name = TYPE_ESP32S3_MCPWM, .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ESP32S3McpwmState), .instance_init = init,
    .instance_finalize = finalize, .class_init = class_init,
};
static void register_types(void)
{
    type_register_static(&info);
}
type_init(register_types)
