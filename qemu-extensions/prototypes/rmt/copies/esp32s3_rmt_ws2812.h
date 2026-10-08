/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_TIMER_ESP32S3_RMT_WS2812_H
#define HW_TIMER_ESP32S3_RMT_WS2812_H

#include <stdbool.h>
#include <stdint.h>

#define ESP32S3_RMT_WS2812_MAX_LEDS 256
#define ESP32S3_RMT_WS2812_MAX_BYTES (ESP32S3_RMT_WS2812_MAX_LEDS * 3)
#define ESP32S3_RMT_WS2812_RESET_NS 50000

/*
 * ws2812-functional-3v3: a fixed functional profile, NOT measured silicon or
 * a claim that classic WS2812(B) operates within specification at 3.3 V.
 * Inclusive timing windows, in ns: T0H 250..550, T0L 700..1000,
 * T1H 650..950, T1L 300..600, cell 950..1550; reset valid low >=50000.
 * Primary protocol: Worldsemi WS2812B LED-V2.0, data method/GRB order:
 * https://d2j2m4p6r3pg95.cloudfront.net/module_files/led-cube/assets/datasheets/WS2812B.pdf
 * Classic 400/800/850/450 ns timing reference (datasheet reproduction):
 * https://www.luxalight.eu/sites/default/files/downloads/2020-03/Datasheet_WS2812B.pdf
 * Revisions differ; this named profile deliberately fixes the values above.
 * No PWM, optical, thermal, current-load or DOUT propagation model is implied.
 *
 * The owner supplies authoritative published terminal samples and virtual-time
 * expiry callbacks, with monotonic ns timestamps in QEMU's int64_t time range.
 * input(false, ...) is unknown, not low: it cancels decoding/reset timing and
 * requires a fresh real low reset to resynchronize. No initial sample is an
 * edge. Only reset() denotes an actual power cycle; an unknown power interval
 * must invalidate input without clearing the previously latched power epoch.
 *
 * The last bit's valid high pulse can lead directly into reset; that low is
 * observed as reset, not assigned a fabricated nominal bit-low/cell duration.
 * Extra bits represent data forwarded beyond the configured first N pixels:
 * values/timing are ignored after the first N words, and counters saturate.
 * A timing-malformed first N words, an incomplete first N pixels,
 * or unknown input cannot replace the held frame. An empty low reset neither
 * changes latched data nor increments frame_count.
 */
typedef struct Esp32S3RmtWs2812 {
    /* Public inspection: first led_count*3 bytes, in MSB-decoded GRB order. */
    unsigned led_count;
    uint8_t latched[ESP32S3_RMT_WS2812_MAX_BYTES];
    uint64_t frame_count;
    bool latch_valid;
    /* All counters saturate. errors counts malformed/partial/unknown events. */
    uint64_t errors;
    uint64_t malformed_frames;
    uint64_t partial_frames;
    uint64_t unknown_inputs;
    uint64_t forwarded_bits; /* Ignored downstream high pulses, not decoded. */

    /* Bounded staging and decoder state; owned by the engine. */
    uint8_t pending[ESP32S3_RMT_WS2812_MAX_BYTES];
    unsigned pending_bits;
    uint64_t high_since_ns;
    uint64_t low_since_ns;
    uint64_t pending_high_ns;
    bool sample_valid;
    bool sample_level;
    bool unknown_input;
    bool synchronized;
    bool frame_active;
    bool frame_bad;
    bool high_started;
    bool bit_pending;
    bool pending_one;
    bool reset_done;
} Esp32S3RmtWs2812;

/* Initializes state; rejects led_count outside 1..256, leaving it disabled. */
bool esp32s3_rmt_ws2812_init(Esp32S3RmtWs2812 *s, unsigned led_count);
/* Clears the actual power epoch, including pixels/counters; retains count. */
void esp32s3_rmt_ws2812_reset(Esp32S3RmtWs2812 *s);
void esp32s3_rmt_ws2812_input(Esp32S3RmtWs2812 *s, bool valid, bool level,
                            uint64_t ns);
/* Absolute virtual ns of the next low-reset expiry, or -1 if none. */
int64_t esp32s3_rmt_ws2812_deadline(const Esp32S3RmtWs2812 *s);
void esp32s3_rmt_ws2812_expire(Esp32S3RmtWs2812 *s, uint64_t ns);

#endif
