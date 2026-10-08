/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Explicit audio vectors over actual resolved serial nets. No PCM/PDM
 * converter, generated sample, internal loopback, or assumed power source. */
#include "qemu/osdep.h"
#include "hw/misc/esp32s3_i2s_peer.h"
#include "qemu/timer.h"
#include "qemu/host-utils.h"
#include "sysemu/runstate.h"
#include <math.h>

struct S3I2sPeer {
    S3I2sPeerConfig config;
    S3I2sPeerProvider provider;
    QEMUTimer *timer;
    S3I2sPeerCapture *capture;
    S3I2sPeerTransition *transitions;
    size_t transition_capacity, transition_count, transition_first;
    uint64_t transition_total, transition_lost, transition_epoch, generation;
    uint64_t power_on_ns;
    uint64_t source_id, halfphase, wire_frame;
    int64_t config_activation_ns, activation_ns;
    S3I2sPeerTransition outgoing;
    bool outgoing_active, publication_pending, outputs_active, power_known;
    size_t capture_count, source_cursor;
    uint64_t transmitted, captured, power_epoch;
    uint64_t interval_whole, interval_rem, interval_den, fraction;
    int64_t next_ns;
    unsigned frame_bits, wire_bit, rx_bits;
    uint32_t tx_word, rx_word;
    bool started, subscribed, busy, powered, epoch_valid, paused;
    bool synchronized, initialized, exhausted, overflow;
    bool bclk, ws, last_clock, last_ws, boundary_pending, tx_valid;
    bool staging, dout_oe, dout_level;
    bool raw_phase_started;
    unsigned boundary_bit;
    const char *reason;
};

static bool width_valid(unsigned width)
{
    return width == 8 || width == 16 || width == 24 || width == 32;
}

bool esp32s3_i2s_peer_config_validate(const S3I2sPeerConfig *c, Error **errp)
{
    if (!c || (c->role != S3_I2S_PEER_MASTER &&
               c->role != S3_I2S_PEER_SLAVE) ||
        c->format < S3_I2S_PEER_PHILIPS || c->format > S3_I2S_PEER_RAW_PDM ||
        !width_valid(c->data_bits) || !width_valid(c->slot_bits) ||
        c->data_bits > c->slot_bits || !c->slots || c->slots > 16 ||
        c->slot_bits * c->slots > 128 || !c->slot_mask ||
        (c->slot_mask >> c->slots) || !c->ws_width ||
        c->ws_width > c->slot_bits * c->slots ||
        !c->rate_num_hz || !c->rate_den || !c->capture_capacity ||
        c->capture_capacity > 65536) {
        error_setg(errp, "i2s-sample-peer: invalid explicit geometry, mask, rate or capture capacity");
        return false;
    }
    unsigned frame_bits = c->slots * c->slot_bits;
    uint64_t numerator = UINT64_C(1000000000) * c->rate_den;
    if (c->rate_num_hz > numerator / (2 * frame_bits)) {
        error_setg(errp, "i2s-sample-peer: clock halfperiod is below one virtual nanosecond");
        return false;
    }
    if ((c->format != S3_I2S_PEER_TDM && c->slots != 2) ||
        (c->format == S3_I2S_PEER_PHILIPS &&
         (!c->bit_shift || c->ws_width != c->slot_bits)) ||
        (c->format == S3_I2S_PEER_MSB &&
         (c->bit_shift || c->ws_width != c->slot_bits)) ||
        (c->format == S3_I2S_PEER_PCM &&
         (c->ws_width != 1 || !c->bit_shift))) {
        error_setg(errp, "i2s-sample-peer: format and WS/shift geometry disagree");
        return false;
    }
    if (c->format == S3_I2S_PEER_RAW_PDM) {
        if (c->data_bits != 16 || c->slot_bits != 16 || c->slot_mask != 3 ||
            c->ws_width != 1 || c->bit_shift ||
            c->lsb_first || c->left_align ||
            c->tx_samples || c->tx_sample_count || !c->raw_bits ||
            !c->raw_bit_count || c->raw_bit_count > 65536 ||
            c->raw_bit_count % 32) {
            error_setg(errp, "i2s-sample-peer: raw-PDM requires two 16-bit slots and a complete physical-CLK halfphase vector; PCM alignment is unsupported");
            return false;
        }
        for (size_t i = 0; i < c->raw_bit_count; ++i) {
            if (c->raw_bits[i] > 1) {
                error_setg(errp, "i2s-sample-peer: rawBits contains a nonbinary value");
                return false;
            }
        }
    } else {
        unsigned active = ctpop32(c->slot_mask);
        if (c->raw_bits || c->raw_bit_count || !c->tx_samples ||
            !c->tx_sample_count || c->tx_sample_count > 65536 ||
            c->tx_sample_count % active) {
            error_setg(errp, "i2s-sample-peer: txSamples must contain complete ascending active-slot frames, not raw bits");
            return false;
        }
        for (size_t i = 0; i < c->tx_sample_count; ++i) {
            if (c->data_bits < 32 && c->tx_samples[i] >> c->data_bits) {
                error_setg(errp, "i2s-sample-peer: txSamples exceeds normalized dataBits");
                return false;
            }
        }
    }
    return true;
}

