/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_ESP32S3_I2S_H
#define HW_ESP32S3_I2S_H
#include "hw/sysbus.h"
#include "hw/clock.h"
#include "hw/dma/esp_gdma.h"
#include "hw/gpio/esp32s3_gpio.h"
#include "hw/misc/esp32s3_i2s_pdm.h"
#include <stdio.h>
#include "qemu/timer.h"
#define TYPE_ESP32S3_I2S "esp32s3-i2s"
OBJECT_DECLARE_SIMPLE_TYPE(ESP32S3I2sState, ESP32S3_I2S)
#define S3_I2S_FIFO_BYTES 256
#define S3_I2S_REG_WORDS 33

/* Version 1 is payload-free attribution of native TX state, not a data oracle.
 * LOAD means successful FIFO removal, never serial completion. COMPLETE
 * requires every slot bit (16 distinct bits/channel for raw PDM) to reach a
 * real clock boundary after successful data publication. Word IDs survive
 * replay/copy; zero denotes register/idle data, never a FIFO payload.
 * Epochs advance on TX start/rearm and shifter/device reset. Reset records
 * state which hardware fields clear; capture retention and IDs never reset.
 * The once-allocated ring retains newest records, with total/lost and an
 * inclusive first_sequence making every overwritten record observable. */
#define S3_I2S_TX_TRANSITION_VERSION 1
#define S3_I2S_TX_TRANSITION_CAPACITY 16384
#define S3_I2S_TX_TRANSITION_NO_INDEX UINT32_MAX
#define S3_I2S_TX_TRANSITION_NO_PHASE UINT64_MAX
typedef enum S3I2sTxTransitionKind {
    S3_I2S_TX_START, S3_I2S_TX_ACTIVATE, S3_I2S_TX_STOP,
    S3_I2S_TX_RESET, S3_I2S_TX_FIFO_RESET, S3_I2S_TX_SUSPEND,
    S3_I2S_TX_LOAD, S3_I2S_TX_COMPLETE, S3_I2S_TX_ABORT,
} S3I2sTxTransitionKind;
typedef enum S3I2sTxSource {
    S3_I2S_TX_PAYLOAD, S3_I2S_TX_SINGLE, S3_I2S_TX_MONO_COPY,
    S3_I2S_TX_UNDERRUN_REPEAT, S3_I2S_TX_IDLE_ZERO,
} S3I2sTxSource;
/* PAYLOAD identifies a new FIFO source; MONO_COPY/UNDERRUN_REPEAT reuse its
 * ID. A raw channel is the helper's left(0)/right(1) channel, not temporal
 * FIFO order (WS polarity may swap those). NO_INDEX/NO_PHASE and time -1
 * mean not applicable/not observed. Halfphase indices count actual raw
 * WS/data publications globally; initial inactive WS is not a halfphase.
 * CLEAR_POSITION refers to hardware wire phase, not global IDs/ordinals. */
typedef enum S3I2sTxTransitionReason {
    S3_I2S_TX_REASON_NONE, S3_I2S_TX_REASON_REGISTER_START,
    S3_I2S_TX_REASON_GDMA_REARM, S3_I2S_TX_REASON_REGISTER_STOP,
    S3_I2S_TX_REASON_UNDERFLOW, S3_I2S_TX_REASON_GATE,
    S3_I2S_TX_REASON_CLOCK, S3_I2S_TX_REASON_DEPENDENCY,
    S3_I2S_TX_REASON_STREAM_RESET, S3_I2S_TX_REASON_DEVICE_RESET,
    S3_I2S_TX_REASON_FIFO_RESET, S3_I2S_TX_REASON_CONFIG,
    S3_I2S_TX_REASON_RESYNC,
} S3I2sTxTransitionReason;
enum {
    S3_I2S_TX_FLAG_RAW = 1u << 0,
    S3_I2S_TX_FLAG_SLAVE = 1u << 1,
    S3_I2S_TX_FLAG_SHARED = 1u << 2,
    S3_I2S_TX_FLAG_CLEAR_FIFO = 1u << 3,
    S3_I2S_TX_FLAG_CLEAR_SHIFTER = 1u << 4,
    S3_I2S_TX_FLAG_CLEAR_FRAME_CACHE = 1u << 5,
    S3_I2S_TX_FLAG_CLEAR_POSITION = 1u << 6,
};
typedef struct S3I2sTxTransition {
    uint64_t sequence, epoch, source_word_id, word_ordinal, frame;
    int64_t ns, first_ns, last_ns, first_boundary_ns, last_boundary_ns;
    uint64_t first_halfphase, last_halfphase;
    uint32_t kind, reason, flags, source, slot, raw_channel, physical_phase;
    uint32_t valid_bits, slot_bits, shifted_bits;
} S3I2sTxTransition;
typedef struct S3I2sTxTransitionWindow {
    const S3I2sTxTransition *records;
    uint64_t epoch, total, lost, first_sequence;
    int64_t activation_ns;
    uint32_t version, controller, capacity, count, first_index;
    bool active;
} S3I2sTxTransitionWindow;
typedef struct S3I2sTxWordPhase {
    S3I2sTxTransition event;
    unsigned published_bits;
    bool pending, published;
} S3I2sTxWordPhase;

