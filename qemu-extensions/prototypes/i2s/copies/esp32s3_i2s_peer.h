/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_MISC_ESP32S3_I2S_PEER_H
#define HW_MISC_ESP32S3_I2S_PEER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "qapi/error.h"
#include "qapi/qmp/qdict.h"

typedef enum S3I2sPeerRole {
    S3_I2S_PEER_MASTER, S3_I2S_PEER_SLAVE,
} S3I2sPeerRole;
typedef enum S3I2sPeerFormat {
    S3_I2S_PEER_PHILIPS, S3_I2S_PEER_MSB, S3_I2S_PEER_PCM,
    S3_I2S_PEER_TDM, S3_I2S_PEER_RAW_PDM,
} S3I2sPeerFormat;
typedef struct S3I2sPeerConfig {
    S3I2sPeerRole role;
    S3I2sPeerFormat format;
    uint32_t data_bits, slot_bits, slots, slot_mask, ws_width;
    bool ws_pol, bit_shift, lsb_first, left_align;
    uint64_t rate_num_hz;
    uint32_t rate_den;
    bool repeat;
    uint32_t capture_capacity;
    char identity[65]; /* Canonical config + immutable vector SHA256. */
    /* Owned, immutable once moved into a peer. Samples are normalized to
     * data_bits, in frame order then ascending selected-slot order. Raw bits
     * are chronological physical CLK (= terminal WS) halfphases, not BCLK
     * edges: bit0 belongs to the first actual transition into WS_POL
     * (phase0), then alternating phase1/phase0. Publishing the inactive
     * initial WS level is not an edge. Raw S3 BCLK is twice the physical
     * PDM clock. The master prepares before each physical edge and holds
     * through the entire halfphase; it captures DIN at the following actual
     * internal BCLK rising edge, after slave source reactions settle. The
     * slave updates at each physical edge (bit0 already preloaded), captures
     * the immutable edge DIN, and holds DOUT for the S3 master's later BCLK
     * sample. Exhaustion occurs before the next unprovided physical edge,
     * never by truncating the last legitimate halfphase. Exactly one vector. */
    uint32_t *tx_samples;
    size_t tx_sample_count;
    uint8_t *raw_bits;
    size_t raw_bit_count;
} S3I2sPeerConfig;

bool esp32s3_i2s_peer_parse(const QDict *component, S3I2sPeerConfig *config,
                          Error **errp);
void esp32s3_i2s_peer_config_clear(S3I2sPeerConfig *config);
bool esp32s3_i2s_peer_config_validate(const S3I2sPeerConfig *config, Error **errp);

typedef enum S3I2sPeerTerminal {
    S3_I2S_PEER_VDD, S3_I2S_PEER_GND, S3_I2S_PEER_BCLK,
    S3_I2S_PEER_WS, S3_I2S_PEER_DIN, S3_I2S_PEER_DOUT,
} S3I2sPeerTerminal;

typedef struct S3I2sPeerPower {
    bool known, powered;
    double vdd, gnd;
    uint64_t power_epoch;
    uint64_t generation;
    uint64_t power_on_ns;
} S3I2sPeerPower;

typedef void (*S3I2sPeerNotify)(void *opaque, int64_t ns);
typedef struct S3I2sPeerProvider {
    void *opaque;
    /* Returns false if the provider itself is unavailable. Rail validity is
     * separate from power state; powered must use the explicit 2.7..3.6 V
     * vdd-gnd profile, never assumed rails. */
    bool (*power)(void *opaque, S3I2sPeerPower *power);
    /* Read the actual resolved terminal at ns. Returns false for provider
     * failure; valid=false for floating, contended, or unknown inputs. */
    bool (*sample)(void *opaque, S3I2sPeerTerminal terminal, int64_t ns,
                   bool *valid, bool *level);
    /* Rail-referenced push-pull, 40 ohm, using explicit current vdd/gnd.
     * Outer calls solve synchronously. Inside a solved-frame observer round,
     * queue the drive for the next solve delta, keeping the current consumed
     * frame immutable; settle it before the outer publication returns.
     * False means unavailable. */
    bool (*drive)(void *opaque, S3I2sPeerTerminal terminal, bool oe, bool level);
    /* REQUIRED for master role. Publish these three role drives atomically,
     * then solve and dispatch the coherent actual frame. No callbacks may
     * observe partially initialized clocks/data. Same rails and 40 ohm. */
    bool (*drive_frame)(void *opaque, bool bclk_oe, bool bclk,
                        bool ws_oe, bool ws, bool dout_oe, bool dout);
    /* Register solved-frame AND model/power wakes. Exactly one subscription
     * per instance. No immediate callback inside subscribe. Notifications
     * caused by drive may reenter; the service guards that reentrancy.
     * A solved frame must be immutable for its observer dispatch round:
     * all edge sinks see the original DIN/DOUT. Source updates queued by
     * an observer become visible only in the following solved delta. Raw
     * slave output then remains stable through the whole clock halfphase. */
    bool (*subscribe)(void *opaque, S3I2sPeerNotify notify, void *notify_opaque);
    void (*unsubscribe)(void *opaque, S3I2sPeerNotify notify, void *notify_opaque);
    /* Required persistent dependency reporting. Service pauses the VM before
     * returning from unknown input/power, exhaustion, or capture overflow. */
    void (*dependency)(void *opaque, const char *reason, int64_t ns);
} S3I2sPeerProvider;

