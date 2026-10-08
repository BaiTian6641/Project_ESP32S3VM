/* SPDX-License-Identifier: GPL-2.0-or-later */
/* ESP32-S3 I2S v2: physical serial shifters and bounded FIFO/GDMA demand.
 * Register layout: locked ESP-IDF 6.1 esp32s3 i2s_reg.h; electrical behavior:
 * Espressif ESP32-S3 TRM v1.8 chapter 28. No ESP32 ADC/DAC/LCD inheritance. */
#include "qemu/osdep.h"
#include "hw/misc/esp32s3_i2s.h"
#include "hw/misc/esp32s3_electrical.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-clock.h"
#include "hw/irq.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/bswap.h"
#include "sysemu/runstate.h"

#define RAW 0x0c
#define ST 0x10
#define ENA 0x14
#define CLR 0x18
#define RX_CONF 0x20
#define TX_CONF 0x24
#define RX_CONF1 0x28
#define TX_CONF1 0x2c
#define RX_CLK 0x30
#define TX_CLK 0x34
#define RX_DIV 0x38
#define TX_DIV 0x3c
#define PDM_CONF 0x40
#define PDM_CONF1 0x44
#define RX_TDM 0x50
#define TX_TDM 0x54
#define RX_TIMING 0x58
#define TX_TIMING 0x5c
#define HUNG 0x60
#define EOF_NUM 0x64
#define SINGLE 0x68
#define STATE 0x6c
#define DATE 0x80
#define R(s,a) ((s)->reg[(a) / 4])
#define START BIT(2)
#define SLAVE BIT(3)
#define UPDATE BIT(8)
#define PDM BIT(20)
#define TDM BIT(19)

static const unsigned tx_bclk[2] = {22, 28};
static const unsigned tx_ws[2] = {24, 29};
static const unsigned rx_bclk[2] = {26, 31};
static const unsigned rx_ws[2] = {27, 32};
static const unsigned data_signal[2] = {25, 30};
static void stream_edge(void *opaque);
static void reconcile(ESP32S3I2sState *s);
static bool shared_tx(ESP32S3I2sState *s);

static uint32_t tx_transition_flags(const S3I2sStream *p)
{
    return ((p->conf & PDM) ? S3_I2S_TX_FLAG_RAW : 0) |
           ((p->conf & SLAVE) ? S3_I2S_TX_FLAG_SLAVE : 0) |
           ((p->conf & BIT(27)) ? S3_I2S_TX_FLAG_SHARED : 0);
}
static S3I2sTxTransition tx_transition_event(const S3I2sStream *p,
                                           S3I2sTxTransitionKind kind)
{
    S3I2sTxTransition event = {
        .epoch = p->owner->tx_epoch, .frame = p->frames,
        .ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
        .first_ns = -1, .last_ns = -1,
        .first_boundary_ns = -1, .last_boundary_ns = -1,
        .first_halfphase = S3_I2S_TX_TRANSITION_NO_PHASE,
        .last_halfphase = S3_I2S_TX_TRANSITION_NO_PHASE,
        .kind = kind, .flags = tx_transition_flags(p),
        .source = S3_I2S_TX_TRANSITION_NO_INDEX,
        .slot = S3_I2S_TX_TRANSITION_NO_INDEX,
        .raw_channel = S3_I2S_TX_TRANSITION_NO_INDEX,
        .physical_phase = S3_I2S_TX_TRANSITION_NO_INDEX,
        .valid_bits = p->data_bits, .slot_bits = p->slot_bits,
    };
    return event;
}
static void tx_transition_append(ESP32S3I2sState *s,
                                 S3I2sTxTransition *event)
{
    if (!s->tx_transitions) { return; }
    event->sequence = s->tx_transition_total++;
    s->tx_transitions[event->sequence % S3_I2S_TX_TRANSITION_CAPACITY] = *event;
}
bool esp32s3_i2s_tx_transition_window(const ESP32S3I2sState *s,
                                    S3I2sTxTransitionWindow *window)
{
    memset(window, 0, sizeof(*window));
    window->version = S3_I2S_TX_TRANSITION_VERSION;
    window->controller = s->controller;
    window->activation_ns = s->tx_activation_ns;
    window->epoch = s->tx_epoch;
    window->active = s->tx_transition_active;
    if (!s->tx_transitions) { return false; }
    window->records = s->tx_transitions;
    window->capacity = S3_I2S_TX_TRANSITION_CAPACITY;
    window->total = s->tx_transition_total;
    window->count = MIN(window->total, window->capacity);
    window->lost = window->total - window->count;
    window->first_sequence = window->lost;
    window->first_index = window->first_sequence % window->capacity;
    return true;
}
static void tx_transition_abort(S3I2sStream *p,
                                S3I2sTxTransitionReason reason)
{
    if (p->rx || !p->owner->tx_transitions) { return; }
    for (unsigned i = 0; i < 2; ++i) {
        S3I2sTxWordPhase *phase = &p->tx_phase[i];
        if (phase->pending) {
            phase->event.kind = S3_I2S_TX_ABORT;
            phase->event.reason = reason;
            phase->event.ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            tx_transition_append(p->owner, &phase->event);
        }
        phase->pending = phase->published = false;
    }
    p->prepared = false;
}
static void tx_transition_suspend(S3I2sStream *p,
                                  S3I2sTxTransitionReason reason)
{
    ESP32S3I2sState *s = p->owner;
    if (p->rx || !s->tx_transitions) { return; }
    tx_transition_abort(p, reason);
    if (s->tx_transition_active) {
        S3I2sTxTransition event = tx_transition_event(p, S3_I2S_TX_SUSPEND);
        event.reason = reason;
        tx_transition_append(s, &event);
        s->tx_transition_active = false;
    }
}
static void tx_transition_start(S3I2sStream *p,
                                S3I2sTxTransitionReason reason)
{
    ESP32S3I2sState *s = p->owner;
    if (p->rx || !s->tx_transitions) { return; }
    tx_transition_abort(p, reason);
    ++s->tx_epoch;
    s->tx_transition_active = false;
    s->tx_transition_started = true;
    s->tx_activation_ns = -1;
    S3I2sTxTransition event = tx_transition_event(p, S3_I2S_TX_START);
    event.reason = reason;
    if (reason == S3_I2S_TX_REASON_REGISTER_START) {
        event.flags |= S3_I2S_TX_FLAG_CLEAR_POSITION;
    }
    tx_transition_append(s, &event);
}
static void tx_transition_reset(S3I2sStream *p,
                                S3I2sTxTransitionReason reason)
{
    ESP32S3I2sState *s = p->owner;
    if (p->rx || !s->tx_transitions) { return; }
    tx_transition_abort(p, reason);
    ++s->tx_epoch;
    s->tx_transition_active = s->tx_transition_started = false;
    s->tx_activation_ns = -1;
    S3I2sTxTransition event = tx_transition_event(p, S3_I2S_TX_RESET);
    event.reason = reason;
    event.flags |= S3_I2S_TX_FLAG_CLEAR_FIFO | S3_I2S_TX_FLAG_CLEAR_SHIFTER |
                   S3_I2S_TX_FLAG_CLEAR_FRAME_CACHE | S3_I2S_TX_FLAG_CLEAR_POSITION;
    tx_transition_append(s, &event);
}
static void tx_transition_stop(S3I2sStream *p,
                               S3I2sTxTransitionReason reason)
{
    ESP32S3I2sState *s = p->owner;
    if (p->rx || !s->tx_transitions) { return; }
    tx_transition_abort(p, reason);
    if (s->tx_transition_started) {
        S3I2sTxTransition event = tx_transition_event(p, S3_I2S_TX_STOP);
        event.reason = reason;
        tx_transition_append(s, &event);
    }
    s->tx_transition_active = s->tx_transition_started = false;
}
static void tx_transition_begin(S3I2sStream *p, unsigned channel,
                                unsigned slot, uint64_t id,
                                S3I2sTxSource source)
{
    if (!p->owner->tx_transitions) { return; }
    S3I2sTxWordPhase *phase = &p->tx_phase[channel];
    if (phase->pending) {
        phase->event.kind = S3_I2S_TX_ABORT;
        phase->event.reason = S3_I2S_TX_REASON_RESYNC;
        phase->event.ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        tx_transition_append(p->owner, &phase->event);
    }
    memset(phase, 0, sizeof(*phase));
    phase->event = tx_transition_event(p, S3_I2S_TX_COMPLETE);
    phase->event.source_word_id = id;
    phase->event.word_ordinal = ++p->owner->tx_word_ordinal;
    phase->event.source = source;
    phase->event.slot = slot;
    if (p->conf & PDM) {
        phase->event.raw_channel = channel;
        phase->event.valid_bits = phase->event.slot_bits = 16;
    }
    phase->pending = true;
}
/* Preparation only stages attribution. The helper may already have consumed
 * a raw bit, but no completion exists until data AND a real clock publication
 * succeed. Ordinary bits additionally require the real rising boundary. */