typedef struct S3I2sStream {
    struct ESP32S3I2sState *owner;
    QEMUTimer *timer;
    uint32_t conf, conf1, tdm, timing, pdm_conf, pdm_conf1;
    uint8_t fifo[S3_I2S_FIFO_BYTES];
    unsigned head, count;
    uint32_t sample, previous_sample, rx_word;
    uint32_t last_frame[16];
    uint16_t frame_valid;
    Esp32s3I2sPdmRawState pdm;
    unsigned wire_bit, slot_bits, data_bits, slots, frame_bits, sample_bytes;
    unsigned rx_bits, rx_bytes, channel;
    uint64_t interval_num, interval_den, fraction;
    uint64_t interval_whole, interval_remainder, timeout_cycles;
    uint64_t external_phase, external_step, external_threshold, external_module_threshold;
    int64_t next_ns;
    uint64_t frames, samples, starvation;
    bool rx, bclk, ws, data, enabled, paused, synchronized;
    bool initialized, ws_boundary_pending;
    bool external_source, last_mclk, external_ready;
    bool sample_valid, previous_valid, last_input_clock, last_input_ws;
    /* TX attribution only; no sample values in these identities/records. */
    uint64_t source_word_id, previous_word_id, last_frame_word_id[16];
    S3I2sTxSource sample_source;
    S3I2sTxWordPhase tx_phase[2];
    unsigned prepared_bit, prepared_mask, prepared_phase;
    int64_t prepared_ns;
    bool prepared, prepared_advances;
} S3I2sStream;

typedef struct ESP32S3I2sState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    Clock *bus, *xtal, *pll160, *pll240;
    ESPGdmaState *gdma;
    ESP32S3GPIOState *gpio;
    DeviceState *electrical;
    char *record_directory;
    FILE *sample_record;
    uint64_t record_sequence;
    uint32_t controller, reg[S3_I2S_REG_WORDS];
    uint32_t raw, ena;
    S3I2sStream tx, rx;
    QEMUTimer *update_timer;
    QEMUTimer *mclk_timer;
    uint64_t mclk_den, mclk_fraction, mclk_whole, mclk_remainder;
    uint64_t mclk_external_num, mclk_external_step, mclk_external_phase;
    int64_t mclk_next_ns;
    bool mclk_level, mclk_initialized, mclk_external, mclk_routed, mclk_last_input;
    uint32_t mclk_key_cfg, mclk_key_div;
    uint64_t mclk_key_hz;
    bool mclk_key_valid, mclk_key_rx, mclk_key_powered, mclk_key_routed;
    uint8_t pending_update;
    bool reset_asserted, notifying, gate;
    S3I2sTxTransition *tx_transitions;
    uint64_t tx_transition_total, tx_epoch, tx_source_word_id, tx_word_ordinal;
    uint64_t tx_halfphase;
    int64_t tx_activation_ns;
    bool tx_transition_active, tx_transition_started;
} ESP32S3I2sState;
/* Called after a native electrical solve, at the actual current virtual time.
 * This consumes resolved slave clock edges, never desired output levels. */
void esp32s3_i2s_net_changed(ESP32S3I2sState *s);
void esp32s3_i2s_routes_changed(ESP32S3I2sState *s);
/* Cold, read-only view: false means record-directory capture was not enabled.
 * Index i is records[(first_index + i) % capacity], 0 <= i < count.
 * Pointers remain valid until finalize, contents until subsequent evolution;
 * a synchronous QMP export must consume the view without advancing the VM.
 * This call never resets hardware, clocks, epochs, RX records or retention. */
bool esp32s3_i2s_tx_transition_window(const ESP32S3I2sState *s,
                                    S3I2sTxTransitionWindow *window);
#endif