static int64_t now_ns(void)
{
    return qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

static void transition_append(S3I2sPeer *p, S3I2sPeerTransition record,
                              unsigned kind, unsigned source)
{
    record.sequence = p->transition_total++;
    record.kind = kind;
    record.source = source;
    record.ns = now_ns();
    record.epoch = p->transition_epoch;
    record.power_epoch = p->power_epoch;
    record.generation = p->generation;
    record.power_on_ns = p->power_on_ns;
    record.power_known = p->power_known;
    record.powered = p->powered;
    size_t index = (p->transition_first + p->transition_count) %
                   p->transition_capacity;
    if (p->transition_count == p->transition_capacity) {
        p->transition_first = (p->transition_first + 1) %
                              p->transition_capacity;
        ++p->transition_lost;
    } else {
        ++p->transition_count;
    }
    p->transitions[index] = record;
}

static void transition_lifecycle(S3I2sPeer *p, unsigned kind, unsigned source)
{
    transition_append(p, (S3I2sPeerTransition) {
        .source_cursor = p->source_cursor,
        .raw = p->config.format == S3_I2S_PEER_RAW_PDM,
        .slot = UINT32_MAX, .raw_channel = UINT32_MAX,
        .physical_phase = UINT32_MAX, .bit_phase = UINT32_MAX,
        .first_halfphase = UINT64_MAX, .last_halfphase = UINT64_MAX,
        .first_ns = -1, .last_ns = -1,
        .first_boundary_ns = -1, .last_boundary_ns = -1,
    }, kind, source);
}

static void outgoing_begin(S3I2sPeer *p, bool raw, unsigned slot)
{
    p->outgoing = (S3I2sPeerTransition) {
        .source_id = ++p->source_id,
        .source_cursor = p->source_cursor,
        .word_ordinal = raw ? p->halfphase / 16 : p->source_id - 1,
        .frame = raw ? p->halfphase / 32 : p->wire_frame,
        .first_halfphase = raw ? p->halfphase : UINT64_MAX,
        .last_halfphase = raw ? p->halfphase : UINT64_MAX,
        .slot = slot,
        .raw_channel = raw ? p->source_cursor % 2 : UINT32_MAX,
        .physical_phase = raw ? p->source_cursor % 2 : UINT32_MAX,
        .bit_phase = raw ? p->halfphase % 16 : UINT32_MAX,
        .valid_bits = raw ? 1 : p->config.data_bits,
        .slot_bits = raw ? 1 : p->config.slot_bits,
        .raw = raw,
        .first_ns = -1, .last_ns = -1,
        .first_boundary_ns = -1, .last_boundary_ns = -1,
    };
    p->outgoing_active = true;
}

static void outgoing_published(S3I2sPeer *p)
{
    if (!p->publication_pending || !p->outgoing_active) {
        return;
    }
    p->publication_pending = false;
    int64_t ns = now_ns();
    p->outgoing.last_ns = ns;
    if (!p->outgoing.published_bits++) {
        p->outgoing.first_ns = ns;
        transition_append(p, p->outgoing, S3_I2S_PEER_TRANSITION_BEGIN,
                          S3_I2S_PEER_SOURCE_PAYLOAD);
    }
}

static void outgoing_boundary(S3I2sPeer *p)
{
    if (!p->outgoing_active || !p->outgoing.published_bits) {
        return;
    }
    int64_t ns = now_ns();
    if (p->outgoing.first_boundary_ns < 0) {
        p->outgoing.first_boundary_ns = ns;
    }
    p->outgoing.last_boundary_ns = ns;
    if (p->outgoing.raw) {
        p->outgoing.last_halfphase = p->halfphase++;
    }
    if (p->outgoing.published_bits == p->outgoing.slot_bits) {
        transition_append(p, p->outgoing, S3_I2S_PEER_TRANSITION_COMPLETE,
                          S3_I2S_PEER_SOURCE_PAYLOAD);
        p->outgoing_active = false;
        ++p->transmitted;
    }
}

static void release(S3I2sPeer *p)
{
    p->publication_pending = false;
    if (p->outgoing_active) {
        transition_append(p, p->outgoing, S3_I2S_PEER_TRANSITION_ABORT,
                          S3_I2S_PEER_SOURCE_PAYLOAD);
        p->outgoing_active = false;
    }
    p->dout_oe = false;
    bool ok;
    if (p->config.role == S3_I2S_PEER_MASTER) {
        ok = p->provider.drive_frame(p->provider.opaque, false, false,
                                    false, false, false, false);
    } else {
        ok = p->provider.drive(p->provider.opaque, S3_I2S_PEER_DOUT,
                               false, false);
    }
    if (ok && p->outputs_active) {
        transition_lifecycle(p, S3_I2S_PEER_TRANSITION_RELEASE,
                             S3_I2S_PEER_SOURCE_RELEASED);
        p->outputs_active = false;
    }
}

static bool dependency(S3I2sPeer *p, const char *reason, int64_t ns)
{
    timer_del(p->timer);
    bool report = !p->paused || p->reason != reason;
    p->paused = true;
    p->reason = reason;
    release(p);
    if (report) {
        p->provider.dependency(p->provider.opaque, reason, ns);
    }
    vm_stop(RUN_STATE_PAUSED);
    return false;
}

static bool commit_frame(S3I2sPeer *p, int64_t ns)
{
    p->staging = false;
    if (!p->provider.drive_frame(p->provider.opaque, true, p->bclk,
                                true, p->ws, p->dout_oe, p->dout_level)) {
        return dependency(p, "atomic actual clock/data publication unavailable", ns);
    }
    p->outputs_active = true;
    outgoing_published(p);
    return true;
}

static bool drive(S3I2sPeer *p, S3I2sPeerTerminal terminal, bool oe,
                  bool level, int64_t ns)
{
    if (p->config.role == S3_I2S_PEER_MASTER) {
        p->dout_oe = oe;
        p->dout_level = level;
        return p->staging || commit_frame(p, ns);
    }
    if (!p->provider.drive(p->provider.opaque, terminal, oe, level)) {
        return dependency(p, "actual terminal drive/settle provider unavailable", ns);
    }
    p->outputs_active |= oe;
    outgoing_published(p);
    return true;
}

static bool sample(S3I2sPeer *p, S3I2sPeerTerminal terminal,
                   bool *level, int64_t ns)
{
    bool valid;
    if (!p->provider.sample(p->provider.opaque, terminal, ns, &valid, level) ||
        !valid) {
        const char *reason = terminal == S3_I2S_PEER_BCLK ?
            "actual BCLK unresolved, floating or contended" :
            terminal == S3_I2S_PEER_WS ?
            "actual WS/physical CLK unresolved, floating or contended" :
            "actual DIN unresolved, floating or contended before capture";
        return dependency(p, reason, ns);
    }
    return true;
}

static void wire_reset(S3I2sPeer *p)
{
    timer_del(p->timer);
    release(p);
    p->source_cursor = 0;
    p->fraction = 0;
    p->wire_frame = 0;
    p->halfphase = 0;
    p->wire_bit = p->rx_bits = p->tx_word = p->rx_word = 0;
    p->paused = p->synchronized = p->initialized = false;
    p->exhausted = p->boundary_pending = p->tx_valid = false;
    p->bclk = p->ws = p->last_clock = p->last_ws = false;
    p->raw_phase_started = false;
    p->reason = NULL;
}

static bool power_update(S3I2sPeer *p, int64_t ns)
{
    S3I2sPeerPower power;
    p->power_known = false;
    if (!p->provider.power(p->provider.opaque, &power)) {
        p->powered = false;
        return dependency(p, "explicit vdd/gnd rails unknown before serial consumption", ns);
    }
    p->power_known = power.known && isfinite(power.vdd) && isfinite(power.gnd);
    p->generation = power.generation;
    p->power_on_ns = power.power_on_ns;
    double supply = power.vdd - power.gnd;
    bool powered = p->power_known && power.powered &&
                   supply >= 2.7 && supply <= 3.6;
    bool was_powered = p->powered;
    p->powered = powered;
    if (!p->epoch_valid || p->power_epoch != power.power_epoch) {
        wire_reset(p);
        p->epoch_valid = true;
        p->power_epoch = power.power_epoch;
        ++p->transition_epoch;
        transition_lifecycle(p, S3_I2S_PEER_TRANSITION_POWER_EPOCH,
                             S3_I2S_PEER_SOURCE_RELEASED);
    }
    if (!p->power_known) {
        return dependency(p, "explicit vdd/gnd rails unknown before serial consumption", ns);
    }
    if (!powered) {
        if (was_powered) {
            wire_reset(p);
        }
        p->powered = false;
        timer_del(p->timer);
        release(p);
        return false;
    }
    p->powered = true;
    return !p->paused;
}

static bool next_source(S3I2sPeer *p, bool raw, int64_t ns)
{
    size_t count = raw ? p->config.raw_bit_count : p->config.tx_sample_count;
    if (p->source_cursor == count) {
        if (p->config.repeat) {
            p->source_cursor = 0;
        } else {
            p->exhausted = true;
            transition_lifecycle(p, S3_I2S_PEER_TRANSITION_EXHAUSTED,
                                 S3_I2S_PEER_SOURCE_EXHAUSTED);
            return dependency(p, raw ?
                "explicit rawBits playlist exhausted; no physical halfphase source" :
                "explicit txSamples playlist exhausted; no next active-slot sample", ns);
        }
    }
    return true;
}

static bool capture_room(S3I2sPeer *p, int64_t ns)
{
    if (p->capture_count == p->config.capture_capacity) {
        p->overflow = true;
        return dependency(p, "bounded actual capture full; next sample cannot be qualified", ns);
    }
    return true;
}

static void append(S3I2sPeer *p, uint32_t value, unsigned slot, bool raw,
                   int64_t ns)
{
    S3I2sPeerCapture *record = &p->capture[p->capture_count++];
    record->ns = ns;
    record->sequence = p->captured++;
    record->sample = value;
    record->slot = slot;
    record->raw = raw;
}

static bool selected(S3I2sPeer *p, unsigned slot)
{
    return !!(p->config.slot_mask & (1u << slot));
}

static bool transmit(S3I2sPeer *p, int64_t ns)
{
    unsigned slot = p->wire_bit / p->config.slot_bits;
    unsigned bit = p->wire_bit % p->config.slot_bits;
    if (!selected(p, slot)) {
        p->tx_valid = false;
        return drive(p, S3_I2S_PEER_DOUT, false, false, ns);
    }
    if (!bit) {
        if (!next_source(p, false, ns)) {
            return false;
        }
        outgoing_begin(p, false, slot);
        p->tx_word = p->config.tx_samples[p->source_cursor++];
        if (p->config.left_align) {
            p->tx_word <<= p->config.slot_bits - p->config.data_bits;
        }
        p->tx_valid = true;
    }
    if (!p->tx_valid) {
        /* A shifted initial WS boundary precedes the first complete slot.
         * There is no preceding playlist word to fabricate its final bit. */
        return drive(p, S3_I2S_PEER_DOUT, false, false, ns);
    }
    unsigned shift = p->config.lsb_first ? bit : p->config.slot_bits - bit - 1;
    p->publication_pending = true;
    return drive(p, S3_I2S_PEER_DOUT, true, (p->tx_word >> shift) & 1, ns);
}

static bool receive(S3I2sPeer *p, int64_t ns)
{
    unsigned slot = p->wire_bit / p->config.slot_bits;
    unsigned bit = p->wire_bit % p->config.slot_bits;
    if (!selected(p, slot)) {
        p->rx_bits = 0;
        return true;
    }
    if (!bit) {
        if (!capture_room(p, ns)) {
            return false;
        }
        p->rx_word = p->rx_bits = 0;
    } else if (!p->rx_bits) {
        return true; /* Initial shifted partial slot, never a full sample. */
    }
    bool level;
    if (!sample(p, S3_I2S_PEER_DIN, &level, ns)) {
        return false;
    }
    unsigned shift = p->config.lsb_first ? bit : p->config.slot_bits - bit - 1;
    p->rx_word |= (uint32_t)level << shift;
    ++p->rx_bits;
    if (bit + 1 == p->config.slot_bits) {
        uint32_t value = p->rx_word;
        if (p->config.left_align) {
            value >>= p->config.slot_bits - p->config.data_bits;
        }
        if (p->config.data_bits < 32) {
            value &= (1u << p->config.data_bits) - 1;
        }
        append(p, value, slot, false, ns);
        p->rx_bits = 0;
    }
    return true;
}

static bool expected_ws(S3I2sPeer *p)
{
    unsigned wire = (p->wire_bit + p->config.bit_shift) % p->frame_bits;
    bool level;
    if (p->config.format == S3_I2S_PEER_PHILIPS ||
        p->config.format == S3_I2S_PEER_MSB) {
        level = wire >= p->config.slot_bits;
    } else {
        level = wire >= p->config.ws_width;
    }
    return level ^ p->config.ws_pol;
}

static bool raw_preload(S3I2sPeer *p, int64_t ns)
{
    if (!next_source(p, true, ns)) {
        return false;
    }
    if (!p->outgoing_active) {
        outgoing_begin(p, true, (p->source_cursor / 16) % 2);
    }
    p->publication_pending = true;
    return drive(p, S3_I2S_PEER_DOUT, true,
                 p->config.raw_bits[p->source_cursor], ns);
}

static bool raw_receive(S3I2sPeer *p, bool ws, int64_t ns)
{
    bool level;
    if (!capture_room(p, ns) || !sample(p, S3_I2S_PEER_DIN, &level, ns)) {
        return false;
    }
    append(p, level, ws ^ p->config.ws_pol, true, ns);
    return true;
}

static bool raw_slave_edge(S3I2sPeer *p, bool ws, int64_t ns, bool first)
{
    if (!next_source(p, true, ns)) {
        return false;
    }
    if (first) {
        outgoing_boundary(p);
    }
    if (!raw_receive(p, ws, ns)) {
        return false;
    }
    /* DIN is the immutable actual edge frame. DOUT changes for this new
     * halfphase, then holds until the next actual physical CLK edge, giving
     * the S3 master's later internal BCLK sample the correct current bit. */
    if (!first && !raw_preload(p, ns)) {
        return false;
    }
    if (!first) {
        outgoing_boundary(p);
    }
    ++p->source_cursor;
    return true;
}

static bool schedule(S3I2sPeer *p, int64_t ns)
{
    uint64_t step = p->interval_whole;
    p->fraction += p->interval_rem;
    if (p->fraction >= p->interval_den) {
        ++step;
        p->fraction -= p->interval_den;
    }
    if (step > INT64_MAX || p->next_ns > INT64_MAX - (int64_t)step) {
        return dependency(p, "rational clock exceeds virtual-time range", ns);
    }
    p->next_ns += step;
    if (p->next_ns <= ns) {
        return dependency(p, "rational clock deadline is not a finite future event", ns);
    }
    timer_mod(p->timer, p->next_ns);
    return true;
}

static bool master_initialize(S3I2sPeer *p, int64_t ns)
{
    p->synchronized = p->config.format != S3_I2S_PEER_RAW_PDM;
    p->bclk = false;
    p->wire_bit = (p->frame_bits - p->config.bit_shift) % p->frame_bits;
    p->ws = p->config.format == S3_I2S_PEER_RAW_PDM ?
            !p->config.ws_pol : expected_ws(p);
    p->staging = true;
    if (p->config.format == S3_I2S_PEER_RAW_PDM) {
        if (!raw_preload(p, ns)) {
            return false;
        }
    } else if (!transmit(p, ns)) {
        return false;
    }
    if (!commit_frame(p, ns)) {
        return false;
    }
    p->initialized = true;
    p->next_ns = ns;
    return schedule(p, ns);
}

static void master_tick(void *opaque)
{
    S3I2sPeer *p = opaque;
    int64_t ns = now_ns();
    p->busy = true;
    if (!power_update(p, ns)) {
        goto out;
    }
    if (!p->initialized) {
        master_initialize(p, ns);
        goto out;
    }
    p->bclk = !p->bclk;
    bool raw = p->config.format == S3_I2S_PEER_RAW_PDM;
    p->staging = true;
    if (!p->bclk) {
        if (raw) {
            /* Prepare the new phase before atomically publishing its edge.
             * Bit0 was preloaded during initialization. Do not advance DOUT
             * after the edge: clocked MCU receivers may consume it later
             * within this same physical halfphase. */
            if (p->raw_phase_started && !raw_preload(p, ns)) {
                goto out;
            }
            p->ws = !p->ws;
        } else {
            p->ws = expected_ws(p);
            if (!transmit(p, ns)) {
                goto out;
            }
        }
    }
    if (!commit_frame(p, ns)) {
        goto out;
    }
    bool clock, ws;
    if (!sample(p, S3_I2S_PEER_BCLK, &clock, ns) ||
        !sample(p, S3_I2S_PEER_WS, &ws, ns)) {
        goto out;
    }
    if (clock != p->bclk || ws != p->ws) {
        dependency(p, "master clocks do not match actual resolved driven levels", ns);
        goto out;
    }
    if (raw) {
        if (!p->bclk) {
            p->raw_phase_started = p->synchronized = true;
            outgoing_boundary(p);
            ++p->source_cursor;
        } else if (p->raw_phase_started && !raw_receive(p, ws, ns)) {
            /* The initial inactive-baseline BCLK rise consumes no data.
             * Genuine phases capture only after slave module-clock source
             * reactions have had the actual setup halfperiod to settle. */
            goto out;
        }
    } else if (p->bclk) {
        outgoing_boundary(p);
        if (!receive(p, ns)) {
            goto out;
        }
        p->wire_bit = (p->wire_bit + 1) % p->frame_bits;
        p->wire_frame += p->wire_bit == 0;
    }
    schedule(p, ns);
out:
    p->busy = false;
}

static bool slave_initialize(S3I2sPeer *p, int64_t ns)
{
    bool valid, ws, clock = false;
    if (!p->provider.sample(p->provider.opaque, S3_I2S_PEER_WS, ns,
                            &valid, &ws)) {
        return dependency(p, "actual WS provider unavailable during startup", ns);
    }
    if (!valid) {
        return true; /* No clock edge has yet been observed or consumed. */
    }
    if (p->config.format != S3_I2S_PEER_RAW_PDM) {
        if (!p->provider.sample(p->provider.opaque, S3_I2S_PEER_BCLK, ns,
                                &valid, &clock)) {
            return dependency(p, "actual BCLK provider unavailable during startup", ns);
        }
        if (!valid) {
            return true;
        }
    } else {
        if (!raw_preload(p, ns)) {
            return false;
        }
    }
    p->last_ws = ws;
    p->last_clock = clock;
    p->initialized = true;
    return true;
}

static void slave_notify(S3I2sPeer *p, int64_t ns)
{
    if (!p->initialized) {
        slave_initialize(p, ns);
        return;
    }
    bool ws, clock;
    if (!sample(p, S3_I2S_PEER_WS, &ws, ns)) {
        return;
    }
    if (p->config.format == S3_I2S_PEER_RAW_PDM) {
        if (ws != p->last_ws) {
            p->last_ws = ws;
            bool first = !p->synchronized;
            if (!p->synchronized && ws == p->config.ws_pol) {
                p->synchronized = true;
            }
            if (p->synchronized) {
                raw_slave_edge(p, ws, ns, first);
            }
        }
        return;
    }
    if (!sample(p, S3_I2S_PEER_BCLK, &clock, ns)) {
        return;
    }
    bool falling = p->last_clock && !clock;
    bool rising = !p->last_clock && clock;
    bool standard = p->config.format == S3_I2S_PEER_PHILIPS ||
                    p->config.format == S3_I2S_PEER_MSB;
    if (ws != p->last_ws) {
        bool logical = ws ^ p->config.ws_pol;
        bool boundary = standard || !logical;
        if (boundary && (p->synchronized || !logical)) {
            unsigned position = standard && logical ? p->config.slot_bits : 0;
            p->boundary_bit = (position + p->frame_bits - p->config.bit_shift) %
                              p->frame_bits;
            p->boundary_pending = true;
        }
    }
    p->last_ws = ws;
    p->last_clock = clock;
    if (falling) {
        if (p->boundary_pending) {
            if (p->synchronized && p->wire_bit != p->boundary_bit) {
                dependency(p, "actual WS boundary disagrees with configured serial frame", ns);
                return;
            }
            p->wire_bit = p->boundary_bit;
            p->boundary_pending = false;
            p->synchronized = true;
        }
        if (p->synchronized) {
            transmit(p, ns);
        }
    }
    if (rising && p->synchronized && !p->paused) {
        if (p->boundary_pending) {
            dependency(p, "WS boundary has no actual falling setup edge before consumption", ns);
            return;
        }
        if (ws != expected_ws(p)) {
            dependency(p, "actual WS level disagrees with explicit polarity/width/shift", ns);
            return;
        }
        outgoing_boundary(p);
        if (receive(p, ns)) {
            p->wire_bit = (p->wire_bit + 1) % p->frame_bits;
            p->wire_frame += p->wire_bit == 0;
        }
    }
}

void esp32s3_i2s_peer_notify(void *opaque, int64_t ns)
{
    S3I2sPeer *p = opaque;
    if (!p || !p->started || p->busy) {
        return;
    }
    p->busy = true;
    if (power_update(p, ns)) {
        if (p->config.role == S3_I2S_PEER_MASTER) {
            if (!p->initialized) {
                master_initialize(p, ns);
            }
        } else {
            slave_notify(p, ns);
        }
    }
    p->busy = false;
}

S3I2sPeer *esp32s3_i2s_peer_new(S3I2sPeerConfig *owned,
                               const S3I2sPeerProvider *provider, Error **errp)
{
    if (!provider || !provider->power || !provider->sample || !provider->drive ||
        !provider->subscribe || !provider->unsubscribe || !provider->dependency) {
        error_setg(errp, "i2s-sample-peer: complete native electrical provider required");
        return NULL;
    }
    if (!esp32s3_i2s_peer_config_validate(owned, errp)) {
        return NULL;
    }
    if (owned->role == S3_I2S_PEER_MASTER && !provider->drive_frame) {
        error_setg(errp, "i2s-sample-peer: master requires atomic native clock/data drive_frame");
        return NULL;
    }
    S3I2sPeer *p = g_try_new0(S3I2sPeer, 1);
    if (!p) {
        error_setg(errp, "i2s-sample-peer: instance allocation failed");
        return NULL;
    }
    p->capture = g_try_new0(S3I2sPeerCapture, owned->capture_capacity);
    if (!p->capture) {
        g_free(p);
        error_setg(errp, "i2s-sample-peer: bounded capture allocation failed");
        return NULL;
    }
    p->transition_capacity = 2 * owned->capture_capacity + 16;
    p->transitions = g_try_new0(S3I2sPeerTransition, p->transition_capacity);
    if (!p->transitions) {
        g_free(p->capture);
        g_free(p);
        error_setg(errp, "i2s-sample-peer: bounded transition allocation failed");
        return NULL;
    }
    p->config_activation_ns = now_ns();
    p->activation_ns = -1;
    p->provider = *provider;
    p->config = *owned;
    memset(owned, 0, sizeof(*owned));
    p->frame_bits = p->config.slot_bits * p->config.slots;
    p->interval_den = p->config.rate_num_hz * (2 * p->frame_bits);
    uint64_t numerator = UINT64_C(1000000000) * p->config.rate_den;
    p->interval_whole = numerator / p->interval_den;
    p->interval_rem = numerator % p->interval_den;
    p->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, master_tick, p);
    return p;
}

