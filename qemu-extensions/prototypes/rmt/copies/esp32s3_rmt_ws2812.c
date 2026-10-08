/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Bounded WS2812 functional sink; authoritative sampling/power/timers live in
 * the native electrical peer wrapper. See the header for profile and sources.
 */
#include "qemu/osdep.h"
#include "hw/timer/esp32s3_rmt_ws2812.h"

static void count_event(uint64_t *counter)
{
    if (*counter != UINT64_MAX) {
        ++*counter;
    }
}

static bool in_window(uint64_t ns, unsigned min, unsigned max)
{
    return ns >= min && ns <= max;
}

static void clear_frame(Esp32S3RmtWs2812 *s)
{
    s->pending_bits = 0;
    s->frame_active = false;
    s->frame_bad = false;
    s->high_started = false;
    s->bit_pending = false;
}

static void malformed_frame(Esp32S3RmtWs2812 *s)
{
    if (!s->frame_bad) {
        count_event(&s->errors);
        count_event(&s->malformed_frames);
    }
    s->frame_bad = true;
    s->high_started = false;
    s->bit_pending = false;
}

static void append_bit(Esp32S3RmtWs2812 *s)
{
    unsigned bit = s->pending_bits;

    if (bit < s->led_count * 24) {
        unsigned byte = bit / 8;

        /* Every byte is overwritten before reuse; no full-frame clearing. */
        if (!(bit % 8)) {
            s->pending[byte] = 0;
        }
        s->pending[byte] |= (uint8_t)s->pending_one << (7 - bit % 8);
        ++s->pending_bits;
    } else {
        /* A sink for the first N LEDs does not store downstream chain data. */
        count_event(&s->forwarded_bits);
    }
    s->bit_pending = false;
}

bool esp32s3_rmt_ws2812_init(Esp32S3RmtWs2812 *s, unsigned led_count)
{
    memset(s, 0, sizeof(*s));
    if (!led_count || led_count > ESP32S3_RMT_WS2812_MAX_LEDS) {
        return false;
    }
    s->led_count = led_count;
    s->synchronized = true; /* Actual power-on reset, not an input edge. */
    return true;
}

void esp32s3_rmt_ws2812_reset(Esp32S3RmtWs2812 *s)
{
    unsigned led_count = s->led_count;

    esp32s3_rmt_ws2812_init(s, led_count);
}

int64_t esp32s3_rmt_ws2812_deadline(const Esp32S3RmtWs2812 *s)
{
    if (!s->led_count || !s->sample_valid || s->sample_level ||
        s->reset_done ||
        s->low_since_ns > INT64_MAX - ESP32S3_RMT_WS2812_RESET_NS) {
        return -1;
    }
    return s->low_since_ns + ESP32S3_RMT_WS2812_RESET_NS;
}

void esp32s3_rmt_ws2812_expire(Esp32S3RmtWs2812 *s, uint64_t ns)
{
    if (!s->led_count || !s->sample_valid || s->sample_level ||
        s->reset_done || ns < s->low_since_ns ||
        ns - s->low_since_ns < ESP32S3_RMT_WS2812_RESET_NS) {
        return;
    }

    if (s->synchronized && s->frame_active && !s->frame_bad) {
        /*
         * The actual final high was validated at its falling edge. Its low
         * has merged into an observed reset, so there is no following rising
         * edge from which a normal bit-low or cell period could be measured.
         */
        if (s->bit_pending) {
            append_bit(s);
        }
        if (s->pending_bits == s->led_count * 24) {
            memcpy(s->latched, s->pending, s->led_count * 3);
            s->latch_valid = true;
            count_event(&s->frame_count);
        } else {
            count_event(&s->errors);
            count_event(&s->partial_frames);
        }
    }
    clear_frame(s);
    s->synchronized = true;
    s->reset_done = true;
}

void esp32s3_rmt_ws2812_input(Esp32S3RmtWs2812 *s, bool valid, bool level,
                            uint64_t ns)
{
    uint64_t duration;

    if (!s->led_count) {
        return;
    }
    if (!valid) {
        /* Cancel before expiry: unknown is never evidence of continuous low. */
        if (!s->unknown_input) {
            count_event(&s->errors);
            count_event(&s->unknown_inputs);
        }
        s->unknown_input = true;
        s->sample_valid = false;
        s->synchronized = false;
        s->reset_done = false;
        clear_frame(s);
        return;
    }
    s->unknown_input = false;
    if (!s->sample_valid) {
        /* Establish a baseline only: a sampled high is not a rising edge. */
        s->sample_valid = true;
        s->sample_level = level;
        s->reset_done = false;
        if (!level) {
            s->low_since_ns = ns;
        }
        return;
    }

    /* The old valid low is known through this sample, including at an edge. */
    esp32s3_rmt_ws2812_expire(s, ns);
    if (level == s->sample_level) {
        return;
    }
    s->sample_level = level;
    s->reset_done = false;
    if (!level) {
        s->low_since_ns = ns;
        if (s->high_started) {
            s->high_started = false;
            if (s->pending_bits == s->led_count * 24) {
                /* This pulse belongs to unmodeled downstream DOUT data. */
                s->bit_pending = true;
                return;
            }
            duration = ns - s->high_since_ns;
            if (in_window(duration, 250, 550)) {
                s->pending_one = false;
            } else if (in_window(duration, 650, 950)) {
                s->pending_one = true;
            } else {
                malformed_frame(s);
                return;
            }
            s->pending_high_ns = duration;
            s->bit_pending = true;
        }
        return;
    }

    if (!s->synchronized || s->frame_bad) {
        return;
    }
    if (s->bit_pending) {
        duration = ns - s->low_since_ns;
        if (s->pending_bits < s->led_count * 24 &&
            (!in_window(duration, s->pending_one ? 300 : 700,
                        s->pending_one ? 600 : 1000) ||
             !in_window(s->pending_high_ns + duration, 950, 1550))) {
            malformed_frame(s);
            return;
        }
        append_bit(s);
    }
    s->frame_active = true;
    s->high_started = true;
    s->high_since_ns = ns;
}
