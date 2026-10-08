/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_TIMER_ESP32S3_RMT_NEC_H
#define HW_TIMER_ESP32S3_RMT_NEC_H

#include <stdbool.h>
#include <stdint.h>

/* A functional, powered, active-low demodulated NEC envelope source, not
 * ESP32-S3 RMT RX demodulation, an optical carrier/AGC model, or measured
 * receiver silicon. The wrapper alone owns rails, terminal drive and the
 * QEMU_CLOCK_VIRTUAL timer; this state has no electrical or pad API. With
 * known 2.7..3.6 V supply span, OUT is a finite 40-ohm open-drain ground
 * branch: output_enable && !level enables it. Logical high releases OUT,
 * never drives a high voltage; an explicit external pull-up and the actual
 * electrical kernel must establish a physical high, with no implicit pull. */
typedef enum Esp32S3RmtNecPhase {
    ESP32S3_RMT_NEC_WAIT,
    ESP32S3_RMT_NEC_LEAD_MARK,
    ESP32S3_RMT_NEC_LEAD_SPACE,
    ESP32S3_RMT_NEC_BIT_MARK,
    ESP32S3_RMT_NEC_BIT_SPACE,
    ESP32S3_RMT_NEC_FINAL_MARK,
} Esp32S3RmtNecPhase;

typedef struct Esp32S3RmtNecOutput {
    bool output_enable;        /* Logical powered waveform permission. */
    bool level;                /* True: release; false: sink if enabled. */
    int64_t deadline_ns;        /* Absolute virtual ns, or -1: no timer. */
} Esp32S3RmtNecOutput;

typedef struct Esp32S3RmtNec {
    /* Public observations; mutation is through the functions below only. */
    uint8_t address;
    uint8_t command;
    uint32_t period_ms;
    bool power_known;
    bool powered;              /* False while rails are unknown. */
    uint64_t power_on_ns;
    uint64_t power_epoch;
    uint64_t frame_count;       /* Completed frames since init, saturating. */
    uint64_t frame_abort_count; /* Late unschedulable attempts, saturating. */

    /* Fixed-size private waveform state: no queues or per-edge allocation. */
    uint32_t frame_word;
    uint64_t period_ns;
    uint64_t first_frame_ns;
    uint64_t frame_start_ns;
    int64_t next_deadline_ns;
    Esp32S3RmtNecPhase phase;
    uint8_t bit_index;
    bool configured;
    bool have_epoch;
    bool anchor_valid;
    bool level;
} Esp32S3RmtNec;

/* address/command: 0..255; period_ms: 100..60000. Invalid init leaves a
 * disabled, unconfigured state; invalid configure leaves state unchanged.
 * The configured bytes are immutable source data, never derived from RX. */
bool esp32s3_rmt_nec_init(Esp32S3RmtNec *s, uint32_t address,
                         uint32_t command, uint32_t period_ms);

/* Identical configuration is a no-op. A semantic change cancels the current
 * waveform and installs a new dataset whose first whole frame is now+100 ms
 * if an epoch has already been observed. Before any epoch, the first frame
 * remains actual power_on+100 ms. A later actual epoch overrides this anchor.
 * No frame/error count is invented by an intentional dataset replacement. */
bool esp32s3_rmt_nec_configure(Esp32S3RmtNec *s, uint32_t address,
                              uint32_t command, uint32_t period_ms,
                              uint64_t now_ns);

/* Caller supplies authoritative solved rail state and virtual timestamps.
 * Epochs identify actual power cycles; power_on_ns is stable within an epoch.
 * Known-off cancels every future edge and releases OUT. Unknown rails also
 * release/cancel, but retain the dataset, epoch and frame anchor. Restoring
 * the same epoch schedules the next strictly-future whole anchored frame,
 * never a partial frame or an invented power cycle. An actual known epoch
 * change resets the waveform to actual power_on_ns+100 ms. */
void esp32s3_rmt_nec_power(Esp32S3RmtNec *s, bool known, bool powered,
                          uint64_t power_on_ns, uint64_t epoch,
                          uint64_t now_ns);

int64_t esp32s3_rmt_nec_deadline(const Esp32S3RmtNec *s);

/* Times are monotonic QEMU virtual ns, not host wall time. At most one
 * physical OUT transition is returned per call. A due callback advances one
 * phase at now if its next ideal edge is still future; slight callback
 * lateness does not discard a frame. Otherwise the unschedulable attempt is
 * counted as aborted, OUT releases to logical idle-high, and arithmetic skips
 * to a whole future frame. There is no replay of past edges or repeated zero
 * timer.
 * Frames are first+k*period, with 9 ms low, 4.5 ms high, 32 LSB-first bits
 * [address, ~address, command, ~command], 560 us low marks, 560/1690 us high
 * spaces, and a final 560 us low mark. Idle always releases the open-drain
 * branch; physical high depends on the explicit actual-net pull-up, not the
 * logical level. All deadlines and entire scheduled frames fit INT64_MAX;
 * unrepresentable future frames have deadline -1, without time wrapping. */
Esp32S3RmtNecOutput esp32s3_rmt_nec_step(Esp32S3RmtNec *s, uint64_t now_ns);

#endif
