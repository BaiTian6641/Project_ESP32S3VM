/* SPDX-License-Identifier: GPL-2.0-or-later
 * ESP32-S3 LCD_CAM: one register block, shared interrupt, independent GDMA
 * directions. Every output is a native GPIO-matrix desired drive; CAM samples
 * only resolved GPIO-matrix inputs. No panel/sensor framebuffer shortcut.
 * Register/clock/signal definitions: locked ESP-IDF 5.5.5 ESP32-S3 LCD_CAM HAL.
 */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-clock.h"
#include "hw/misc/esp32s3_lcd_cam.h"
#include "hw/misc/esp32s3_electrical.h"
#include "hw/xtensa/esp32s3_reset_domain.h"
#include "qapi/visitor.h"
#include "qemu/bitops.h"
#define BIT(n) (1u << (n))
#define R(s, o) ((s)->reg[(o) / 4])
#define L(s, o) ((s)->lcd_active[(o) / 4])
#define C(s, o) ((s)->cam_active[(o) / 4])
enum { LCD_CLOCK=0, CAM_CTRL=4, CAM_CTRL1=8, CAM_CONV=12,
       LCD_CONV=16, LCD_USER=20, LCD_MISC=24, LCD_CTRL=28,
       LCD_CTRL1=32, LCD_CTRL2=36, LCD_CMD=40, LCD_DELAY=48,
       LCD_DATA_DELAY=56, INT_ENA=100, INT_RAW=104, INT_ST=108,
       INT_CLR=112, DATE=252 };
enum { PH_FRONT, PH_COMMAND, PH_DUMMY, PH_DATA, PH_BACK, PH_IDLE };
enum { SIG_CS=132, SIG_DATA=133, SIG_CAM_CLK=149, SIG_DE=150,
       SIG_HSYNC=151, SIG_VSYNC=152, SIG_DC=153, SIG_LCD_CLK=154 };