static void tx_transition_prepare(S3I2sStream *p, unsigned mask,
                                  unsigned bit, unsigned physical_phase,
                                  bool advances)
{
    if (!p->owner->tx_transitions) { return; }
    p->prepared = true;
    p->prepared_mask = mask;
    p->prepared_bit = bit;
    p->prepared_phase = physical_phase;
    p->prepared_advances = advances;
    p->prepared_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}
static void tx_transition_publish(S3I2sStream *p)
{
    ESP32S3I2sState *s = p->owner;
    if (p->rx || !s->tx_transitions) { return; }
    if (!s->tx_transition_active) {
        S3I2sTxTransition event = tx_transition_event(p, S3_I2S_TX_ACTIVATE);
        s->tx_transition_active = true;
        s->tx_activation_ns = event.ns;
        tx_transition_append(s, &event);
    }
    if (!p->prepared) { return; }
    bool raw = p->conf & PDM;
    uint64_t halfphase = raw ? s->tx_halfphase++ : S3_I2S_TX_TRANSITION_NO_PHASE;
    for (unsigned i = 0; i < 2; ++i) {
        S3I2sTxWordPhase *phase = &p->tx_phase[i];
        if (!(p->prepared_mask & (1u << i)) || !phase->pending) { continue; }
        if (p->prepared_bit != phase->event.shifted_bits) {
            tx_transition_abort(p, S3_I2S_TX_REASON_RESYNC);
            return;
        }
        if (phase->event.first_ns < 0) {
            phase->event.first_ns = p->prepared_ns;
            phase->event.first_halfphase = halfphase;
        }
        phase->event.last_ns = p->prepared_ns;
        phase->event.last_halfphase = halfphase;
        phase->event.physical_phase = p->prepared_phase;
        phase->published = true;
        if (p->prepared_advances) { ++phase->published_bits; }
    }
}
static void tx_transition_boundary(S3I2sStream *p, bool sampling_boundary)
{
    if (p->rx || !p->owner->tx_transitions || !p->prepared) { return; }
    int64_t ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    for (unsigned i = 0; i < 2; ++i) {
        S3I2sTxWordPhase *phase = &p->tx_phase[i];
        if (!(p->prepared_mask & (1u << i)) || !phase->pending ||
            !phase->published) { continue; }
        if (sampling_boundary) {
            if (phase->event.first_boundary_ns < 0) {
                phase->event.first_boundary_ns = ns;
            }
            phase->event.last_boundary_ns = ns;
        }
        if (p->prepared_advances) {
            ++phase->event.shifted_bits;
            if (phase->event.shifted_bits == phase->event.slot_bits &&
                phase->published_bits == phase->event.slot_bits) {
                phase->event.ns = ns;
                tx_transition_append(p->owner, &phase->event);
                phase->pending = false;
            }
        }
        phase->published = false;
    }
    p->prepared = false;
}