bool esp32s3_i2s_peer_start(S3I2sPeer *p, Error **errp)
{
    if (!p || p->started) {
        error_setg(errp, "i2s-sample-peer: invalid or already started instance");
        return false;
    }
    if (!p->provider.subscribe(p->provider.opaque, esp32s3_i2s_peer_notify, p)) {
        error_setg(errp, "i2s-sample-peer: actual solved-frame/power subscription unavailable");
        return false;
    }
    p->subscribed = p->started = true;
    p->activation_ns = now_ns();
    /* Observe the real committed endpoint before exposing ACTIVATE. This
     * also establishes the actual epoch for known-off or unknown rails;
     * no hardware source may start while these origins are still defaults. */
    p->busy = true;
    bool ready = power_update(p, p->activation_ns);
    if (p->epoch_valid) {
        transition_lifecycle(p, S3_I2S_PEER_TRANSITION_ACTIVATE,
                             S3_I2S_PEER_SOURCE_RELEASED);
    }
    p->busy = false;
    if (ready) {
        esp32s3_i2s_peer_notify(p, now_ns());
    }
    /* Subscription/setup succeeded. Unknown rails, unresolved active nets
     * and other runtime dependencies already released outputs, logged their
     * reason and paused the VM; expose those through status, not Error.
     * Otherwise the service would misclassify a genuine electrical boundary
     * as a failed factory/activation and prevent its capture export. */
    return true;
}