static void lcd_edge(void *opaque);
static void reconcile(ESP32S3LcdCamState *s);
static bool available(ESP32S3LcdCamState *s)
{
    return s->gate && !s->reset_asserted && clock_get_hz(s->bus);
}
static void irq_update(ESP32S3LcdCamState *s)
{
    qemu_set_irq(s->irq, available(s) && !!(s->raw & s->ena));
}
static void event(ESP32S3LcdCamState *s, uint32_t mask)
{
    s->raw |= mask;
    irq_update(s);
}
static Clock *source(ESP32S3LcdCamState *s, uint32_t reg)
{
    switch ((reg >> 29) & 3) {
    case 1: return s->xtal;
    case 2: return s->pll240;
    case 3: return s->pll160;
    default: return NULL;
    }
}
/* Rational half-period; accumulated remainder avoids drift at 160/240 MHz. */
static bool period_config(ESP32S3LcdCamState *s, S3LcdCamPeriod *p,
                          uint32_t reg, bool lcd)
{
    Clock *clk = source(s, reg);
    uint64_t hz = clk ? clock_get_hz(clk) : 0;
    unsigned n = (reg >> 9) & 255, a = (reg >> 23) & 63;
    unsigned b = (reg >> 17) & 63;
    unsigned prescale = lcd && !(reg & BIT(6)) ? (reg & 63) + 1 : 1;
    if (!hz || !available(s)) { return false; }
    if (!n) { n = 256; }
    else if (n == 1) { n = 2; } /* TRM 29.3.3 divider field encoding. */
    if (!a) { a = 1; b = 0; }
    uint64_t numerator = UINT64_C(1000000000) * (n * a + b) * prescale;
    uint64_t denominator = hz * a * 2;
    p->whole = numerator / denominator;
    p->remainder = numerator % denominator;
    p->denominator = denominator;
    p->fraction = 0;
    p->next_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    return p->whole != 0; /* No unrepresentable zero-time edge loops. */
}
static void schedule(S3LcdCamPeriod *p, QEMUTimer *timer)
{
    p->next_ns += p->whole;
    p->fraction += p->remainder;
    if (p->fraction >= p->denominator) {
        p->fraction -= p->denominator;
        ++p->next_ns;
    }
    timer_mod(timer, p->next_ns);
}
static void drive(ESP32S3LcdCamState *s, unsigned signal, bool oe, bool level)
{
    esp32s3_electrical_set_matrix_drive(s->electrical, signal, oe, level, false);
}
static unsigned delayed_signal(unsigned index)
{
    static const unsigned control[] = { SIG_DC, SIG_DE, SIG_HSYNC, SIG_VSYNC };
    return index < 16 ? SIG_DATA + index : control[index - 16];
}
static int64_t delay_deadline(ESP32S3LcdCamState *s, bool rising)
{
    S3LcdCamPeriod *p = &s->lcd_module_period;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (!p->denominator) { return now; }
    uint64_t elapsed = MAX(now - s->lcd_module_anchor_ns, 0);
    __uint128_t half = (__uint128_t)p->whole * p->denominator + p->remainder;
    uint64_t edge = ((__uint128_t)elapsed * p->denominator) / half + 1;
    if ((edge & 1) != rising) { ++edge; }
    int64_t deadline = s->lcd_module_anchor_ns + edge * p->whole +
                       ((__uint128_t)edge * p->remainder) / p->denominator;
    if (deadline <= now) {
        edge += 2;
        deadline = s->lcd_module_anchor_ns + edge * p->whole +
                   ((__uint128_t)edge * p->remainder) / p->denominator;
    }
    return deadline;
}
static void lcd_delay_schedule(ESP32S3LcdCamState *s)
{
    uint32_t pending = s->lcd_drive_pending;
    if (!pending) {
        if (timer_pending(s->lcd_delay_timer)) { timer_del(s->lcd_delay_timer); }
        return;
    }
    int64_t next = INT64_MAX;
    while (pending) {
        unsigned i = ctz32(pending);
        pending &= pending - 1;
        next = MIN(next, s->lcd_drive_deadline[i]);
    }
    timer_mod(s->lcd_delay_timer, next);
}
static void lcd_delay_commit(void *opaque)
{
    ESP32S3LcdCamState *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    esp32s3_electrical_begin_update(s->electrical);
    uint32_t pending = s->lcd_drive_pending;
    while (pending) {
        unsigned i = ctz32(pending);
        pending &= pending - 1;
        if (s->lcd_drive_deadline[i] <= now) {
            s->lcd_drive_pending &= ~BIT(i);
            drive(s, delayed_signal(i), available(s) && (s->lcd_drive_value[i] & 2),
                  s->lcd_drive_value[i] & 1);
        }
    }
    esp32s3_electrical_end_update(s->electrical);
    lcd_delay_schedule(s);
}
static void lcd_delayed_drive(ESP32S3LcdCamState *s, unsigned index,
                               unsigned mode, bool oe, bool level,
                               int64_t deadline[2])
{
    uint8_t value = (oe ? 2 : 0) | level;
    if (s->lcd_drive_value[index] == value && s->lcd_drive_mode[index] == mode) { return; }
    s->lcd_drive_value[index] = value;
    s->lcd_drive_mode[index] = mode;
    s->lcd_drive_pending &= ~BIT(index);
    if (!oe || !mode || mode == 3) {
        /* Mode 3 is reserved. Keep its physical line unresolved rather than
         * interpreting an undocumented delay as mode 0. */
        drive(s, delayed_signal(index), oe && mode != 3, level);
    } else {
        if (deadline[mode - 1] < 0) { deadline[mode - 1] = delay_deadline(s, mode == 1); }
        s->lcd_drive_deadline[index] = deadline[mode - 1];
        s->lcd_drive_pending |= BIT(index);
    }
}
static void lcd_drives(ESP32S3LcdCamState *s, bool oe, bool clk,
                       bool dc, bool de, bool hs, bool vs)
{
    esp32s3_electrical_begin_update(s->electrical);
    unsigned bits = L(s, LCD_USER) & BIT(23) ? 16 : 8;
    int64_t deadline[2] = { -1, -1 };
    for (unsigned i = 0; i < 16; ++i) {
        lcd_delayed_drive(s, i, (L(s, LCD_DATA_DELAY) >> (2 * i)) & 3,
                          oe && i < bits, (s->lcd_word >> i) & 1, deadline);
    }
    drive(s, SIG_CS, oe, !s->lcd_running);
    lcd_delayed_drive(s, 16, L(s, LCD_DELAY) & 3, oe, dc, deadline);
    lcd_delayed_drive(s, 17, (L(s, LCD_DELAY) >> 2) & 3, oe, de, deadline);
    lcd_delayed_drive(s, 18, (L(s, LCD_DELAY) >> 4) & 3, oe, hs, deadline);
    lcd_delayed_drive(s, 19, (L(s, LCD_DELAY) >> 6) & 3, oe, vs, deadline);
    drive(s, SIG_LCD_CLK, oe, clk);
    esp32s3_electrical_end_update(s->electrical);
    lcd_delay_schedule(s);
}
static bool dc_for(ESP32S3LcdCamState *s, unsigned phase)
{
    bool idle = L(s, LCD_MISC) & BIT(31);
    unsigned bit = phase == PH_COMMAND ? 30 : phase == PH_DUMMY ? 29 : 28;
    return phase == PH_IDLE ? idle : idle ^ !!(L(s, LCD_MISC) & BIT(bit));
}
static void lcd_idle(ESP32S3LcdCamState *s)
{
    uint32_t ctrl = L(s, LCD_CTRL2);
    s->lcd_clock_level = !!(L(s, LCD_CLOCK) & BIT(7));
    lcd_drives(s, available(s), s->lcd_clock_level, dc_for(s, PH_IDLE),
               !!(ctrl & BIT(8)), !!(ctrl & BIT(23)), !!(ctrl & BIT(7)));
}
static uint16_t ordered_word(uint16_t word, unsigned bits, uint32_t user)
{
    if (bits == 16 && (user & BIT(22))) { word = (word << 8) | (word >> 8); }
    if (user & BIT(21)) {
        uint16_t reversed = 0;
        for (unsigned i = 0; i < bits; ++i) { reversed |= ((word >> i) & 1) << (bits-1-i); }
        word = reversed;
    }
    return bits == 8 ? word & 255 : word;
}
static void tx_refill(ESP32S3LcdCamState *s)
{
    unsigned channel;
    if (s->tx_eof || s->tx_fault || s->tx_count == S3_LCDCAM_FIFO_BYTES ||
        !esp_gdma_get_channel_periph(s->gdma, GDMA_LCDCAM, ESP_GDMA_OUT_IDX, &channel)) {
        return;
    }
    s->tx_channel = channel;
    unsigned tail = (s->tx_head + s->tx_count) % S3_LCDCAM_FIFO_BYTES;
    unsigned take = MIN(S3_LCDCAM_FIFO_BYTES - s->tx_count, S3_LCDCAM_FIFO_BYTES-tail);
    EspGdmaTxInfo info;
    bool fulfilled = esp_gdma_read_channel_ex(s->gdma, channel, &s->tx_fifo[tail], take, &info);
    s->tx_count += info.served;
    s->tx_eof |= info.eof_reached;
    if (info.eof_reached) {
        s->last_lcd_eof_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    }
    s->tx_list_done |= info.list_done;
    /* A short request at EOF is expected. Other short requests may be
     * temporarily descriptor-quantum limited; never lose fulfilled bytes. */
    if (!fulfilled && !info.served && !info.eof_reached && !info.list_done) {
        s->lcd_stalled = true;
    }
}
static bool tx_word(ESP32S3LcdCamState *s)
{
    unsigned bits = L(s, LCD_USER) & BIT(23) ? 16 : 8;
    unsigned bytes = bits / 8;
    bool swap8 = bits == 8 && !!(L(s, LCD_USER) & BIT(19));
    tx_refill(s);
    if (s->tx_count < bytes || (swap8 && !(s->data_cycle & 1) && s->tx_count < 2)) {
        ++s->tx_underflow;
        s->lcd_stalled = true;
        return false;
    }
    uint16_t word;
    if (swap8) {
        /* Swap the pair in place once; then consume in the physical order. */
        if (!(s->data_cycle & 1)) {
            unsigned next = (s->tx_head + 1) % S3_LCDCAM_FIFO_BYTES;
            uint8_t first = s->tx_fifo[s->tx_head];
            s->tx_fifo[s->tx_head] = s->tx_fifo[next];
            s->tx_fifo[next] = first;
        }
    }
    word = s->tx_fifo[s->tx_head];
    s->tx_head = (s->tx_head + 1) % S3_LCDCAM_FIFO_BYTES;
    if (bytes == 2) {
        word |= s->tx_fifo[s->tx_head] << 8;
        s->tx_head = (s->tx_head + 1) % S3_LCDCAM_FIFO_BYTES;
    }
    s->tx_count -= bytes;
    s->lcd_word = ordered_word(word, bits, L(s, LCD_USER));
    s->lcd_stalled = false;
    return true;
}
static unsigned phase_length(ESP32S3LcdCamState *s, unsigned phase)
{
    uint32_t user = L(s, LCD_USER), misc = L(s, LCD_MISC);
    switch (phase) {
    case PH_FRONT: return misc & BIT(26) ? ((misc >> 6) & 63) + 1 : 0;
    case PH_COMMAND: return user & BIT(26) ? (user & BIT(31) ? 2 : 1) : 0;
    case PH_DUMMY: return user & BIT(25) ? ((user >> 29) & 3) + 1 : 0;
    case PH_DATA: return user & BIT(24) ? (user & BIT(13) ? UINT_MAX : (user & 8191) + 1) : 0;
    case PH_BACK: return misc & BIT(26) ? ((misc >> 12) & 8191) + 1 : 0;
    default: return 0;
    }
}
static void lcd_finish(ESP32S3LcdCamState *s)
{
    s->lcd_running = false;
    R(s, LCD_USER) &= ~BIT(27);
    L(s, LCD_USER) &= ~BIT(27);
    s->lcd_phase = PH_IDLE;
    timer_del(s->lcd_timer);
    lcd_idle(s);
    /* The final physical strobe and hold phase have already finished. */
    s->last_lcd_done_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    event(s, BIT(1));
}
static bool i80_prepare(ESP32S3LcdCamState *s)
{
    while (s->lcd_phase < PH_IDLE) {
        unsigned length = phase_length(s, s->lcd_phase);
        if (s->lcd_phase == PH_DATA && length == UINT_MAX &&
            (s->tx_eof || s->tx_list_done) && !s->tx_count) {
            length = 0;
        }
        if (s->phase_cycle >= length) {
            ++s->lcd_phase;
            s->phase_cycle = 0;
            continue;
        }
        if (s->lcd_phase == PH_COMMAND) {
            s->lcd_word = (L(s, LCD_CMD) >> (16 * s->phase_cycle)) & 65535;
        } else if (s->lcd_phase == PH_DATA) {
            if (!tx_word(s)) {
                if (length == UINT_MAX && (s->tx_eof || s->tx_list_done) && !s->tx_count) {
                    ++s->lcd_phase;
                    s->phase_cycle = 0;
                    continue;
                }
                return false;
            }
        }
        return true;
    }
    lcd_finish(s);
    return false;
}
static bool rgb_active_pixel(ESP32S3LcdCamState *s)
{
    unsigned first_x = (L(s, LCD_CTRL) & 2047) + 1;
    unsigned first_y = (L(s, LCD_CTRL1) & 255) + 1;
    unsigned width = ((L(s, LCD_CTRL1) >> 8) & 4095) + 1;
    unsigned height = ((L(s, LCD_CTRL) >> 11) & 1023) + 1;
    return s->lcd_x >= first_x && s->lcd_x < first_x + width &&
           s->lcd_y >= first_y && s->lcd_y < first_y + height;
}
static void rgb_levels(ESP32S3LcdCamState *s, bool clk)
{
    uint32_t ctrl = L(s, LCD_CTRL2);
    unsigned first_y = (L(s, LCD_CTRL1) & 255) + 1;
    unsigned height = ((L(s, LCD_CTRL) >> 11) & 1023) + 1;
    unsigned hpos = (ctrl >> 24) & 255;
    bool hs = s->lcd_x >= hpos && s->lcd_x < hpos + ((ctrl >> 16) & 127) + 1;
    hs &= !!(ctrl & BIT(9)) || (s->lcd_y >= first_y && s->lcd_y < first_y + height);
    bool vs = s->lcd_y < (ctrl & 127) + 1;
    bool de = rgb_active_pixel(s);
    lcd_drives(s, true, clk, dc_for(s, PH_IDLE), de ^ !!(ctrl & BIT(8)),
               hs ^ !!(ctrl & BIT(23)), vs ^ !!(ctrl & BIT(7)));
}
static void rgb_advance(ESP32S3LcdCamState *s)
{
    unsigned total_x = (L(s, LCD_CTRL1) >> 20) + 1;
    unsigned total_y = ((L(s, LCD_CTRL) >> 21) & 1023) + 1;
    ++s->lcd_x;
    if (s->lcd_x >= total_x) {
        s->lcd_x = 0;
        ++s->lcd_y;
    }
    if (s->lcd_y == (L(s, LCD_CTRL2) & 127) + 1 && !s->lcd_x) {
        event(s, BIT(0)); /* VSYNC pulse end, not first active pixel. */
    }
    if (s->lcd_y < total_y) { return; }
    ++s->lcd_frames;
    s->lcd_y = 0;
    if (s->lcd_pending_update) {
        memcpy(s->lcd_active, s->reg, sizeof(s->lcd_active));
        s->lcd_pending_update = false;
        period_config(s, &s->lcd_period, L(s, LCD_CLOCK), true);
        period_config(s, &s->lcd_module_period, L(s, LCD_CLOCK), false);
        s->lcd_module_anchor_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    }
    s->tx_eof = false;
    s->data_cycle = 0;
    if (!(L(s, LCD_MISC) & BIT(25))) { lcd_finish(s); }
}
static void lcd_edge(void *opaque)
{
    ESP32S3LcdCamState *s = opaque;
    if (!available(s) || !s->lcd_running) { return; }
    bool first_level = !!(L(s, LCD_CLOCK) & BIT(8));
    bool first = s->lcd_clock_level != first_level;
    bool rgb = !!(L(s, LCD_CTRL) & BIT(31));
    if (first) {
        if (rgb) {
            if (rgb_active_pixel(s) && !tx_word(s)) {
                /* Functional starvation profile: freeze coordinates/PCLK at
                 * idle until real DMA payload becomes available. Never emit
                 * invented pixels or fake a completed VSYNC/frame. */
                s->lcd_clock_level = !!(L(s, LCD_CLOCK) & BIT(7));
                rgb_levels(s, s->lcd_clock_level);
                timer_del(s->lcd_timer);
                return;
            }
        } else if (!i80_prepare(s)) {
            return;
        }
    }
    s->lcd_clock_level = first ? first_level : !first_level;
    if (rgb) {
        rgb_levels(s, s->lcd_clock_level);
    } else {
        bool blank = s->lcd_phase == PH_FRONT || s->lcd_phase == PH_BACK;
        bool wire_clock = blank ? !!(L(s, LCD_CLOCK) & BIT(7)) : s->lcd_clock_level;
        lcd_drives(s, true, wire_clock, dc_for(s, blank ? PH_IDLE : s->lcd_phase),
                   s->lcd_phase == PH_DATA, false, false);
    }
    if (!first) {
        if (rgb) {
            if (rgb_active_pixel(s)) { ++s->lcd_words; ++s->data_cycle; }
            rgb_advance(s);
        } else {
            ++s->phase_cycle;
            if (s->lcd_phase == PH_DATA) { ++s->lcd_words; ++s->data_cycle; }
            if (s->phase_cycle >= phase_length(s, s->lcd_phase)) {
                ++s->lcd_phase;
                s->phase_cycle = 0;
            }
        }
    }
    if (s->lcd_running) { schedule(&s->lcd_period, s->lcd_timer); }
}
static void rx_drain(ESP32S3LcdCamState *s)
{
    unsigned channel;
    if (!s->rx_count || s->rx_fault ||
        !esp_gdma_get_channel_periph(s->gdma, GDMA_LCDCAM, ESP_GDMA_IN_IDX, &channel)) {
        return;
    }
    s->rx_channel = channel;
    unsigned take = MIN(s->rx_count, S3_LCDCAM_FIFO_BYTES - s->rx_head);
    unsigned limit = (C(s, CAM_CTRL1) & 65535) + 1;
    if (!(C(s, CAM_CTRL) & BIT(8))) { take = MIN(take, limit-s->cam_segment_bytes); }
    uint32_t served = 0;
    esp_gdma_write_channel_ex(s->gdma, channel, &s->rx_fifo[s->rx_head], take, &served);
    s->rx_head = (s->rx_head + served) % S3_LCDCAM_FIFO_BYTES;
    s->rx_count -= served;
    s->cam_segment_bytes += served;
    s->cam_frame_bytes += served;
    s->cam_bytes += served;
    if (!(C(s, CAM_CTRL) & BIT(8)) && s->cam_segment_bytes == limit) {
        if (!esp_gdma_finish_rx_segment(s->gdma, channel)) { s->rx_fault = true; }
        s->cam_segment_bytes = 0;
    }
}
static bool rx_byte(ESP32S3LcdCamState *s, uint8_t byte)
{
    rx_drain(s);
    if (s->rx_count == S3_LCDCAM_FIFO_BYTES) {
        ++s->rx_overflow;
        if (C(s, CAM_CTRL) & BIT(0)) {
            R(s, CAM_CTRL1) &= ~BIT(29);
            C(s, CAM_CTRL1) &= ~BIT(29);
        }
        s->rx_fault = true;
        return false;
    }
    unsigned tail = (s->rx_head + s->rx_count) % S3_LCDCAM_FIFO_BYTES;
    s->rx_fifo[tail] = byte;
    ++s->rx_count;
    return true;
}
static void cam_frame_end(ESP32S3LcdCamState *s)
{
    /* At most one wrap of the finite FIFO: bounded actual fulfilled drains. */
    rx_drain(s);
    rx_drain(s);
    bool complete = !s->rx_count && !s->rx_fault && !s->rx_swap_pending;
    if ((C(s, CAM_CTRL) & BIT(8)) && s->cam_segment_bytes) {
        /* A real VSYNC closes the actually fulfilled DMA packet even if
         * camera data was lost. GDMA EOF is not an image-validity flag.
         * The UHCI-only ERR_EOF entry point must never be used for ID5. */
        bool ended = esp_gdma_finish_rx_channel(s->gdma, s->rx_channel);
        complete &= ended;
    }
    if (complete && s->cam_frame_bytes) { ++s->cam_frames; }
    else if (!complete) { ++s->dropped_frames; }
    if (s->cam_frame_bytes) {
        s->last_cam_frame_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    }
    s->cam_segment_bytes = 0;
    s->cam_frame_bytes = 0;
    s->cam_line = 0;
    s->rx_swap_pending = false;
    if (!complete) { s->rx_head = s->rx_count = 0; }
}
static bool matrix_sample(ESP32S3LcdCamState *s, unsigned signal, bool *level)
{
    if (signal >= ESP32S3_GPIO_FUNC_IN_SEL_COUNT) { return false; }
    if (!(s->gpio->func_in_sel_cfg[signal] & BIT(7))) { return false; }
    unsigned pad = s->gpio->func_in_sel_cfg[signal] & 63;
    /* GPIO's strict helper consumes a persisted sample after scheduling its
     * UNKNOWN gate. Validate the committed electrical frame first so that
     * neither floating nor contended data can enter the camera FIFO. */
    if (pad < ESP32S3_GPIO_COUNT && !esp32s3_gpio_net_valid(s->gpio, pad)) {
        esp32s3_gpio_consume_unknown(s->gpio, pad);
        return false;
    }
    return esp32s3_gpio_matrix_sample(s->gpio, signal, level);
}
void esp32s3_lcd_cam_resolved_frame(void *opaque, uint64_t sample_ns)
{
    ESP32S3LcdCamState *s = opaque;
    (void)sample_ns;
    if (s->notifying || !available(s) || !(C(s, CAM_CTRL1) & BIT(29))) { return; }
    s->notifying = true;
    bool clock, vsync, href, hsync;
    bool valid = matrix_sample(s, SIG_CAM_CLK, &clock) &&
                 matrix_sample(s, SIG_VSYNC, &vsync) &&
                 matrix_sample(s, SIG_DE, &href);
    if ((C(s, CAM_CTRL1) & BIT(28))) {
        valid &= matrix_sample(s, SIG_HSYNC, &hsync);
    } else {
        hsync = true;
    }
    if (!valid) {
        if (s->cam_inputs_valid) { ++s->invalid_edges; }
        timer_del(s->cam_filter_timer);
        s->cam_filter_pending = false;
        if (s->cam_synchronized && (s->cam_frame_bytes || s->rx_count)) {
            /* Losing a wire is not a VSYNC edge. Do not manufacture EOF;
             * retain the fault until a real boundary or explicit reset. */
            s->rx_fault = true;
        }
        s->cam_inputs_valid = false;
        s->cam_synchronized = false;
        s->notifying = false;
        return;
    }
    clock ^= !!(C(s, CAM_CTRL1) & BIT(22));
    vsync ^= !!(C(s, CAM_CTRL1) & BIT(27));
    href ^= !!(C(s, CAM_CTRL1) & BIT(25));
    hsync ^= !!(C(s, CAM_CTRL1) & BIT(26));
    if (!s->cam_inputs_valid) {
        s->cam_last_clock = clock;
        s->cam_last_vsync = vsync;
        s->cam_last_href = href;
        s->cam_inputs_valid = true;
        /* A software arm during the already-active VSYNC pulse belongs to
         * that frame. Do not require a second synthetic VSYNC edge. */
        s->cam_synchronized = vsync;
        s->notifying = false;
        return;
    }
    if (C(s, CAM_CTRL1) & BIT(23)) {
        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        if (vsync == s->cam_last_vsync) {
            timer_del(s->cam_filter_timer);
            s->cam_filter_pending = false;
        } else {
            if (!s->cam_filter_pending || s->cam_filter_candidate != vsync) {
                unsigned cycles = ((C(s, CAM_CTRL) >> 1) & 7) + 1;
                if (!s->cam_period.denominator) {
                    s->notifying = false;
                    return;
                }
                uint64_t ns = s->cam_period.whole * 2 * cycles;
                uint64_t fraction = s->cam_period.remainder * 2 * cycles;
                ns += DIV_ROUND_UP(fraction, s->cam_period.denominator);
                s->cam_filter_candidate = vsync;
                s->cam_filter_pending = true;
                s->cam_filter_deadline = now + ns;
                timer_mod(s->cam_filter_timer, s->cam_filter_deadline);
            }
            if (now < s->cam_filter_deadline) {
                vsync = s->cam_last_vsync;
            } else {
                s->cam_filter_pending = false;
                timer_del(s->cam_filter_timer);
            }
        }
    }
    if (vsync && !s->cam_last_vsync) {
        if (s->cam_synchronized || s->cam_frame_bytes || s->rx_count || s->rx_swap_pending) {
            cam_frame_end(s);
        }
        s->cam_synchronized = true;
        event(s, BIT(2));
    }
    if (href && !s->cam_last_href) {
        ++s->cam_line;
        if ((C(s, CAM_CTRL) & BIT(7)) &&
            s->cam_line % (((C(s, CAM_CTRL1) >> 16) & 63) + 1) == 0) {
            event(s, BIT(3));
        }
    }
    if (clock && !s->cam_last_clock && href && !vsync && hsync && s->cam_synchronized && !s->rx_fault) {
        unsigned bits = C(s, CAM_CTRL1) & BIT(24) ? 16 : 8;
        uint16_t word = 0;
        for (unsigned i = 0; i < bits; ++i) {
            bool bit;
            if (!matrix_sample(s, SIG_DATA+i, &bit)) {
                ++s->invalid_edges;
                s->rx_fault = true;
                break;
            }
            word |= bit << i;
        }
        if (!s->rx_fault) {
            uint32_t order = (C(s, CAM_CTRL) & BIT(6) ? BIT(21) : 0) |
                             (C(s, CAM_CTRL) & BIT(5) ? BIT(22) : 0);
            word = ordered_word(word, bits, order);
            if (bits == 8 && (C(s, CAM_CONV) & BIT(21))) {
                if (!s->rx_swap_pending) {
                    s->rx_swap_byte = word;
                    s->rx_swap_pending = true;
                } else {
                    rx_byte(s, word);
                    rx_byte(s, s->rx_swap_byte);
                    s->rx_swap_pending = false;
                }
            } else {
                rx_byte(s, word);
                if (bits == 16) { rx_byte(s, word >> 8); }
            }
            rx_drain(s);
        }
    }
    s->cam_last_clock = clock;
    s->cam_last_vsync = vsync;
    s->cam_last_href = href;
    s->notifying = false;
}
static void cam_filter_expired(void *opaque)
{
    esp32s3_lcd_cam_resolved_frame(opaque, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
}
static void cam_clock_edge(void *opaque)
{
    ESP32S3LcdCamState *s = opaque;
    if (!available(s) || !source(s, C(s, CAM_CTRL))) { return; }
    s->cam_clock_level = !s->cam_clock_level;
    drive(s, SIG_CAM_CLK, true, s->cam_clock_level);
    schedule(&s->cam_period, s->cam_clock_timer);
}
static void lcd_start(ESP32S3LcdCamState *s)
{
    if (!available(s) || s->lcd_running) { return; }
    memcpy(s->lcd_active, s->reg, sizeof(s->lcd_active));
    s->lcd_pending_update = false;
    if (!period_config(s, &s->lcd_period, L(s, LCD_CLOCK), true)) { return; }
    period_config(s, &s->lcd_module_period, L(s, LCD_CLOCK), false);
    s->lcd_module_anchor_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    /* Establish the newly selected idle clock with CS deasserted. Changing
     * idle polarity must never fabricate a WR edge while CS is active. */
    lcd_idle(s);
    s->lcd_running = true;
    s->lcd_x = s->lcd_y = s->phase_cycle = s->data_cycle = 0;
    s->lcd_phase = PH_FRONT;
    /* Async FIFO state and prefetched EOF belong to GDMA/AFIFO, not START. */
    s->lcd_stalled = false;
    s->lcd_clock_level = !(L(s, LCD_CLOCK) & BIT(8));
    lcd_idle(s);
    /* The first callback sets data before the first clock edge. */
    s->lcd_clock_level = !(L(s, LCD_CLOCK) & BIT(8));
    schedule(&s->lcd_period, s->lcd_timer);
}
static void reconcile(ESP32S3LcdCamState *s)
{
    if (!available(s)) {
        timer_del(s->lcd_timer);
        timer_del(s->cam_clock_timer);
        timer_del(s->cam_filter_timer);
        s->cam_filter_pending = false;
        lcd_drives(s, false, false, false, false, false, false);
        drive(s, SIG_CAM_CLK, false, false);
        irq_update(s);
        return;
    }
    if ((R(s, LCD_USER) & BIT(27)) && !s->lcd_running) { lcd_start(s); }
    if (s->lcd_running && !timer_pending(s->lcd_timer) && !s->lcd_stalled &&
        period_config(s, &s->lcd_period, L(s, LCD_CLOCK), true)) {
        period_config(s, &s->lcd_module_period, L(s, LCD_CLOCK), false);
        s->lcd_module_anchor_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        schedule(&s->lcd_period, s->lcd_timer);
    }
    if (!timer_pending(s->cam_clock_timer)) {
        if (period_config(s, &s->cam_period, C(s, CAM_CTRL), false)) {
            schedule(&s->cam_period, s->cam_clock_timer);
        } else {
            drive(s, SIG_CAM_CLK, false, false);
        }
    }
    irq_update(s);
}
static uint64_t mmio_read(void *opaque, hwaddr address, unsigned size)
{
    ESP32S3LcdCamState *s = opaque;
    if (address >= sizeof(s->reg) || size != 4 || (address & 3)) { return 0; }
    switch (address) {
    case INT_ENA: return s->ena;
    case INT_RAW: return s->raw;
    case INT_ST: return s->raw & s->ena;
    case INT_CLR: return 0;
    default: return R(s, address);
    }
}
static void lcd_reset(ESP32S3LcdCamState *s)
{
    timer_del(s->lcd_timer);
    timer_del(s->lcd_delay_timer);
    s->lcd_drive_pending = 0;
    memset(s->lcd_drive_value, 0xff, sizeof(s->lcd_drive_value));
    memset(s->lcd_drive_mode, 0xff, sizeof(s->lcd_drive_mode));
    s->lcd_running = s->lcd_stalled = false;
    s->tx_head = s->tx_count = 0;
    s->tx_eof = s->tx_list_done = s->tx_fault = false;
    R(s, LCD_USER) &= ~(BIT(27)|BIT(28));
    memcpy(s->lcd_active, s->reg, sizeof(s->lcd_active));
    lcd_idle(s);
}
static void cam_reset(ESP32S3LcdCamState *s)
{
    s->rx_head = s->rx_count = s->cam_segment_bytes = s->cam_line = 0;
    s->cam_frame_bytes = 0;
    s->rx_fault = s->rx_swap_pending = s->cam_synchronized = s->cam_inputs_valid = false;
    R(s, CAM_CTRL1) &= ~(BIT(29)|BIT(30)|BIT(31));
    memcpy(s->cam_active, s->reg, sizeof(s->cam_active));
    timer_del(s->cam_filter_timer);
    s->cam_filter_pending = false;
}
static void mmio_write(void *opaque, hwaddr address, uint64_t value, unsigned size)
{
    ESP32S3LcdCamState *s = opaque;
    if (address >= sizeof(s->reg) || size != 4 || (address & 3) || s->reset_asserted) { return; }
    uint32_t old = R(s, address), val = value;
    switch (address) {
    case INT_ENA: s->ena = val & 15; irq_update(s); return;
    case INT_CLR: s->raw &= ~(val & 15); irq_update(s); return;
    case INT_RAW: case INT_ST: return;
    case LCD_USER:
        R(s, address) = val & ~BIT(20);
        if (val & BIT(28)) { lcd_reset(s); }
        else if (!(val & BIT(27)) && s->lcd_running) {
            /* Explicit stop is not a successful transfer completion. */
            timer_del(s->lcd_timer);
            s->lcd_running = false;
            lcd_idle(s);
        }
        if (val & BIT(20)) {
            if (s->lcd_running && (L(s, LCD_CTRL) & BIT(31))) { s->lcd_pending_update = true; }
            else { memcpy(s->lcd_active, s->reg, sizeof(s->lcd_active)); }
        }
        break;
    case LCD_MISC:
        R(s, address) = val & ~BIT(27);
        if (val & BIT(27)) {
            s->tx_head = s->tx_count = 0;
            s->tx_eof = s->tx_list_done = false;
        }
        break;
    case CAM_CTRL:
        R(s, address) = val & ~BIT(4);
        if (val & BIT(4)) {
            memcpy(s->cam_active, s->reg, sizeof(s->cam_active));
            timer_del(s->cam_clock_timer);
        }
        break;
    case CAM_CTRL1:
        R(s, address) = val & ~(BIT(30)|BIT(31));
        if (val & BIT(30)) { cam_reset(s); }
        if (val & BIT(31)) {
            s->rx_head = s->rx_count = 0;
            s->rx_swap_pending = s->rx_fault = false;
        }
        if ((val ^ old) & BIT(29)) {
            memcpy(s->cam_active, s->reg, sizeof(s->cam_active));
            s->cam_inputs_valid = s->cam_synchronized = false;
            s->cam_segment_bytes = s->cam_line = 0;
            s->cam_frame_bytes = 0;
        }
        break;
    default: R(s, address) = val; break;
    }
    reconcile(s);
    if (address == CAM_CTRL1 && (C(s, CAM_CTRL1) & BIT(29))) {
        esp32s3_lcd_cam_resolved_frame(s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    }
}
static const MemoryRegionOps mmio_ops = {
    .read = mmio_read, .write = mmio_write, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size=4, .max_access_size=4 },
    .impl = { .min_access_size=4, .max_access_size=4 },
};
static void device_reset(ESP32S3LcdCamState *s)
{
    timer_del(s->lcd_timer);
    timer_del(s->cam_clock_timer);
    memset(s->reg, 0, sizeof(s->reg));
    R(s, LCD_MISC) = 17 << 1;
    R(s, DATE) = 33566752;
    s->raw = s->ena = 0;
    lcd_reset(s);
    cam_reset(s);
    s->cam_clock_level = false;
    drive(s, SIG_CAM_CLK, false, false);
    irq_update(s);
}
static void reset_hold(Object *obj, ResetType type)
{
    ESP32S3LcdCamState *s = ESP32S3_LCD_CAM(obj);
    if (!s->soc_reset || esp32s3_reset_covers_periph(s->soc_reset)) {
        device_reset(s);
    }
}
static void gate_input(void *opaque, int n, int level)
{
    ESP32S3LcdCamState *s = opaque;
    s->gate = !!level;
    reconcile(s);
}
static void reset_input(void *opaque, int n, int level)
{
    ESP32S3LcdCamState *s = opaque;
    if (level && !s->reset_asserted) { device_reset(s); }
    s->reset_asserted = !!level;
    reconcile(s);
}
static void clock_changed(void *opaque, ClockEvent evt)
{
    ESP32S3LcdCamState *s = opaque;
    timer_del(s->lcd_timer);
    timer_del(s->cam_clock_timer);
    timer_del(s->lcd_delay_timer);
    s->lcd_drive_pending = 0;
    memset(s->lcd_drive_mode, 0xff, sizeof(s->lcd_drive_mode));
    reconcile(s);
}
static void gdma_armed(ESPGdmaState *gdma, GdmaPeripheral peripheral,
                       uint32_t channel, int direction, void *opaque)
{
    ESP32S3LcdCamState *s = opaque;
    if (!available(s)) { return; }
    if (direction == ESP_GDMA_IN_IDX) {
        s->rx_channel = channel;
        s->rx_fault = false;
        rx_drain(s);
    } else {
        s->tx_channel = channel;
        s->tx_fault = s->tx_eof = s->tx_list_done = false;
        /* The finite AFIFO accepts data with a live configured LCD clock,
         * before the control unit starts its wire sequence. Preserve any
         * prefetched EOF until those actual bytes have left the pins. */
        Clock *clk = source(s, L(s, LCD_CLOCK));
        if (clk && clock_get_hz(clk)) { tx_refill(s); }
        if (s->lcd_stalled && s->tx_count) {
            s->lcd_stalled = false;
            s->lcd_clock_level = !(L(s, LCD_CLOCK) & BIT(8));
        }
    }
    reconcile(s);
}
static void status_get(Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    ESP32S3LcdCamState *s = ESP32S3_LCD_CAM(obj);
    g_autofree char *json = g_strdup_printf(
        "{\"profile\":\"s3-lcdcam-edge-functional-v1\",\"lcd_running\":%s,"
        "\"lcd_words\":%" PRIu64 ",\"lcd_frames\":%" PRIu64 ","
        "\"cam_bytes\":%" PRIu64 ",\"cam_frames\":%" PRIu64 ","
        "\"tx_underflow\":%" PRIu64 ",\"rx_overflow\":%" PRIu64 ","
        "\"invalid_edges\":%" PRIu64 ",\"dropped_frames\":%" PRIu64 ","
        "\"last_lcd_eof_ns\":%" PRIu64 ",\"last_lcd_done_ns\":%" PRIu64 ","
        "\"last_cam_frame_ns\":%" PRIu64 ","
        "\"tx_fifo_bytes\":%u,\"rx_fifo_bytes\":%u,\"lcd_stalled\":%s}",
        s->lcd_running ? "true" : "false", s->lcd_words, s->lcd_frames,
        s->cam_bytes, s->cam_frames, s->tx_underflow, s->rx_overflow,
        s->invalid_edges, s->dropped_frames,
        s->last_lcd_eof_ns, s->last_lcd_done_ns, s->last_cam_frame_ns,
        s->tx_count, s->rx_count,
        s->lcd_stalled ? "true" : "false");
    visit_type_str(v, name, &json, errp);
}
static void realize(DeviceState *dev, Error **errp)
{
    ESP32S3LcdCamState *s = ESP32S3_LCD_CAM(dev);
    if (!s->gdma || !s->gpio || !s->electrical) {
        error_setg(errp, "LCD_CAM requires actual GDMA, GPIO and electrical links");
        return;
    }
    if (!esp32s3_electrical_subscribe(s->electrical, esp32s3_lcd_cam_resolved_frame, s)) {
        error_setg(errp, "LCD_CAM cannot subscribe to resolved electrical frames");
        return;
    }
    s->subscribed = true;
    esp_gdma_set_peripheral_notify(s->gdma, GDMA_LCDCAM, gdma_armed, s);
}
static void instance_init(Object *obj)
{
    ESP32S3LcdCamState *s = ESP32S3_LCD_CAM(obj);
    s->lcd_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, lcd_edge, s);
    s->cam_clock_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, cam_clock_edge, s);
    s->cam_filter_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, cam_filter_expired, s);
    s->lcd_delay_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, lcd_delay_commit, s);
    s->bus = qdev_init_clock_in(DEVICE(s), "bus", clock_changed, s, ClockUpdate);
    s->xtal = qdev_init_clock_in(DEVICE(s), "xtal", clock_changed, s, ClockUpdate);
    s->pll160 = qdev_init_clock_in(DEVICE(s), "pll160", clock_changed, s, ClockUpdate);
    s->pll240 = qdev_init_clock_in(DEVICE(s), "pll240", clock_changed, s, ClockUpdate);
    qdev_init_gpio_in_named(DEVICE(s), gate_input, "clk-gate", 1);
    qdev_init_gpio_in_named(DEVICE(s), reset_input, "reset", 1);
    memory_region_init_io(&s->iomem, obj, &mmio_ops, s, TYPE_ESP32S3_LCD_CAM, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->irq);
    object_property_add(obj, "status-json", "str", status_get, NULL, NULL, NULL);
}
static void instance_finalize(Object *obj)
{
    ESP32S3LcdCamState *s = ESP32S3_LCD_CAM(obj);
    if (s->subscribed) {
        esp32s3_electrical_unsubscribe(s->electrical, esp32s3_lcd_cam_resolved_frame, s);
    }
    timer_free(s->lcd_timer);
    timer_free(s->cam_clock_timer);
    timer_free(s->cam_filter_timer);
    timer_free(s->lcd_delay_timer);
}
static Property properties[] = {
    DEFINE_PROP_LINK("gdma", ESP32S3LcdCamState, gdma, TYPE_ESP_GDMA, ESPGdmaState *),
    DEFINE_PROP_LINK("gpio", ESP32S3LcdCamState, gpio, TYPE_ESP32S3_GPIO, ESP32S3GPIOState *),
    DEFINE_PROP_LINK("electrical", ESP32S3LcdCamState, electrical, TYPE_ESP32S3_ELECTRICAL, DeviceState *),
    DEFINE_PROP_LINK("soc-reset", ESP32S3LcdCamState, soc_reset, TYPE_DEVICE, DeviceState *),
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
    .name=TYPE_ESP32S3_LCD_CAM, .parent=TYPE_SYS_BUS_DEVICE,
    .instance_size=sizeof(ESP32S3LcdCamState), .instance_init=instance_init,
    .instance_finalize=instance_finalize, .class_init=class_init,
};
static void register_types(void) { type_register_static(&type_info); }
type_init(register_types)