static void irq_update(ESP32S3I2sState *s)
{
    qemu_set_irq(s->irq, !!(s->raw & s->ena));
}
static void dependency(S3I2sStream *p, const char *reason)
{
    if (!p->paused) {
        qemu_log_mask(LOG_GUEST_ERROR, "I2S%u %s dependency: %s at %" PRId64 " ns\n",
                      p->owner->controller, p->rx ? "RX" : "TX", reason,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    }
    tx_transition_suspend(p, S3_I2S_TX_REASON_DEPENDENCY);
    if (p->rx && shared_tx(p->owner)) {
        tx_transition_suspend(&p->owner->tx, S3_I2S_TX_REASON_DEPENDENCY);
    }
    p->paused = true;
    timer_del(p->timer);
    vm_stop(RUN_STATE_PAUSED);
}
static bool drive(ESP32S3I2sState *s, unsigned signal, bool oe, bool value)
{
    if (!s->electrical ||
        !esp32s3_electrical_set_matrix_drive(s->electrical, signal, oe, value, false)) {
        dependency(&s->tx, "native electrical matrix drive unavailable");
        return false;
    }
    return true;
}
static bool sample(S3I2sStream *p, unsigned signal, bool *level)
{
    ESP32S3I2sState *s = p->owner;
    if (!s->gpio || !s->electrical || signal >= ESP32S3_GPIO_FUNC_IN_SEL_COUNT) {
        dependency(p, "matrix input provider unavailable");
        return false;
    }
    unsigned pad = s->gpio->func_in_sel_cfg[signal] & 0x3f;
    if (pad < ESP32S3_GPIO_COUNT) {
        double volts;
        bool valid, resolved;
        if (!esp32s3_electrical_line(s->electrical, pad,
                qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), &volts, &valid, &resolved) || !valid) {
            dependency(p, "consumed input floating, contended or unknown");
            return false;
        }
    }
    /* GPIO owns mux, inversion and input-enable truth; the electrical owner
     * owns net resolution. A strict helper never fabricates a bypass bit. */
    if (!esp32s3_gpio_matrix_sample(s->gpio, signal, level)) {
        dependency(p, "consumed matrix input unresolved or direct route absent");
        return false;
    }
    return true;
}
static bool powered(ESP32S3I2sState *s)
{
    return s->gate && !s->reset_asserted && clock_get_hz(s->bus) &&
           (R(s, TX_CLK) & BIT(29));
}
static bool clock_available(S3I2sStream *p)
{
    uint32_t cfg = R(p->owner, p->rx ? RX_CLK : TX_CLK);
    return (cfg & BIT(26)) &&
           (p->interval_den || (p->external_source && p->external_ready));
}
static bool stream_running(S3I2sStream *p)
{
    return powered(p->owner) && p->enabled && !p->paused && clock_available(p);
}
static bool shared_rx(ESP32S3I2sState *s)
{
    return (s->tx.conf & BIT(27)) && (s->rx.conf & SLAVE) && !(s->tx.conf & SLAVE);
}
static bool shared_tx(ESP32S3I2sState *s)
{
    return (s->tx.conf & BIT(27)) && (s->tx.conf & SLAVE) && !(s->rx.conf & SLAVE);
}
static void fifo_reset(S3I2sStream *p)
{
    p->head = p->count = 0;
}
static void stream_reset(S3I2sStream *p)
{
    timer_del(p->timer);
    fifo_reset(p);
    p->wire_bit = p->rx_bits = p->rx_bytes = 0;
    p->sample = p->rx_word = p->previous_sample = 0;
    memset(p->last_frame, 0, sizeof(p->last_frame));
    p->source_word_id = p->previous_word_id = 0;
    p->sample_source = S3_I2S_TX_IDLE_ZERO;
    memset(p->last_frame_word_id, 0, sizeof(p->last_frame_word_id));
    p->frame_valid = 0;
    esp32s3_i2s_pdm_raw_reset(&p->pdm, p->owner->controller);
    p->sample_valid = p->previous_valid = false;
    p->bclk = p->ws = p->data = false;
    p->synchronized = p->paused = p->enabled = false;
    p->initialized = p->ws_boundary_pending = false;
    p->last_input_clock = p->last_input_ws = false;
    p->external_phase = 0;
    p->external_source = p->last_mclk = p->external_ready = false;
    p->fraction = p->frames = p->samples = p->starvation = 0;
}
static bool dma_channel(S3I2sStream *p, uint32_t *channel)
{
    ESP32S3I2sState *s = p->owner;
    return s->gdma && esp_gdma_get_channel_periph(s->gdma,
        s->controller ? GDMA_I2S1 : GDMA_I2S0,
        p->rx ? ESP_GDMA_IN_IDX : ESP_GDMA_OUT_IDX, channel);
}
static void fifo_pump(S3I2sStream *p)
{
    uint32_t channel;
    if (!dma_channel(p, &channel)) { return; }
    ESP32S3I2sState *s = p->owner;
    /* Bound work by one hardware FIFO, including across tiny EOF-marked
     * descriptors. Served-count APIs retain real partial transfers on faults. */
    unsigned budget = S3_I2S_FIFO_BYTES;
    while (budget) {
        uint32_t served = 0;
        if (p->rx) {
            if (!p->count) { break; }
            unsigned take = MIN(budget, MIN(p->count, S3_I2S_FIFO_BYTES - p->head));
            unsigned eof = R(s, EOF_NUM) & 0xfff;
            if (eof) {
                if (p->rx_bytes >= eof) {
                    if (!esp_gdma_finish_rx_segment(s->gdma, channel)) {
                        dependency(p, "GDMA rejected updated RX EOF counter boundary");
                        return;
                    }
                    p->rx_bytes = 0;
                }
                take = MIN(take, eof - p->rx_bytes);
            }
            esp_gdma_write_channel_ex(s->gdma, channel, &p->fifo[p->head], take, &served);
            p->head = (p->head + served) % S3_I2S_FIFO_BYTES;
            p->count -= served;
            p->rx_bytes += served;
            if (eof && p->rx_bytes == eof) {
                if (!esp_gdma_finish_rx_segment(s->gdma, channel)) {
                    dependency(p, "GDMA rejected actual RX EOF segment boundary");
                    return;
                }
                p->rx_bytes = 0;
                if (((p->conf >> 13) & 3) == 1) {
                    p->enabled = false;
                    R(s, RX_CONF) &= ~START;
                    s->raw |= BIT(0);
                    irq_update(s);
                    timer_del(p->timer);
                    return;
                }
            }
        } else {
            if (p->count == S3_I2S_FIFO_BYTES) { break; }
            unsigned tail = (p->head + p->count) % S3_I2S_FIFO_BYTES;
            unsigned take = MIN(budget, MIN(S3_I2S_FIFO_BYTES - p->count,
                                            S3_I2S_FIFO_BYTES - tail));
            EspGdmaTxInfo info;
            esp_gdma_read_channel_ex(s->gdma, channel, &p->fifo[tail], take, &info);
            served = info.served;
            p->count += served;
        }
        if (!served) { break; }
        budget -= served;
    }
}
static bool tx_word(S3I2sStream *p, uint32_t *word, uint64_t *id,
                    unsigned slot)
{
    *id = 0;
    fifo_pump(p);
    if (p->count < p->sample_bytes) { return false; }
    uint32_t value = 0;
    for (unsigned i = 0; i < p->sample_bytes; ++i) {
        unsigned shift = (p->conf & BIT(7)) && !(p->conf & PDM) ?
                         8 * (p->sample_bytes - i - 1) : 8 * i;
        value |= (uint32_t)p->fifo[p->head] << shift;
        p->head = (p->head + 1) % S3_I2S_FIFO_BYTES;
        --p->count;
    }
    if (p->slot_bits > p->sample_bytes * 8 && (p->conf & BIT(15))) {
        value <<= p->slot_bits - p->sample_bytes * 8;
    }
    *word = value;
    if (p->owner->tx_transitions) {
        *id = ++p->owner->tx_source_word_id;
        S3I2sTxTransition event = tx_transition_event(p, S3_I2S_TX_LOAD);
        event.source_word_id = *id;
        event.source = S3_I2S_TX_PAYLOAD;
        event.slot = slot;
        if (p->conf & PDM) { event.valid_bits = event.slot_bits = 16; }
        tx_transition_append(p->owner, &event);
    }
    return true;
}
static void hung(S3I2sStream *p)
{
    ESP32S3I2sState *s = p->owner;
    uint32_t cfg = R(s, HUNG);
    p->starvation += p->slot_bits;
    /* Threshold is converted once when the stream configuration is latched. */
    if ((cfg & BIT(11)) && p->starvation >= p->timeout_cycles) {
        s->raw |= p->rx ? BIT(2) : BIT(3);
        irq_update(s);
    }
}
static bool record_sample(S3I2sStream *p, uint32_t word, bool accepted)
{
    ESP32S3I2sState *s = p->owner;
    if (!s->sample_record) { return true; }
    /* Immutable actual RX samples, not a host playback queue. Each record
     * has one resolved serial word and explicitly marks FIFO drops. */
    uint8_t event[32];
    stq_le_p(event, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    stq_le_p(event + 8, s->record_sequence++);
    stq_le_p(event + 16, p->frames);
    stl_le_p(event + 24, word);
    stw_le_p(event + 28, p->channel);
    event[30] = p->sample_bytes * 8;
    event[31] = accepted ? 3 : 5; /* valid + accepted/dropped */
    if (fwrite(event, sizeof(event), 1, s->sample_record) != 1) {
        dependency(p, "immutable sample recorder write failed");
        return false;
    }
    return true;
}
static void rx_word(S3I2sStream *p)
{
    ESP32S3I2sState *s = p->owner;
    uint32_t word = p->rx_word;
    unsigned storage_bits = p->sample_bytes * 8;
    if (p->slot_bits > storage_bits && (p->conf & BIT(15))) {
        word >>= p->slot_bits - storage_bits;
    }
    fifo_pump(p);
    bool accepted = p->count + p->sample_bytes <= S3_I2S_FIFO_BYTES;
    if (!record_sample(p, word, accepted)) { return; }
    if (p->count + p->sample_bytes > S3_I2S_FIFO_BYTES) {
        hung(p);
        if (((p->conf >> 13) & 3) == 2) {
            R(s, RX_CONF) &= ~START;
            p->enabled = false;
            s->raw |= BIT(0);
            irq_update(s);
        }
        return;
    }
    for (unsigned i = 0; i < p->sample_bytes; ++i) {
        unsigned shift = (p->conf & BIT(7)) && !(p->conf & PDM) ?
                         8 * (p->sample_bytes - i - 1) : 8 * i;
        unsigned tail = (p->head + p->count) % S3_I2S_FIFO_BYTES;
        p->fifo[tail] = word >> shift;
        ++p->count;
    }
    ++p->samples;
    p->starvation = 0;
    fifo_pump(p);
}
static unsigned logical_bit(S3I2sStream *p)
{
    return (p->wire_bit + p->frame_bits - !!(p->conf1 & BIT(29))) % p->frame_bits;
}
static bool selected(S3I2sStream *p, unsigned slot)
{
    return slot < p->slots && slot < 16 && !!(p->tdm & (1u << slot));
}
static bool pdm_transmit(S3I2sStream *p)
{
    ESP32S3I2sState *s = p->owner;
    uint32_t cfg = p->pdm_conf;
    if (!p->pdm.ready) {
        unsigned words = p->conf & BIT(5) ? 1 : 2;
        uint32_t first = 0, second = 0;
        uint64_t ids[2] = {0, 0};
        S3I2sTxSource sources[2] = {S3_I2S_TX_PAYLOAD, S3_I2S_TX_PAYLOAD};
        fifo_pump(p);
        if (p->count >= words * 2) {
            tx_word(p, &first, &ids[0], 0);
            if (words == 2) { tx_word(p, &second, &ids[1], 1); }
            p->last_frame[0] = first;
            p->last_frame[1] = second;
            p->last_frame_word_id[0] = ids[0];
            p->last_frame_word_id[1] = ids[1];
            p->frame_valid = 3;
            p->samples += words;
            p->starvation = 0;
        } else {
            hung(p);
            if ((p->conf & BIT(13)) && !(p->conf & SLAVE)) {
                tx_transition_stop(p, S3_I2S_TX_REASON_UNDERFLOW);
                p->enabled = false;
                s->raw |= BIT(1);
                irq_update(s);
                return false;
            }
            if (!(p->conf & BIT(13))) {
                if (!p->frame_valid) {
                    dependency(p, "raw PDM starved before a real DMA frame");
                    return false;
                }
                first = p->last_frame[0];
                second = p->last_frame[1];
                ids[0] = p->last_frame_word_id[0];
                ids[1] = p->last_frame_word_id[1];
                sources[0] = sources[1] = S3_I2S_TX_UNDERRUN_REPEAT;
            }
            if (p->conf & BIT(13)) {
                sources[0] = sources[1] = S3_I2S_TX_IDLE_ZERO;
            }
        }
        Esp32s3I2sPdmResult result = esp32s3_i2s_pdm_raw_tx_load(
            &p->pdm, p->conf, p->conf1, cfg, first, second, R(s, SINGLE));
        if (result != ESP32S3_I2S_PDM_OK) {
            dependency(p, "raw PDM frame format unavailable");
            return false;
        }
        if (s->tx_transitions) {
            /* Mirror only the helper's source identities, never its values.
             * Raw channel0 is left, channel1 right; stereo source units are
             * temporal FIFO order and WS polarity maps them to channels. */
            bool pol = p->conf & BIT(17);
            unsigned left = pol ? 1 : 0;
            unsigned right = pol ? 0 : 1;
            uint64_t channel_ids[2] = {ids[left], ids[right]};
            S3I2sTxSource channel_sources[2] = {sources[left], sources[right]};
            if (words == 1) {
                bool left_valid = !!(p->conf & BIT(9)) != pol;
                channel_ids[left_valid ? 0 : 1] = ids[0];
                channel_sources[left_valid ? 0 : 1] = sources[0];
                channel_ids[left_valid ? 1 : 0] = 0;
                channel_sources[left_valid ? 1 : 0] = S3_I2S_TX_SINGLE;
            }
            unsigned mode = (p->conf >> 24) & 7;
            if (mode == 1 || mode == 2) {
                unsigned from = mode == 1 ? !!pol : !pol;
                channel_ids[1 - from] = channel_ids[from];
                channel_sources[1 - from] = S3_I2S_TX_MONO_COPY;
            } else if (mode == 3 || mode == 4) {
                unsigned single = mode == 3 ? !!pol : !pol;
                channel_ids[single] = 0;
                channel_sources[single] = S3_I2S_TX_SINGLE;
            }
            if ((cfg & BIT(24)) && !(cfg & BIT(23))) {
                channel_ids[0] = channel_ids[1] = ids[0];
                channel_sources[0] = channel_sources[1] = sources[0];
            }
            if (!(cfg & BIT(24)) || (cfg & BIT(23))) {
                tx_transition_begin(p, 0, 0, channel_ids[0], channel_sources[0]);
            }
            tx_transition_begin(p, 1, 1, channel_ids[1], channel_sources[1]);
        }
    }
    uint8_t bits, mask;
    unsigned phase = p->ws ^ !!(p->conf & BIT(17));
    unsigned channel_mask = cfg & BIT(24) ? (cfg & BIT(23) ? 3 : 2) : 1u << phase;
    unsigned bit = p->pdm.counts[cfg & BIT(24) ? 1 : phase];
    if (esp32s3_i2s_pdm_raw_tx_edge(&p->pdm, p->conf, cfg, p->ws,
                                   &bits, &mask) != ESP32S3_I2S_PDM_OK) {
        dependency(p, "raw PDM phase has no real input word");
        return false;
    }
    if (!drive(s, data_signal[s->controller], mask & 1, bits & 1)) { return false; }
    if (!s->controller && !drive(s, 128, mask & 2, bits & 2)) { return false; }
    tx_transition_prepare(p, channel_mask, bit, phase, !(cfg & BIT(24)) || phase == 1);
    return true;
}
static bool pdm_receive(S3I2sStream *p)
{
    unsigned phase = p->ws ^ !!(p->conf & BIT(17));
    unsigned lines = p->owner->controller ? 1 : 4;
    uint8_t bits = 0, valid = 0;
    for (unsigned line = 0; line < lines; ++line) {
        unsigned channel = 2 * line + phase;
        if ((p->conf & BIT(5)) &&
            phase != !(!!(p->conf & BIT(9)) ^ !!(p->conf & BIT(17)))) { continue; }
        if (!(p->tdm & (1u << channel))) { continue; }
        bool value;
        unsigned signal = line ? 50 + line : data_signal[p->owner->controller];
        if (!sample(p, signal, &value)) { return false; }
        bits |= value << line;
        valid |= 1u << line;
    }
    Esp32s3I2sPdmResult result = esp32s3_i2s_pdm_raw_rx_edge(
        &p->pdm, p->conf, p->conf1, p->tdm, p->ws, bits, valid);
    if (result != ESP32S3_I2S_PDM_OK) {
        dependency(p, "raw PDM missing source or bounded frame overflow");
        return false;
    }
    uint8_t channel;
    uint16_t word;
    /* At most eight real completed channel words; no allocation or copying
     * proportional to stream length and no zero-time descriptor loop. */
    while (esp32s3_i2s_pdm_raw_rx_pop(&p->pdm, p->conf, &channel, &word) ==
           ESP32S3_I2S_PDM_OK) {
        p->rx_word = word;
        p->channel = channel;
        rx_word(p);
    }
    return true;
}
static bool transmit(S3I2sStream *p)
{
    ESP32S3I2sState *s = p->owner;
    if (p->conf & PDM) { return pdm_transmit(p); }
    unsigned logical = logical_bit(p);
    unsigned slot = logical / p->slot_bits;
    unsigned bit = logical % p->slot_bits;
    if (!bit) {
        bool active = selected(p, slot);
        bool consume = slot < p->slots && (active || (p->tdm & BIT(20)));
        uint32_t value;
        uint64_t id = 0;
        S3I2sTxSource source = S3_I2S_TX_IDLE_ZERO;
        if (consume && tx_word(p, &value, &id, slot)) {
            p->sample = value;
            p->source_word_id = id;
            source = S3_I2S_TX_PAYLOAD;
            p->sample_valid = true;
            p->previous_sample = value;
            p->previous_word_id = id;
            p->previous_valid = true;
            p->last_frame[slot] = value;
            p->last_frame_word_id[slot] = id;
            p->frame_valid |= 1u << slot;
            ++p->samples;
            p->starvation = 0;
        } else if (consume) {
            hung(p);
            if (p->conf & BIT(13)) {
                if (!(p->conf & SLAVE)) {
                    tx_transition_stop(p, S3_I2S_TX_REASON_UNDERFLOW);
                    p->enabled = false;
                    s->raw |= BIT(1);
                    irq_update(s);
                    return false;
                }
                /* TRM 28.8.1: slave STOP_EN starved output is zero. This is
                 * observable hardware idle, not an external sample source. */
                p->sample = 0;
                p->source_word_id = 0;
                p->sample_valid = true;
            } else if (p->frame_valid & (1u << slot)) {
                p->sample = p->last_frame[slot];
                p->source_word_id = p->last_frame_word_id[slot];
                source = S3_I2S_TX_UNDERRUN_REPEAT;
                p->sample_valid = true;
            } else {
                p->sample_valid = false;
                source = p->sample_source == S3_I2S_TX_IDLE_ZERO ?
                         S3_I2S_TX_IDLE_ZERO : S3_I2S_TX_UNDERRUN_REPEAT;
            }
        }
        if (!active) {
            p->sample = p->conf & BIT(6) ? p->previous_sample : R(s, SINGLE);
            p->sample_valid = !(p->conf & BIT(6)) || p->previous_valid;
            p->source_word_id = p->conf & BIT(6) ? p->previous_word_id : 0;
            source = p->conf & BIT(6) ? S3_I2S_TX_MONO_COPY : S3_I2S_TX_SINGLE;
        }
        tx_transition_begin(p, 0, slot, p->source_word_id, source);
        p->sample_source = source;
    }
    unsigned shift = p->conf & BIT(18) ? bit : p->slot_bits - bit - 1;
    p->data = (p->sample >> shift) & 1;
    /* An enabled S3 shifter drives SD, including initial shift-register
     * padding and documented hardware starvation, never a high-Z sample. */
    if (!drive(s, data_signal[s->controller], true, p->data)) { return false; }
    tx_transition_prepare(p, 1, bit, S3_I2S_TX_TRANSITION_NO_INDEX, true);
    return true;
}
static bool receive(S3I2sStream *p)
{
    if (p->conf & PDM) { return pdm_receive(p); }
    unsigned logical = logical_bit(p);
    unsigned slot = logical / p->slot_bits;
    p->channel = slot;
    unsigned bit = logical % p->slot_bits;
    if (!bit) { p->rx_word = 0; p->rx_bits = 0; }
    bool active = selected(p, slot);
    if ((p->conf & BIT(5)) && p->slots == 2) {
        active &= slot == ((p->conf & BIT(9)) ? 0 : 1);
    }
    if (active) {
        /* A Philips start can expose the tail of the preceding, incomplete
         * slot. It is not a sample and must not enter DMA or consume SD. */
        if (!p->rx_bits && bit) { return true; }
        bool value;
        if (!sample(p, data_signal[p->owner->controller], &value)) { return false; }
        unsigned shift = p->conf & BIT(18) ? bit : p->slot_bits - bit - 1;
        p->rx_word |= (uint32_t)value << shift;
        ++p->rx_bits;
        if (bit + 1 == p->slot_bits && p->rx_bits == p->slot_bits) { rx_word(p); }
    }
    return true;
}
static bool ws_level(S3I2sStream *p)
{
    if (p->conf & PDM) {
        return (p->wire_bit & 1) ^ !!(p->conf & BIT(17));
    }
    return (p->wire_bit >= ((p->conf1 & 127) + 1)) ^ !!(p->conf & BIT(17));
}
static void advance(S3I2sStream *p)
{
    if (++p->wire_bit == p->frame_bits) {
        p->wire_bit = 0;
        ++p->frames;
    }
}
static void schedule(S3I2sStream *p)
{
    if (!p->enabled || p->paused || !p->interval_den) { return; }
    uint64_t whole = p->interval_whole;
    p->fraction += p->interval_remainder;
    if (p->fraction >= p->interval_den) {
        ++whole;
        p->fraction -= p->interval_den;
    }
    p->next_ns += whole;
    timer_mod(p->timer, MAX(p->next_ns, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1));
}
static void stream_edge(void *opaque)
{
    S3I2sStream *p = opaque;
    ESP32S3I2sState *s = p->owner;
    if (!stream_running(p)) { return; }
    p->bclk = !p->bclk;
    unsigned clock_signal = p->rx ? rx_bclk[s->controller] : tx_bclk[s->controller];
    unsigned ws_signal = p->rx ? rx_ws[s->controller] : tx_ws[s->controller];
    if (!p->bclk) {
        p->ws = ws_level(p);
        if (!p->rx && !transmit(p)) {
            /* No physical edge was published. Keep phase consistent with
             * the held clock so a later genuine DMA arm resumes this edge. */
            p->bclk = !p->bclk;
            return;
        }
        if (p->rx && shared_tx(s) && !s->tx.external_source && stream_running(&s->tx)) {
            s->tx.wire_bit = p->wire_bit;
            s->tx.frames = p->frames;
            s->tx.ws = p->ws;
            if (!transmit(&s->tx)) { return; }
        }
        if (!drive(s, ws_signal, true, p->ws)) { return; }
    }
    if (!drive(s, clock_signal, true, p->bclk)) { return; }
    S3I2sStream *tx = !p->rx ? p :
        shared_tx(s) && !s->tx.external_source && stream_running(&s->tx) ? &s->tx : NULL;
    if (tx) {
        if (p->bclk) { tx_transition_boundary(tx, true); }
        else { tx_transition_publish(tx); }
    }
    /* Data is sampled on the actual resolved pad, even in full-duplex. Only
     * the clock-sharing path is internal, as selected by SIG_LOOPBACK. */
    if (p->bclk) {
        if (p->rx && !receive(p)) { return; }
        if (p->rx && !p->enabled && shared_tx(s)) {
            tx_transition_suspend(&s->tx, S3_I2S_TX_REASON_CLOCK);
        }
        if (!p->rx && shared_rx(s) && !s->rx.external_source && stream_running(&s->rx)) {
            s->rx.wire_bit = p->wire_bit;
            s->rx.frames = p->frames;
            s->rx.ws = p->ws;
            if (!receive(&s->rx)) { return; }
        }
        advance(p);
    }
    schedule(p);
}
static void divider(ESP32S3I2sState *s, bool rx, uint64_t *num,
                    uint64_t *den, uint64_t *hz)
{
    uint32_t clk = R(s, rx ? RX_CLK : TX_CLK);
    uint32_t frac = R(s, rx ? RX_DIV : TX_DIV);
    unsigned sel = (clk >> 27) & 3;
    *hz = sel == 0 ? clock_get_hz(s->xtal) :
          sel == 1 ? clock_get_hz(s->pll240) :
          sel == 2 ? clock_get_hz(s->pll160) : 0;
    unsigned n = clk & 255;
    if (!n) { n = 256; }
    if (n == 1) { n = 2; }
    uint64_t z = frac & 511, y = (frac >> 9) & 511, x = (frac >> 18) & 511;
    uint64_t a = z * (x + 1) + y;
    uint64_t b = (frac & BIT(27)) ? a - z : z;
    if (!z) { a = 1; b = 0; }
    *num = (uint64_t)n * a + b;
    *den = a;
}
static void mclk_schedule(ESP32S3I2sState *s)
{
    if (!s->mclk_den) { return; }
    uint64_t whole = s->mclk_whole;
    s->mclk_fraction += s->mclk_remainder;
    if (s->mclk_fraction >= s->mclk_den) {
        ++whole;
        s->mclk_fraction -= s->mclk_den;
    }
    s->mclk_next_ns += whole;
    timer_mod(s->mclk_timer, MAX(s->mclk_next_ns,
                               qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1));
}
static void mclk_edge(void *opaque)
{
    ESP32S3I2sState *s = opaque;
    if (!powered(s)) { return; }
    s->mclk_level = !s->mclk_level;
    if (!drive(s, s->controller ? 21 : 23, true, s->mclk_level)) { return; }
    mclk_schedule(s);
}
static void mclk_update(ESP32S3I2sState *s)
{
    bool rx = R(s, RX_CLK) & BIT(29);
    uint32_t cfg = R(s, rx ? RX_CLK : TX_CLK);
    uint64_t num, den, hz;
    divider(s, rx, &num, &den, &hz);
    bool routed = false;
    if (s->gpio) {
        unsigned signal = s->controller ? 21 : 23;
        for (unsigned pad = 0; pad < ESP32S3_GPIO_COUNT; ++pad) {
            ESP32S3GpioDriveSnapshot raw;
            esp32s3_gpio_get_drive_snapshot(s->gpio, pad, &raw);
            if (raw.matrix_gpio && raw.out_sel == signal) {
                routed = true;
                break;
            }
        }
    }
    /* Model wakes may accompany resolved waveform publication. An unchanged
     * clock/route must not restart its deadline, including from inside this
     * very clock's drive callback; that would schedule the halfperiod twice. */
    uint32_t key_cfg = cfg & 0x1c0000ff;
    uint32_t key_div = R(s, rx ? RX_DIV : TX_DIV) & 0x0fffffff;
    bool live = powered(s);
    if (s->mclk_key_valid && s->mclk_key_cfg == key_cfg &&
        s->mclk_key_div == key_div && s->mclk_key_hz == hz &&
        s->mclk_key_rx == rx && s->mclk_key_powered == live &&
        s->mclk_key_routed == routed) {
        return;
    }
    s->mclk_key_valid = true;
    s->mclk_key_cfg = key_cfg;
    s->mclk_key_div = key_div;
    s->mclk_key_hz = hz;
    s->mclk_key_rx = rx;
    s->mclk_key_powered = live;
    s->mclk_key_routed = routed;
    timer_del(s->mclk_timer);
    bool external = ((cfg >> 27) & 3) == 3;
    if (external != s->mclk_external) {
        s->mclk_external_phase = 0;
        s->mclk_last_input = false;
    }
    s->mclk_external = external;
    s->mclk_routed = routed;
    s->mclk_external_num = num;
    s->mclk_external_step = den;
    s->mclk_den = routed && powered(s) && (cfg & BIT(26)) ? hz * den * 2 : 0;
    if (external && powered(s) && (cfg & BIT(26))) {
        if (routed) { drive(s, s->controller ? 21 : 23, true, s->mclk_level); }
        return;
    }
    if (!s->mclk_den) {
        if (s->electrical) { drive(s, s->controller ? 21 : 23, false, s->mclk_level); }
        return;
    }
    uint64_t ns = num * 1000000000ULL;
    s->mclk_whole = ns / s->mclk_den;
    s->mclk_remainder = ns % s->mclk_den;
    s->mclk_next_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (!s->mclk_initialized) {
        s->mclk_initialized = true;
        s->mclk_level = false;
        s->mclk_fraction = 0;
        drive(s, s->controller ? 21 : 23, true, false);
    } else {
        s->mclk_fraction %= s->mclk_den;
    }
    mclk_schedule(s);
}
static bool configure(S3I2sStream *p)
{
    ESP32S3I2sState *s = p->owner;
    unsigned conf = p->rx ? RX_CONF : TX_CONF;
    if (!p->rx && (p->conf != (R(s, conf) & ~UPDATE) ||
                  p->conf1 != R(s, TX_CONF1) || p->tdm != R(s, TX_TDM) ||
                  p->timing != R(s, TX_TIMING) || p->pdm_conf != R(s, PDM_CONF) ||
                  p->pdm_conf1 != R(s, PDM_CONF1))) {
        tx_transition_suspend(p, S3_I2S_TX_REASON_CONFIG);
    }
    p->conf = R(s, conf) & ~UPDATE;
    p->conf1 = R(s, p->rx ? RX_CONF1 : TX_CONF1);
    p->tdm = R(s, p->rx ? RX_TDM : TX_TDM);
    p->timing = R(s, p->rx ? RX_TIMING : TX_TIMING);
    p->pdm_conf = R(s, PDM_CONF);
    p->pdm_conf1 = R(s, PDM_CONF1);
    p->data_bits = ((p->conf1 >> 13) & 31) + 1;
    p->slot_bits = ((p->conf1 >> 24) & 31) + 1;
    p->slots = ((p->tdm >> 16) & 15) + 1;
    p->frame_bits = (((p->conf1 >> 18) & 63) + 1) * 2;
    p->sample_bytes = (p->data_bits + 7) / 8;
    if (p->data_bits == 24 && (p->conf & BIT(16))) { p->sample_bytes = 4; }
    if (p->conf & PDM) {
        Esp32s3I2sPdmResult result = p->rx ?
            esp32s3_i2s_pdm_rx_mode(s->controller, p->conf, p->conf1, p->tdm) :
            esp32s3_i2s_pdm_tx_mode(s->controller, p->conf, p->conf1, p->pdm_conf);
        if (result != ESP32S3_I2S_PDM_OK) {
            dependency(p, result == ESP32S3_I2S_PDM_CONVERTER_UNDOCUMENTED ?
                "S3 PCM/PDM converter exact filter arithmetic/reference unavailable" :
                result == ESP32S3_I2S_PDM_UNSUPPORTED_PORT ?
                "PDM converter/line capability unsupported on this controller" :
                "PDM register format or output mapping unavailable");
            return false;
        }
        p->frame_bits = 32;
        p->sample_bytes = 2;
    } else if (!(p->conf & TDM)) {
        dependency(p, "both TDM and PDM disabled");
        return false;
    }
    if (p->data_bits != 8 && p->data_bits != 16 && p->data_bits != 24 && p->data_bits != 32) {
        dependency(p, "invalid valid-data width");
        return false;
    }
    if ((!(p->conf & PDM) && p->slot_bits * p->slots > p->frame_bits) ||
        p->timing || !(p->conf & BIT(12))) {
        dependency(p, "unqualified delayed IO, frame geometry or G.711 mode");
        return false;
    }
    uint32_t clk = R(s, p->rx ? RX_CLK : TX_CLK);
    uint64_t num, den, hz;
    divider(s, p->rx, &num, &den, &hz);
    unsigned bdiv = ((p->conf1 >> 7) & 63) + 1;
    bool external = ((clk >> 27) & 3) == 3;
    if (external != p->external_source) {
        p->external_phase = 0;
        p->external_ready = false;
    }
    p->external_source = external;
    p->external_step = den;
    p->external_threshold = num * bdiv;
    p->external_module_threshold = num;
    p->external_phase %= (p->conf & SLAVE) ? num : p->external_threshold;
    p->interval_num = num * bdiv * 1000000000ULL;
    p->interval_den = hz * den * 2;
    p->interval_whole = p->interval_den ? p->interval_num / p->interval_den : 0;
    p->interval_remainder = p->interval_den ? p->interval_num % p->interval_den : 0;
    if (p->interval_den) { p->fraction %= p->interval_den; }
    uint32_t hung_cfg = R(s, HUNG);
    uint64_t timeout_ticks = (uint64_t)(hung_cfg & 255) *
                             (88000u >> ((hung_cfg >> 8) & 7));
    p->timeout_cycles = DIV_ROUND_UP(timeout_ticks * den, num * bdiv);
    /* A disabled PLL source holds evolution; source selectors are not
     * replaced with a convenient fixed oscillator. External clocks arrive
     * through lossless resolved matrix notifications, not a guessed Hz. */
    return true;
}
static void start_stream(S3I2sStream *p)
{
    ESP32S3I2sState *s = p->owner;
    if (!powered(s) || !p->enabled || p->paused) { return; }
    uint32_t clk = R(s, p->rx ? RX_CLK : TX_CLK);
    if (!(clk & BIT(26))) { return; }
    if ((p->conf & SLAVE) || (p->rx && shared_rx(s))) { return; }
    if (!clock_available(p)) {
        if (p->external_source) {
            bool level;
            if (sample(p, s->controller ? 21 : 23, &level)) { p->last_mclk = level; }
        }
        return;
    }
    p->next_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (!p->initialized) {
        p->fraction = 0;
        if (p->conf & PDM) {
            /* Establish the inactive half-clock without consuming a word.
             * The first word is launched on a real future WS transition. */
            p->bclk = true;
            p->ws = !ws_level(p);
        } else {
            p->bclk = false;
            p->ws = ws_level(p);
            if (!p->rx && !transmit(p)) { return; }
        }
        if (p->rx && !(p->conf & PDM) && shared_tx(s) && stream_running(&s->tx)) {
            s->tx.wire_bit = p->wire_bit;
            s->tx.frames = p->frames;
            s->tx.ws = p->ws;
            if (!transmit(&s->tx)) { return; }
        }
        if (!drive(s, p->rx ? rx_ws[s->controller] : tx_ws[s->controller], true, p->ws)) { return; }
        if (!drive(s, p->rx ? rx_bclk[s->controller] : tx_bclk[s->controller], true, p->bclk)) { return; }
        if (!p->rx) { tx_transition_publish(p); }
        else if (shared_tx(s) && !s->tx.external_source && stream_running(&s->tx)) {
            tx_transition_publish(&s->tx);
        }
        p->initialized = true;
    }
    schedule(p);
}
static void reconcile(ESP32S3I2sState *s)
{
    S3I2sStream *streams[] = {&s->tx, &s->rx};
    for (unsigned i = 0; i < 2; ++i) {
        S3I2sStream *p = streams[i];
        timer_del(p->timer);
        bool started = R(s, p->rx ? RX_CONF : TX_CONF) & START;
        if (!started) {
            tx_transition_stop(p, S3_I2S_TX_REASON_REGISTER_STOP);
            p->enabled = false;
            p->initialized = false;
            drive(s, p->rx ? rx_bclk[s->controller] : tx_bclk[s->controller], false, p->bclk);
            drive(s, p->rx ? rx_ws[s->controller] : tx_ws[s->controller], false, p->ws);
            if (!p->rx) {
                /* TRM28.8.1: a stopped but clocked slave drives digital idle
                 * zero. This is hardware output state, not a sample fallback. */
                bool idle = powered(s) && (R(s, TX_CONF) & SLAVE) &&
                            (R(s, TX_CLK) & BIT(26));
                if (idle) { p->data = false; }
                drive(s, data_signal[s->controller], idle, p->data);
            }
            continue;
        }
        if (!p->enabled) {
            tx_transition_start(p, S3_I2S_TX_REASON_REGISTER_START);
            p->enabled = true;
            p->wire_bit = 0;
            p->synchronized = false;
            p->initialized = false;
            p->paused = false;
        }
        if (configure(p)) {
            if (!powered(s) || !clock_available(p)) {
                tx_transition_suspend(p, !powered(s) ? S3_I2S_TX_REASON_GATE :
                                                      S3_I2S_TX_REASON_CLOCK);
            }
            start_stream(p);
        }
    }
    if (shared_tx(s) && !stream_running(&s->rx)) {
        tx_transition_suspend(&s->tx, S3_I2S_TX_REASON_CLOCK);
    }
    mclk_update(s);
}
void esp32s3_i2s_routes_changed(ESP32S3I2sState *s)
{
    /* Raw mux/topology wake only. Never scan all pads at each audio edge. */
    mclk_update(s);
}
static bool slave_sample(S3I2sStream *p, bool clock, bool ws)
{
    if ((p->conf & PDM) && p->rx && shared_rx(p->owner)) {
        /* Shared raw RX samples the transmitter's internal BCLK rising
         * edge, after the physical WS halfphase and data have settled. */
        bool rising = !p->last_input_clock && clock;
        if (!p->synchronized && !clock && ws == !!(p->conf & BIT(17))) {
            p->synchronized = true;
        }
        if (rising && p->synchronized) {
            p->ws = ws;
            if (!receive(p)) { return false; }
            advance(p);
        }
        p->last_input_clock = clock;
        p->last_input_ws = ws;
        return true;
    }
    if (p->conf & PDM) {
        bool edge = ws != p->last_input_ws;
        if (!p->synchronized && edge && ws == !!(p->conf & BIT(17))) {
            p->synchronized = true;
        }
        if (edge && p->synchronized) {
            p->ws = ws;
            if (!(p->rx ? receive(p) : transmit(p))) { return false; }
            if (!p->rx) {
                tx_transition_publish(p);
                /* Raw slave hardware consumes resolved physical WS edges;
                 * it has no local BCLK sampling boundary to invent. Actual
                 * downstream DIN sampling time belongs to the peer record. */
                tx_transition_boundary(p, false);
            }
            advance(p);
        }
        p->last_input_ws = ws;
        return true;
    }
    bool falling = p->last_input_clock && !clock;
    bool rising = !p->last_input_clock && clock;
    if (p->last_input_ws != ws && ws == !!(p->conf & BIT(17))) {
        p->ws_boundary_pending = true;
    }
    if (!p->synchronized && !clock && ws == !!(p->conf & BIT(17))) {
        p->wire_bit = 0;
        p->synchronized = true;
        /* A genuine first falling edge is prepared by the branch below.
         * Preparing here as well would remove two FIFO words for one slot. */
        if (!p->rx && !falling && !transmit(p)) { return false; }
        if (!p->rx && !falling) { tx_transition_publish(p); }
    }
    if (falling) {
        if (p->ws_boundary_pending) {
            if (p->wire_bit) { tx_transition_abort(p, S3_I2S_TX_REASON_RESYNC); }
            p->wire_bit = 0;
            p->synchronized = true;
            p->ws_boundary_pending = false;
        }
        if (p->synchronized && !p->rx && !transmit(p)) { return false; }
        if (p->synchronized && !p->rx) { tx_transition_publish(p); }
    }
    if (rising && p->synchronized) {
        if (p->rx && !receive(p)) { return false; }
        if (!p->rx) { tx_transition_boundary(p, true); }
        advance(p);
    }
    p->last_input_clock = clock;
    p->last_input_ws = ws;
    return true;
}
void esp32s3_i2s_net_changed(ESP32S3I2sState *s)
{
    if (s->notifying || !powered(s)) { return; }
    s->notifying = true;
    bool have_external_level = false, external_level = false;
    bool mclk_rx = R(s, RX_CLK) & BIT(29);
    if (s->mclk_external && (s->mclk_routed || s->tx.enabled || s->rx.enabled) &&
        (R(s, mclk_rx ? RX_CLK : TX_CLK) & BIT(26))) {
        if (!sample(mclk_rx ? &s->rx : &s->tx, s->controller ? 21 : 23,
                    &external_level)) {
            s->notifying = false;
            return;
        }
        have_external_level = true;
        if (external_level != s->mclk_last_input) {
            s->mclk_last_input = external_level;
            s->mclk_external_phase += s->mclk_external_step;
            if (s->mclk_external_phase >= s->mclk_external_num) {
                s->mclk_external_phase -= s->mclk_external_num;
                s->mclk_level = !s->mclk_level;
                if (s->mclk_routed) {
                    drive(s, s->controller ? 21 : 23, true, s->mclk_level);
                }
            }
        }
    }
    S3I2sStream *external[] = {&s->tx, &s->rx};
    bool slave_module_pulse[2] = {false, false};
    for (unsigned i = 0; i < 2; ++i) {
        S3I2sStream *p = external[i];
        uint32_t cfg = R(s, p->rx ? RX_CLK : TX_CLK);
        if (!p->enabled || p->paused || !p->external_source ||
            !(cfg & BIT(26))) { continue; }
        if (!have_external_level) {
            if (!sample(p, s->controller ? 21 : 23, &external_level)) { break; }
            have_external_level = true;
        }
        bool level = external_level;
        if (level != p->last_mclk) {
            p->last_mclk = level;
            p->external_ready = true;
            if (!p->initialized) { start_stream(p); }
            p->external_phase += p->external_step;
            uint64_t threshold = p->conf & SLAVE ?
                                 p->external_module_threshold : p->external_threshold;
            if (p->external_phase >= threshold) {
                p->external_phase -= threshold;
                if (p->conf & SLAVE) {
                    slave_module_pulse[i] = true;
                    continue;
                }
                stream_edge(p);
            }
        }
    }
    S3I2sStream *streams[] = {&s->tx, &s->rx};
    for (unsigned i = 0; i < 2; ++i) {
        S3I2sStream *p = streams[i];
        if (!stream_running(p) || !(p->conf & SLAVE)) { continue; }
        bool shared = p->rx ? shared_rx(s) : shared_tx(s);
        /* An external module clock cannot be inferred from one old edge.
         * Slave decoding advances only on an actual divided module pulse,
         * so a known held external input freezes DMA and stream state. */
        if (p->external_source ? !slave_module_pulse[i] : shared) { continue; }
        bool clock = false, ws;
        if (shared) {
            S3I2sStream *master = p->rx ? &s->tx : &s->rx;
            clock = master->bclk;
            ws = master->ws;
        } else if (p->conf & PDM) {
            unsigned signal = p->rx || ((s->tx.conf & BIT(27)) &&
                                       (s->tx.conf & SLAVE)) ?
                              rx_ws[s->controller] : tx_ws[s->controller];
            if (!sample(p, signal, &ws)) { break; }
        } else {
            if (!sample(p, p->rx ? rx_bclk[s->controller] : tx_bclk[s->controller], &clock) ||
                !sample(p, p->rx ? rx_ws[s->controller] : tx_ws[s->controller], &ws)) { break; }
        }
        if (!slave_sample(p, clock, ws)) { break; }
    }
    s->notifying = false;
}
static void update_registers(void *opaque)
{
    ESP32S3I2sState *s = opaque;
    if (s->pending_update & 1) { R(s, TX_CONF) &= ~UPDATE; }
    if (s->pending_update & 2) { R(s, RX_CONF) &= ~UPDATE; }
    s->pending_update = 0;
    reconcile(s);
}
static uint64_t mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    ESP32S3I2sState *s = opaque;
    switch (addr) {
    case RAW: return s->raw;
    case ST: return s->raw & s->ena;
    case ENA: return s->ena;
    case CLR: return 0;
    case STATE: return !s->tx.enabled;
    default: return addr <= DATE ? R(s, addr) : 0;
    }
}
static void mmio_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    ESP32S3I2sState *s = opaque;
    if (addr > DATE || !s->gate || s->reset_asserted || !clock_get_hz(s->bus)) { return; }
    if (addr == ENA) { s->ena = value & 15; irq_update(s); return; }
    if (addr == CLR) { s->raw &= ~(value & 15); irq_update(s); return; }
    if (addr == RAW || addr == ST || addr == STATE || addr == DATE) { return; }
    R(s, addr) = value;
    if (addr == TX_CONF || addr == RX_CONF) {
        S3I2sStream *p = addr == TX_CONF ? &s->tx : &s->rx;
        if (value & BIT(0)) {
            tx_transition_reset(p, S3_I2S_TX_REASON_STREAM_RESET);
            if (p->rx && shared_tx(s)) {
                tx_transition_suspend(&s->tx, S3_I2S_TX_REASON_STREAM_RESET);
            }
            stream_reset(p);
        } else if (value & BIT(1)) {
            /* FIFO-only reset leaves the loaded shifter and frame cache
             * intact. Preserve its identity and actual completion tracking. */
            if (!p->rx && s->tx_transitions) {
                S3I2sTxTransition event = tx_transition_event(p, S3_I2S_TX_FIFO_RESET);
                event.reason = S3_I2S_TX_REASON_FIFO_RESET;
                event.flags |= S3_I2S_TX_FLAG_CLEAR_FIFO;
                tx_transition_append(s, &event);
            }
            fifo_reset(p);
        }
        if (value & UPDATE) {
            s->pending_update |= addr == TX_CONF ? 1 : 2;
            timer_mod(s->update_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1);
        } else if (!(value & 3)) { reconcile(s); }
    } else if (addr == TX_CLK || addr == RX_CLK || addr == TX_DIV || addr == RX_DIV) {
        reconcile(s);
    }
}
static const MemoryRegionOps mmio_ops = {
    .read = mmio_read, .write = mmio_write, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {.min_access_size = 4, .max_access_size = 4, .unaligned = false},
    .impl = {.min_access_size = 4, .max_access_size = 4},
};
static void clock_changed(void *opaque, ClockEvent event)
{
    ESP32S3I2sState *s = opaque;
    if (event == ClockUpdate) { reconcile(s); }
}
static void device_reset(DeviceState *dev)
{
    ESP32S3I2sState *s = ESP32S3_I2S(dev);
    tx_transition_reset(&s->tx, S3_I2S_TX_REASON_DEVICE_RESET);
    timer_del(s->update_timer);
    stream_reset(&s->tx);
    stream_reset(&s->rx);
    timer_del(s->mclk_timer);
    s->mclk_initialized = s->mclk_level = false;
    s->mclk_key_valid = false;
    s->mclk_den = s->mclk_fraction = 0;
    s->mclk_external = s->mclk_routed = s->mclk_last_input = false;
    s->mclk_external_phase = 0;
    memset(s->reg, 0, sizeof(s->reg));
    s->raw = s->ena = s->pending_update = 0;
    R(s, RX_CONF) = BIT(15) | BIT(12) | BIT(10) | BIT(9);
    R(s, TX_CONF) = BIT(15) | BIT(13) | BIT(12) | BIT(9);
    R(s, RX_CONF1) = BIT(29) | (6u << 7) | (15u << 13) |
                       (15u << 18) | (15u << 24);
    R(s, TX_CONF1) = R(s, RX_CONF1) | BIT(30);
    R(s, RX_CLK) = R(s, TX_CLK) = 2;
    R(s, HUNG) = BIT(11) | 16;
    R(s, DATE) = 0x2102080;
    irq_update(s);
    if (s->electrical) {
        drive(s, tx_bclk[s->controller], false, false);
        drive(s, tx_ws[s->controller], false, false);
        drive(s, rx_bclk[s->controller], false, false);
        drive(s, rx_ws[s->controller], false, false);
        drive(s, data_signal[s->controller], false, false);
        drive(s, s->controller ? 21 : 23, false, false);
        if (!s->controller) { drive(s, 128, false, false); }
    }
}
static void reset_hold(Object *obj, ResetType type)
{
    device_reset(DEVICE(obj));
}
static void gate_input(void *opaque, int n, int level)
{
    ESP32S3I2sState *s = opaque;
    s->gate = level;
    reconcile(s);
}
static void reset_input(void *opaque, int n, int level)
{
    ESP32S3I2sState *s = opaque;
    if (level && !s->reset_asserted) { device_reset(DEVICE(s)); }
    s->reset_asserted = level;
    reconcile(s);
}
static void gdma_armed(ESPGdmaState *gdma, GdmaPeripheral peripheral,
                       uint32_t channel, int direction, void *opaque)
{
    ESP32S3I2sState *s = opaque;
    S3I2sStream *p = direction == ESP_GDMA_IN_IDX ? &s->rx : &s->tx;
    if (!powered(s) || !clock_available(p) || p->paused ||
        !(R(s, p->rx ? RX_CONF : TX_CONF) & START)) {
        return;
    }
    fifo_pump(p);
    if (!p->rx && !p->enabled && p->sample_bytes && p->count >= p->sample_bytes) {
        tx_transition_start(p, S3_I2S_TX_REASON_GDMA_REARM);
        p->enabled = true;
        start_stream(p);
    }
}
static void realize(DeviceState *dev, Error **errp)
{
    ESP32S3I2sState *s = ESP32S3_I2S(dev);
    if (s->controller > 1 || !s->gdma || !s->gpio) {
        error_setg(errp, "I2S requires S3 controller0/1, GPIO and GDMA links");
        return;
    }
    if (s->record_directory) {
        g_autofree char *path = g_strdup_printf("%s/i2s%u.rx.bin",
                                                s->record_directory, s->controller);
        s->sample_record = fopen(path, "wbx");
        if (!s->sample_record) {
            error_setg_errno(errp, errno, "Cannot create immutable I2S RX record '%s'", path);
            return;
        }
        setvbuf(s->sample_record, NULL, _IONBF, 0);
        uint8_t header[16] = {'S', '3', 'I', '2', 'S', 'R', 'X', '1'};
        stl_le_p(header + 8, s->controller);
        stl_le_p(header + 12, 1);
        if (fwrite(header, sizeof(header), 1, s->sample_record) != 1) {
            error_setg_errno(errp, errno, "Cannot write immutable I2S RX header");
        }
        s->tx_transitions = g_new0(S3I2sTxTransition, S3_I2S_TX_TRANSITION_CAPACITY);
    }
    esp_gdma_set_peripheral_notify(s->gdma, s->controller ? GDMA_I2S1 : GDMA_I2S0,
                                   gdma_armed, s);
}
static void instance_init(Object *obj)
{
    ESP32S3I2sState *s = ESP32S3_I2S(obj);
    s->tx.owner = s->rx.owner = s;
    s->tx_activation_ns = -1;
    s->rx.rx = true;
    s->tx.timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, stream_edge, &s->tx);
    s->rx.timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, stream_edge, &s->rx);
    s->update_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, update_registers, s);
    s->mclk_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, mclk_edge, s);
    s->bus = qdev_init_clock_in(DEVICE(s), "bus", clock_changed, s, ClockUpdate);
    s->xtal = qdev_init_clock_in(DEVICE(s), "xtal", clock_changed, s, ClockUpdate);
    s->pll160 = qdev_init_clock_in(DEVICE(s), "pll160", clock_changed, s, ClockUpdate);
    s->pll240 = qdev_init_clock_in(DEVICE(s), "pll240", clock_changed, s, ClockUpdate);
    qdev_init_gpio_in_named(DEVICE(s), reset_input, "reset", 1);
    qdev_init_gpio_in_named(DEVICE(s), gate_input, "clk-gate", 1);
    memory_region_init_io(&s->iomem, obj, &mmio_ops, s, TYPE_ESP32S3_I2S, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->irq);
}
static void instance_finalize(Object *obj)
{
    ESP32S3I2sState *s = ESP32S3_I2S(obj);
    timer_free(s->tx.timer);
    timer_free(s->rx.timer);
    timer_free(s->update_timer);
    timer_free(s->mclk_timer);
    if (s->sample_record) { fclose(s->sample_record); }
    g_free(s->tx_transitions);
}
static Property properties[] = {
    DEFINE_PROP_UINT32("controller", ESP32S3I2sState, controller, 0),
    DEFINE_PROP_STRING("record-directory", ESP32S3I2sState, record_directory),
    DEFINE_PROP_LINK("gdma", ESP32S3I2sState, gdma, TYPE_ESP_GDMA, ESPGdmaState *),
    DEFINE_PROP_LINK("gpio", ESP32S3I2sState, gpio, TYPE_ESP32S3_GPIO, ESP32S3GPIOState *),
    DEFINE_PROP_LINK("electrical", ESP32S3I2sState, electrical, TYPE_ESP32S3_ELECTRICAL, DeviceState *),
    DEFINE_PROP_END_OF_LIST(),
};
static void class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = realize;
    RESETTABLE_CLASS(klass)->phases.hold = reset_hold;
    device_class_set_props(dc, properties);
}
static const TypeInfo type_info = {
    .name = TYPE_ESP32S3_I2S, .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ESP32S3I2sState), .instance_init = instance_init,
    .instance_finalize = instance_finalize, .class_init = class_init,
};
static void register_types(void) { type_register_static(&type_info); }
type_init(register_types)