typedef struct S3I2sPeer S3I2sPeer;
typedef struct S3I2sPeerCapture {
    int64_t ns;
    uint64_t sequence;
    uint32_t sample;
    uint16_t slot;
    bool raw;
} S3I2sPeerCapture;
typedef struct S3I2sPeerStatus {
    bool powered, paused, synchronized, exhausted, capture_overflow;
    /* transmitted counts successfully published complete outgoing words or
     * physical halfphases, independently of DIN. It is not a resolved DOUT
     * data oracle. captured counts actual DIN records, never inferred TX. */
    uint64_t power_epoch, transmitted, captured;
    size_t capture_count;
    const char *dependency;
    const char *config_identity;
} S3I2sPeerStatus;

/* Payload-free native outgoing attribution. BEGIN follows successful source
 * publication; COMPLETE follows every published slot bit and its actual
 * sampling boundary (raw: physical halfphase edge). No DIN success proxy.
 * Ring storage is allocated once: 2*capture_capacity+16 records. Overwrite
 * retains newest records and reports every loss; reads never touch wire state. */
/* source_id is unique across repeats/power resets in one config instance.
 * source_cursor is the consumed immutable vector index, not a sample value.
 * Raw word_ordinal/frame/bit_phase are physical halfphase ordinal divided by
 * 16/32/modulo16 within the epoch; channel/phase are vector index modulo2.
 * Raw slot is the source vector's 16-bit packing half ((cursor/16)%2),
 * not the alternating DIN capture slot/physical phase (cursor%2).
 * Both indices are retained explicitly; slot must not relabel a WS phase.
 * PCM word_ordinal is source_id-1 and frame is the actual wire frame.
 * Absent lifecycle timestamps are -1; absent phase indices are UINT*_MAX.
 * ns and all emitted/boundary times use the actual QEMU virtual clock. */
/* ACTIVATE is appended only after a real provider endpoint establishes
 * generation, power origin/epoch and known/powered state, before serial source
 * initialization. Unknown rails and known-off rails remain distinct. */
typedef enum S3I2sPeerTransitionKind {
    S3_I2S_PEER_TRANSITION_ACTIVATE,
    S3_I2S_PEER_TRANSITION_POWER_EPOCH,
    S3_I2S_PEER_TRANSITION_BEGIN,
    S3_I2S_PEER_TRANSITION_COMPLETE,
    S3_I2S_PEER_TRANSITION_ABORT,
    S3_I2S_PEER_TRANSITION_RELEASE,
    S3_I2S_PEER_TRANSITION_EXHAUSTED,
} S3I2sPeerTransitionKind;
typedef enum S3I2sPeerSource {
    S3_I2S_PEER_SOURCE_PAYLOAD,
    S3_I2S_PEER_SOURCE_EXHAUSTED,
    S3_I2S_PEER_SOURCE_RELEASED,
} S3I2sPeerSource;
typedef struct S3I2sPeerTransition {
    uint64_t sequence, epoch, power_epoch, generation;
    uint64_t power_on_ns;
    uint64_t source_id, source_cursor, word_ordinal, frame;
    uint64_t first_halfphase, last_halfphase;
    int64_t ns, first_ns, last_ns, first_boundary_ns, last_boundary_ns;
    uint32_t kind, source, slot, raw_channel, physical_phase, bit_phase;
    uint32_t valid_bits, slot_bits, published_bits;
    bool raw, power_known, powered;
} S3I2sPeerTransition;
typedef struct S3I2sPeerTransitionWindow {
    uint32_t version;
    uint64_t generation, epoch, total, lost, first_sequence;
    int64_t config_activation_ns, activation_ns;
    uint64_t power_on_ns;
    size_t capacity, count, first_index;
    const S3I2sPeerTransition *records;
} S3I2sPeerTransitionWindow;
void esp32s3_i2s_peer_transition_window(const S3I2sPeer *peer,
                                      S3I2sPeerTransitionWindow *window);

/* Success moves both vectors and zeroes owned_config. Failure leaves it
 * intact. Factory allocates timer and bounded capture once; no edge allocation.
 * new does not drive. start is called after registry routing is installed. */
S3I2sPeer *esp32s3_i2s_peer_new(S3I2sPeerConfig *owned_config,
                               const S3I2sPeerProvider *provider, Error **errp);
/* true means successful lifecycle subscription/setup, including a genuine
 * runtime dependency pause observed while starting. Such pauses remain in
 * status.paused/dependency and stop the VM; they never produce Error.
 * false + Error is only invalid lifecycle state or subscription/setup failure,
 * which the service reports as activation_error. No rail/data fallback. */
bool esp32s3_i2s_peer_start(S3I2sPeer *peer, Error **errp);
void esp32s3_i2s_peer_notify(void *peer, int64_t ns);
void esp32s3_i2s_peer_status(const S3I2sPeer *peer, S3I2sPeerStatus *status);
/* Immutable append-only storage, valid until free. Power epochs reset wire
 * state and playlist position, not already observed capture records. */
const S3I2sPeerCapture *esp32s3_i2s_peer_capture(const S3I2sPeer *peer,
                                             size_t *count);
void esp32s3_i2s_peer_free(S3I2sPeer *peer);

#endif