void esp32s3_i2s_peer_status(const S3I2sPeer *p, S3I2sPeerStatus *status)
{
    *status = (S3I2sPeerStatus) {
        .powered = p->powered,
        .paused = p->paused,
        .synchronized = p->synchronized,
        .exhausted = p->exhausted,
        .capture_overflow = p->overflow,
        .power_epoch = p->power_epoch,
        .transmitted = p->transmitted,
        .captured = p->captured,
        .capture_count = p->capture_count,
        .dependency = p->reason,
        .config_identity = p->config.identity,
    };
}

const S3I2sPeerCapture *esp32s3_i2s_peer_capture(const S3I2sPeer *p, size_t *count)
{
    *count = p->capture_count;
    return p->capture;
}

void esp32s3_i2s_peer_transition_window(const S3I2sPeer *p,
                                      S3I2sPeerTransitionWindow *window)
{
    *window = (S3I2sPeerTransitionWindow) {
        .version = 1, .generation = p->generation,
        .epoch = p->transition_epoch,
        .config_activation_ns = p->config_activation_ns,
        .activation_ns = p->activation_ns,
        .power_on_ns = p->power_on_ns,
        .capacity = p->transition_capacity, .count = p->transition_count,
        .first_index = p->transition_first,
        .total = p->transition_total, .lost = p->transition_lost,
        .first_sequence = p->transition_total - p->transition_count,
        .records = p->transitions,
    };
}

void esp32s3_i2s_peer_free(S3I2sPeer *p)
{
    if (!p) {
        return;
    }
    p->started = false;
    if (p->subscribed) {
        p->provider.unsubscribe(p->provider.opaque, esp32s3_i2s_peer_notify, p);
    }
    timer_del(p->timer);
    release(p);
    timer_free(p->timer);
    esp32s3_i2s_peer_config_clear(&p->config);
    g_free(p->capture);
    g_free(p->transitions);
    g_free(p);
}
