/* SPDX-License-Identifier: GPL-2.0-or-later */
/* OV2640 module-level functional DVP profile. Only resolved terminal frames
 * enable operation; byte payloads never cross a controller backdoor. Register
 * meanings: OmniVision OV2640 v2.2 tables 12/13, Espressif esp32-camera OV2640.
 * Compression: independently implemented T.81 baseline DCT/Huffman, not a
 * reconstruction of OmniVision's undocumented compression microcode/timing. */
#include "qemu/osdep.h"
#include "hw/misc/esp32s3_ov2640_service.h"
#include "hw/misc/esp32s3_electrical.h"
#include "hw/i2c/esp32s3_i2c_service.h"
#include "hw/i2c/esp32s3_i2c_binding.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qemu/log.h"
#include "qapi/qmp/qjson.h"
#include "qapi/qmp/qlist.h"
#include "qapi/qmp/qnum.h"
#include "sysemu/runstate.h"
#include <math.h>

#define TYPE_OV2640_SERVICE "esp32s3-ov2640-service"
#define SENSOR_LIMIT 32
#define JPEG_CAPACITY (32u * 1024u * 1024u)
#define PROFILE "ov2640-functional-dvp-v1"
#define FNV_OFFSET UINT64_C(14695981039346656037)
#define FNV_PRIME UINT64_C(1099511628211)

typedef struct OvService OvService;
typedef struct OvSensor {
    OvService *service;
    char id[65], identity[65];
    uint8_t reg[2][256], indirect[256];
    bool reg_known[2][256], indirect_known[256];
    uint8_t bank, bank_select, pointer, indirect_pointer;
    bool addressed, reading, pointer_pending, final_nack;
    bool testimage, epoch_valid, powered, reset_asserted, ready;
    bool clock_valid, clock_level, pclk, streaming, failed;
    uint8_t staged_byte;
    bool staged_href, staged_vsync, drive_valid, drive_oe;
    uint16_t drive_levels;
    uint64_t epoch, last_rise, xperiod, last_clock_edge;
    int64_t ready_at;
    uint64_t cycle, frames, bytes, frame_bytes, frame_hash, last_frame_hash;
    uint64_t half_period;
    unsigned width, height, line_bytes, lines;
    uint8_t image_mode, com10, ctrl0;
    uint8_t *jpeg;
    size_t jpeg_len, jpeg_capacity;
    uint8_t *capture_current, *capture_last;
    size_t capture_capacity, capture_length;
    uint64_t frame_start_ns, capture_start_ns, capture_end_ns, capture_frame;
    uint64_t capture_half_period, capture_hash;
    unsigned capture_line_bytes, capture_lines, capture_width, capture_height;
    uint8_t capture_mode, capture_ctrl0;
    uint64_t capture_epoch, capture_generation;
    unsigned capture_scene_width, capture_scene_height;
    unsigned capture_crop_x, capture_crop_y, capture_crop_width, capture_crop_height;
    bool capture_mirror, capture_flip;
    bool capture_testimage;
    unsigned scene_width, scene_height, crop_x, crop_y, crop_width, crop_height;
    bool payload_dirty;
    QEMUTimer *timer, *watchdog;
    const char *dependency;
} OvSensor;
struct OvService {
    Object parent;
    DeviceState *electrical;
    OvSensor slots[SENSOR_LIMIT];
    double basis[8][8];
    bool activating, closing;
    char request_id[65];
    uint64_t request_offset, request_count;
    uint64_t request_frame;
};

static const char *const outputs[] = {
    "d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7",
    "pclk", "href", "vsync"
};

static void fail(OvSensor *s, const char *reason)
{
    s->dependency = reason;
    if (!s->failed) {
        qemu_log_mask(LOG_GUEST_ERROR, "OV2640 '%s': %s\n", s->id, reason);
        s->failed = true;
        vm_stop(RUN_STATE_PAUSED);
    }
}

static bool sample(OvSensor *s, const char *role, uint64_t ns, bool *level)
{
    bool valid = false;
    double voltage;
    return esp32s3_electrical_terminal_sample(s->service->electrical, s->id,
        role, ns, &voltage, &valid, level) && valid;
}

static void drive(OvSensor *s, bool oe, uint8_t byte, bool pclk,
                  bool href, bool vsync)
{
    DeviceState *dev = s->service->electrical;
    uint16_t levels = byte | ((unsigned)pclk << 8) |
        ((unsigned)href << 9) | ((unsigned)vsync << 10);
    uint16_t changed = !s->drive_valid || s->drive_oe != oe ?
        0x7ff : s->drive_levels ^ levels;
    if (!changed) {
        return;
    }
    if (!esp32s3_electrical_begin_update(dev)) {
        fail(s, "atomic terminal update unavailable");
        return;
    }
    bool ok = true;
    for (unsigned i = 0; i < G_N_ELEMENTS(outputs); ++i) {
        if (changed & (1u << i)) {
            ok &= esp32s3_electrical_terminal_drive(dev, s->id, outputs[i],
                                                   oe, (levels >> i) & 1);
        }
    }
    s->drive_valid = true;
    s->drive_oe = oe;
    s->drive_levels = levels;
    ok &= esp32s3_electrical_end_update(dev);
    if (!ok) {
        fail(s, "registered finite DVP terminal drive unavailable");
    }
}

static void suspend(OvSensor *s, const char *reason)
{
    bool driven = s->streaming;
    s->streaming = false;
    s->cycle = 0;
    s->frame_bytes = 0;
    s->frame_hash = FNV_OFFSET;
    s->dependency = reason;
    timer_del(s->timer);
    if (driven) {
        drive(s, false, 0, false, false, false);
    }
}

static void reset_registers(OvSensor *s, int64_t ns)
{
    static const uint8_t dsp[][2] = {
        {0x05,1}, {0x44,12}, {0x51,0x40}, {0x52,0xf0},
        {0x55,0x08}, {0x5a,0x58}, {0x5b,0x48}, {0x86,0x0d}, {0x87,0x50},
        {0xc0,0x80}, {0xc1,0x60}, {0xc2,0x0c}, {0xc3,0xff},
        {0xd3,0x82}, {0xe0,4}, {0xed,0x1f}, {0xf0,4}, {0xf7,0x60},
        {0xf8,1}, {0xf9,0x40}, {0xfc,0x80},
    };
    static const uint8_t sensor[][2] = {
        {0x03,0x0f}, {0x04,0x20}, {0x08,0x40}, {0x0a,0x26},
        {0x0b,0x41}, {0x0c,0x38}, {0x10,0x33}, {0x13,0xc7},
        {0x14,0x50}, {0x17,0x11}, {0x18,0x75}, {0x19,1}, {0x1a,0x97},
        {0x1c,0x7f}, {0x1d,0xa2}, {0x24,0x78}, {0x25,0x68},
        {0x26,0xd4}, {0x32,0x36}, {0x34,0x20}, {0x4b,0x20},
        {0x4f,0xca}, {0x50,0xa8}, {0x61,0x80}, {0x62,0x90},
    };
    suspend(s, "reset settling");
    memset(s->reg, 0, sizeof(s->reg));
    memset(s->reg_known, 0, sizeof(s->reg_known));
    memset(s->indirect_known, 0, sizeof(s->indirect_known));
    static const uint8_t sensor_zero[] = {
        0x00,0x09,0x11,0x12,0x15,0x2a,0x2b,0x2d,0x2e,0x2f,
        0x45,0x46,0x47,0x48,0x49,0x4e,0x5d,0x5e,0x5f,0x60
    };
    static const uint8_t dsp_zero[] = {
        0x50,0x53,0x54,0x56,0x57,0x5c,0x7c,0x7d,0x8c,0xda,0xfa,0xfb,0xfd,0xfe
    };
    for (unsigned i = 0; i < sizeof(sensor_zero); ++i) {
        s->reg_known[1][sensor_zero[i]] = true;
    }
    for (unsigned i = 0; i < sizeof(dsp_zero); ++i) {
        s->reg_known[0][dsp_zero[i]] = true;
    }
    memset(s->indirect, 0, sizeof(s->indirect));
    s->indirect_known[0] = true; /* Public BPDATA reset default at BPADDR=0. */
    for (unsigned i = 0; i < G_N_ELEMENTS(dsp); ++i) {
        s->reg[0][dsp[i][0]] = dsp[i][1];
        s->reg_known[0][dsp[i][0]] = true;
    }
    for (unsigned i = 0; i < G_N_ELEMENTS(sensor); ++i) {
        s->reg[1][sensor[i][0]] = sensor[i][1];
        s->reg_known[1][sensor[i][0]] = true;
    }
    s->bank = 1;
    s->bank_select = 0x7f;
    s->pointer = s->indirect_pointer = 0;
    s->addressed = s->reading = s->pointer_pending = s->final_nack = false;
    s->payload_dirty = true;
    s->ready_at = ns + 10000000; /* Named profile: driver reset delay 10 ms. */
}

static void rgb(OvSensor *s, unsigned x, unsigned y, uint8_t out[3])
{
    if (s->reg[1][4] & 0x80) {
        x = s->width - 1 - x;
    }
    if (s->reg[1][4] & 0x40) {
        y = s->height - 1 - y;
    }
    x = MIN(s->scene_width - 1, s->crop_x +
            (uint64_t)x * s->crop_width / s->width);
    y = MIN(s->scene_height - 1, s->crop_y +
            (uint64_t)y * s->crop_height / s->height);
    if (!s->testimage || (s->reg[1][0x12] & 2)) {
        static const uint8_t bars[8][3] = {
            {255,255,255}, {255,255,0}, {0,255,255}, {0,255,0},
            {255,0,255}, {255,0,0}, {0,0,255}, {0,0,0}
        };
        memcpy(out, bars[MIN(7, 8 * x / s->scene_width)], 3);
    } else {
        out[0] = x * 255 / MAX(1, s->scene_width - 1);
        out[1] = y * 255 / MAX(1, s->scene_height - 1);
        out[2] = ((x / 8) ^ (y / 8)) & 1 ? 255 : 0;
    }
}

static void ycbcr(OvSensor *s, unsigned x, unsigned y, bool limited, int out[3])
{
    uint8_t c[3];
    rgb(s, x, y, c);
    int r = c[0], g = c[1], b = c[2];
    if (limited) {
        out[0] = 16 + ((66*r + 129*g + 25*b + 128) >> 8);
        out[1] = 128 + ((-38*r - 74*g + 112*b + 128) >> 8);
        out[2] = 128 + ((112*r - 94*g - 18*b + 128) >> 8);
    } else {
        out[0] = (19595*r + 38470*g + 7471*b + 32768) >> 16;
        out[1] = 128 + ((-11059*r - 21709*g + 32768*b + 32768) >> 16);
        out[2] = 128 + ((32768*r - 27439*g - 5329*b + 32768) >> 16);
    }
    for (unsigned i = 0; i < 3; ++i) {
        out[i] = CLAMP(out[i], 0, 255);
    }
}

/* A complete bounded baseline JPEG encoder: 8-bit YCbCr 4:4:4, separable
 * FDCT, uniform QS quantization, DC prediction, zigzag/RLE, canonical
 * Huffman tables, entropy byte stuffing and interoperable JFIF headers.
 * Non-optimal tables intentionally keep the implementation small/auditable.
 * Worst case <= 300 bytes/block * 90000 blocks + header < 32 MiB at UXGA. */
typedef struct JpegWriter {
    OvSensor *sensor;
    uint32_t bits;
    unsigned count;
    bool overflow;
} JpegWriter;
static void put8(JpegWriter *w, unsigned b)
{
    if (w->sensor->jpeg_len >= w->sensor->jpeg_capacity) {
        w->overflow = true;
    } else {
        w->sensor->jpeg[w->sensor->jpeg_len++] = b;
    }
}
static void put16(JpegWriter *w, unsigned v)
{
    put8(w, v >> 8);
    put8(w, v & 255);
}
static void marker(JpegWriter *w, unsigned m, unsigned len)
{
    put8(w, 255);
    put8(w, m);
    if (len) {
        put16(w, len);
    }
}
static void bits(JpegWriter *w, unsigned v, unsigned n)
{
    if (!n) {
        return;
    }
    w->bits = (w->bits << n) | (v & ((1u << n) - 1));
    w->count += n;
    while (w->count >= 8) {
        w->count -= 8;
        unsigned b = (w->bits >> w->count) & 255;
        put8(w, b);
        if (b == 255) {
            put8(w, 0);
        }
    }
}
static unsigned category(int v)
{
    unsigned a = abs(v), n = 0;
    while (a) {
        ++n;
        a >>= 1;
    }
    return n;
}
static void magnitude(JpegWriter *w, int value, unsigned n)
{
    bits(w, value < 0 ? value + (1 << n) - 1 : value, n);
}
static const uint8_t zigzag[64] = {
    0,1,8,16,9,2,3,10,17,24,32,25,18,11,4,5,
    12,19,26,33,40,48,41,34,27,20,13,6,7,14,21,28,
    35,42,49,56,57,50,43,36,29,22,15,23,30,37,44,51,
    58,59,52,45,38,31,39,46,53,60,61,54,47,55,62,63
};
static void jpeg_block(JpegWriter *w, const int samples[64],
                       unsigned quant, int *predictor)
{
    OvSensor *s = w->sensor;
    double horizontal[8][8];
    int coeff[64];
    for (unsigned row = 0; row < 8; ++row) {
        for (unsigned u = 0; u < 8; ++u) {
            double sum = 0;
            for (unsigned col = 0; col < 8; ++col) {
                sum += samples[row*8 + col] * s->service->basis[u][col];
            }
            horizontal[row][u] = sum;
        }
    }
    for (unsigned v = 0; v < 8; ++v) {
        for (unsigned u = 0; u < 8; ++u) {
            double sum = 0;
            for (unsigned row = 0; row < 8; ++row) {
                sum += horizontal[row][u] * s->service->basis[v][row];
            }
            coeff[v*8 + u] = lround(sum / quant);
        }
    }
    int diff = coeff[0] - *predictor;
    *predictor = coeff[0];
    unsigned n = category(diff);
    bits(w, n, 4); /* DC categories 0..11 have four-bit canonical codes. */
    magnitude(w, diff, n);
    unsigned run = 0;
    for (unsigned k = 1; k < 64; ++k) {
        int value = coeff[zigzag[k]];
        if (!value) {
            ++run;
            continue;
        }
        while (run >= 16) {
            bits(w, 1, 8); /* ZRL is second AC symbol. */
            run -= 16;
        }
        n = category(value);
        if (n > 10) {
            w->overflow = true;
            return;
        }
        bits(w, 2 + run*10 + n - 1, 8);
        magnitude(w, value, n);
        run = 0;
    }
    if (run) {
        bits(w, 0, 8); /* EOB. */
    }
}
static bool encode_jpeg(OvSensor *s)
{
    size_t capacity = (size_t)((s->width + 7) / 8) *
        ((s->height + 7) / 8) * 3 * 300 + 512;
    if (capacity > JPEG_CAPACITY) {
        fail(s, "JPEG dimension allocation bound exceeded");
        return false;
    }
    if (s->jpeg_capacity < capacity) {
        uint8_t *payload = g_try_realloc(s->jpeg, capacity);
        if (!payload) {
            fail(s, "bounded JPEG payload allocation unavailable");
            return false;
        }
        s->jpeg = payload;
        s->jpeg_capacity = capacity;
    }
    s->jpeg_len = 0;
    JpegWriter w = {.sensor = s};
    unsigned q = CLAMP(s->reg[0][0x44], 1, 63);
    marker(&w, 0xd8, 0);
    marker(&w, 0xe0, 16);
    static const uint8_t jfif[] = {'J','F','I','F',0,1,1,0,0,1,0,1,0,0};
    for (unsigned i = 0; i < sizeof(jfif); ++i) {
        put8(&w, jfif[i]);
    }
    marker(&w, 0xdb, 67);
    put8(&w, 0);
    for (unsigned i = 0; i < 64; ++i) {
        put8(&w, q);
    }
    marker(&w, 0xc0, 17);
    put8(&w, 8);
    put16(&w, s->height);
    put16(&w, s->width);
    put8(&w, 3);
    for (unsigned c = 1; c <= 3; ++c) {
        put8(&w, c);
        put8(&w, 0x11);
        put8(&w, 0);
    }
    marker(&w, 0xc4, 210); /* 2 + (17+12) + (17+162). */
    put8(&w, 0);
    for (unsigned n = 1; n <= 16; ++n) {
        put8(&w, n == 4 ? 12 : 0);
    }
    for (unsigned n = 0; n < 12; ++n) {
        put8(&w, n);
    }
    put8(&w, 0x10);
    for (unsigned n = 1; n <= 16; ++n) {
        put8(&w, n == 8 ? 162 : 0);
    }
    put8(&w, 0);
    put8(&w, 0xf0);
    for (unsigned run = 0; run < 16; ++run) {
        for (unsigned n = 1; n <= 10; ++n) {
            put8(&w, (run << 4) | n);
        }
    }
    marker(&w, 0xda, 12);
    put8(&w, 3);
    for (unsigned c = 1; c <= 3; ++c) {
        put8(&w, c);
        put8(&w, 0);
    }
    put8(&w, 0);
    put8(&w, 63);
    put8(&w, 0);
    int predictors[3] = {0};
    for (unsigned y = 0; y < s->height; y += 8) {
        for (unsigned x = 0; x < s->width; x += 8) {
            int samples[3][64];
            for (unsigned row = 0; row < 8; ++row) {
                for (unsigned col = 0; col < 8; ++col) {
                    int pixel[3];
                    ycbcr(s, MIN(x + col, s->width - 1),
                           MIN(y + row, s->height - 1), false, pixel);
                    for (unsigned c = 0; c < 3; ++c) {
                        samples[c][row*8 + col] = pixel[c] - 128;
                    }
                }
            }
            for (unsigned c = 0; c < 3; ++c) {
                jpeg_block(&w, samples[c], q, &predictors[c]);
            }
        }
    }
    if (w.count) {
        bits(&w, (1u << (8 - w.count)) - 1, 8 - w.count);
    }
    marker(&w, 0xd9, 0);
    if (w.overflow) {
        fail(s, "JPEG payload bound or baseline coefficient bound exceeded");
        return false;
    }
    return true;
}

static uint8_t raw_byte(OvSensor *s, unsigned column, unsigned row)
{
    if (s->image_mode & 0x40) {
        int c[3];
        ycbcr(s, column, row, true, c);
        return c[0];
    }
    if (s->image_mode & 8) {
        uint8_t c[3];
        rgb(s, column / 2, row, c);
        unsigned pixel = ((c[0] >> 3) << 11) | ((c[1] >> 2) << 5) | (c[2] >> 3);
        bool low = (column & 1) ^ (s->image_mode & 1);
        return low ? pixel & 255 : pixel >> 8;
    }
    unsigned position = (column & 3) ^ (s->image_mode & 1);
    unsigned x = (column / 4) * 2;
    int a[3], b[3];
    ycbcr(s, x, row, true, a);
    ycbcr(s, MIN(x + 1, s->width - 1), row, true, b);
    if (!(position & 1)) {
        return position ? b[0] : a[0];
    }
    unsigned chroma = ((position == 3) ^ !!(s->ctrl0 & 0x10)) ? 2 : 1;
    return (a[chroma] + b[chroma] + 1) / 2;
}

static bool prepare_frame(OvSensor *s)
{
    unsigned mode = s->reg[1][0x12] & 0x70;
    s->scene_width = mode == 0x20 ? 400 : mode == 0x40 ? 800 : 1600;
    s->scene_height = mode == 0x20 ? 296 : mode == 0x40 ? 600 : 1200;
    s->width = 4 * (s->reg[0][0x5a] | ((s->reg[0][0x5c] & 3) << 8));
    s->height = 4 * (s->reg[0][0x5b] | ((s->reg[0][0x5c] & 4) << 6));
    if (!s->width || !s->height || s->width > 1600 || s->height > 1200) {
        s->dependency = "output scaler dimensions not configured or outside UXGA";
        return false;
    }
    s->crop_width = 4 * (s->reg[0][0x51] |
        ((s->reg[0][0x55] & 8) << 5) | ((s->reg[0][0x57] & 0x80) << 2));
    s->crop_height = 4 * (s->reg[0][0x52] | ((s->reg[0][0x55] & 0x80) << 1));
    s->crop_x = s->reg[0][0x53] | ((s->reg[0][0x55] & 7) << 8);
    s->crop_y = s->reg[0][0x54] | ((s->reg[0][0x55] & 0x70) << 4);
    if (!s->crop_width || !s->crop_height ||
        s->crop_x + s->crop_width > s->scene_width ||
        s->crop_y + s->crop_height > s->scene_height) {
        s->dependency = "DSP input window outside selected sensor scene";
        return false;
    }
    s->image_mode = s->reg[0][0xda];
    s->com10 = s->reg[1][0x15];
    s->ctrl0 = s->reg[0][0xc2];
    if (!(s->image_mode & 0x10) && (s->image_mode & 4)) {
        fail(s, "RAW10/reserved output format unsupported by the eight-bit profile");
        return false;
    }
    unsigned divisor = MAX(1, s->reg[0][0xd3] & 0x7f);
    unsigned clock_div = (s->reg[1][0x11] & 0x3f) + 1;
    unsigned doubled = s->reg[1][0x11] & 0x80 ? 2 : 1;
    /* Profile explicitly extends documented D3 divisor to RGB/JPEG and
     * treats AUTO as the programmed lower-seven-bit divisor. No hidden
     * nominal XCLK: xperiod comes only from resolved terminal rising edges. */
    s->half_period = MAX(UINT64_C(1),
        (s->xperiod * clock_div * divisor + 2*doubled - 1) / (2*doubled));
    if ((s->image_mode & 0x10) && s->payload_dirty && !encode_jpeg(s)) {
        return false;
    }
    s->payload_dirty = false;
    size_t length;
    if (s->image_mode & 0x10) {
        length = s->jpeg_len;
        s->line_bytes = s->image_mode & 2 ? length : 2 * s->width;
        s->lines = (length + s->line_bytes - 1) / s->line_bytes;
    } else {
        s->line_bytes = s->width * (s->image_mode & 0x40 ? 1 : 2);
        s->lines = s->height;
        length = (size_t)s->line_bytes * s->lines;
    }
    if (length > JPEG_CAPACITY) {
        fail(s, "emitted frame bound exceeded");
        return false;
    }
    if (s->capture_capacity < length) {
        uint8_t *current = g_try_malloc(length), *last = g_try_malloc(length);
        if (!current || !last) {
            g_free(current);
            g_free(last);
            fail(s, "bounded immutable frame capture allocation unavailable");
            return false;
        }
        g_free(s->capture_current);
        g_free(s->capture_last);
        s->capture_current = current;
        s->capture_last = last;
        s->capture_capacity = length;
        s->capture_length = 0;
    }
    return true;
}

static bool active_pixel(OvSensor *s, uint8_t *byte)
{
    uint64_t span = s->line_bytes + 16;
    uint64_t line = s->cycle / span, column = s->cycle % span;
    if (line < 8 || line >= 8 + s->lines || column >= s->line_bytes) {
        return false;
    }
    unsigned row = line - 8;
    size_t offset = (size_t)row * s->line_bytes + column;
    if (s->image_mode & 0x10) {
        if (offset >= s->jpeg_len) {
            return false;
        }
        *byte = s->jpeg[offset];
    } else {
        *byte = raw_byte(s, column, row);
    }
    return true;
}

static void pixel_tick(void *opaque)
{
    OvSensor *s = opaque;
    uint64_t ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (!s->streaming || !s->ready || !s->xperiod ||
        ns - s->last_clock_edge > 4 * s->xperiod) {
        suspend(s, "resolved XCLK stopped or sensor not ready");
        return;
    }
    s->pclk = !s->pclk;
    bool latch = s->pclk != !!(s->com10 & 0x10);
    if (!latch) {
        s->staged_byte = 0;
        s->staged_href = active_pixel(s, &s->staged_byte);
        s->staged_vsync = s->cycle < (uint64_t)4 * (s->line_bytes + 16);
    }
    uint8_t byte = s->staged_byte;
    bool href = s->staged_href, vsync = s->staged_vsync;
    /* COM10[4] selects data-update edge; the following opposite edge latches
     * the stable byte. Batching prevents observers seeing an intermediate
     * bus word. At latch edges only PCLK changes (data was already settled). */
    bool gated = (s->com10 & 0x20) && !href;
    drive(s, true, byte, gated ? !!(s->com10 & 0x10) : s->pclk,
          href ^ !!(s->com10 & 8), vsync ^ !!(s->com10 & 2));
    if (!s->streaming || s->failed) {
        return;
    }
    if (latch) {
        if (!s->cycle) {
            s->frame_start_ns = ns;
        }
        if (href) {
            bool net_clock, net_href, net_vsync;
            if (!sample(s, "pclk", ns, &net_clock) || net_clock != s->pclk ||
                !sample(s, "href", ns, &net_href) ||
                net_href != (href ^ !!(s->com10 & 8)) ||
                !sample(s, "vsync", ns, &net_vsync) ||
                net_vsync != (vsync ^ !!(s->com10 & 2))) {
                fail(s, "resolved DVP clock/sync unknown or contended");
                suspend(s, "DVP output contention");
                return;
            }
            uint8_t resolved = 0;
            for (unsigned i = 0; i < 8; ++i) {
                bool level;
                if (!sample(s, outputs[i], ns, &level)) {
                    fail(s, "resolved DVP data terminal UNKNOWN");
                    suspend(s, "DVP data contention");
                    return;
                }
                resolved |= level << i;
            }
            byte = resolved;
            if (s->frame_bytes >= s->capture_capacity) {
                fail(s, "emitted capture capacity exceeded");
                suspend(s, "capture bound");
                return;
            }
            s->capture_current[s->frame_bytes++] = byte;
            s->frame_hash = (s->frame_hash ^ byte) * FNV_PRIME;
            ++s->bytes;
        }
        ++s->cycle;
        if (s->cycle == (uint64_t)(s->lines + 8) * (s->line_bytes + 16)) {
            ++s->frames;
            s->last_frame_hash = s->frame_hash;
            bool leased = s->service->request_frame == s->capture_frame &&
                s->capture_length && !strcmp(s->service->request_id, s->id);
            if (!leased) {
                uint8_t *swap = s->capture_last;
                s->capture_last = s->capture_current;
                s->capture_current = swap;
                s->capture_length = s->frame_bytes;
                s->capture_hash = s->frame_hash;
                s->capture_frame = s->frames;
                s->capture_start_ns = s->frame_start_ns;
                s->capture_end_ns = ns;
                s->capture_half_period = s->half_period;
                s->capture_line_bytes = s->line_bytes;
                s->capture_lines = s->lines;
                s->capture_width = s->width;
                s->capture_height = s->height;
                s->capture_mode = s->image_mode;
                s->capture_ctrl0 = s->ctrl0;
                ESP32S3ElectricalEndpoint endpoint;
                esp32s3_electrical_model_endpoint(s->service->electrical, s->id,
                                                  ns, &endpoint);
                s->capture_epoch = endpoint.power_epoch;
                s->capture_generation = endpoint.generation;
                s->capture_scene_width = s->scene_width;
                s->capture_scene_height = s->scene_height;
                s->capture_crop_x = s->crop_x;
                s->capture_crop_y = s->crop_y;
                s->capture_crop_width = s->crop_width;
                s->capture_crop_height = s->crop_height;
                s->capture_mirror = !!(s->reg[1][4] & 0x80);
                s->capture_flip = !!(s->reg[1][4] & 0x40);
                s->capture_testimage = s->testimage && !(s->reg[1][0x12] & 2);
            }
            s->cycle = s->frame_bytes = 0;
            s->frame_hash = FNV_OFFSET;
        }
    }
    timer_mod(s->timer, ns + s->half_period);
}

static void clock_watchdog(void *opaque)
{
    OvSensor *s = opaque;
    uint64_t ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (!s->xperiod || ns - s->last_clock_edge >= 4 * s->xperiod) {
        s->ready = false;
        s->clock_valid = false;
        s->last_rise = s->xperiod = 0;
        suspend(s, "resolved XCLK edge deadline expired");
    } else {
        timer_mod(s->watchdog, s->last_clock_edge + 4 * s->xperiod);
    }
}

static void observe_sensor(OvSensor *s, uint64_t ns)
{
    ESP32S3ElectricalEndpoint endpoint;
    ESP32S3ElectricalRoute route = esp32s3_electrical_model_endpoint(
        s->service->electrical, s->id, ns, &endpoint);
    s->ready = false;
    if (route == ESP32S3_ELECTRICAL_ROUTE_NONE) {
        suspend(s, "component removed");
        return;
    }
    if (!endpoint.power_known) {
        suspend(s, "registered rails UNKNOWN");
        s->clock_valid = false;
        s->last_rise = s->xperiod = 0;
        return; /* Never interpret UNKNOWN as a power-cycle. */
    }
    if (!s->epoch_valid || s->epoch != endpoint.power_epoch) {
        s->epoch_valid = true;
        s->epoch = endpoint.power_epoch;
        reset_registers(s, ns);
        s->clock_valid = false;
        s->last_rise = s->xperiod = 0;
    }
    s->powered = endpoint.powered;
    if (!s->powered) {
        suspend(s, "registered rails unpowered");
        s->clock_valid = false;
        s->last_rise = s->xperiod = 0;
        timer_del(s->watchdog);
        return;
    }
    bool reset, pwdn, clock;
    if (!sample(s, "reset", ns, &reset) || !sample(s, "pwdn", ns, &pwdn) ||
        !sample(s, "xclk", ns, &clock)) {
        suspend(s, "RESET/PWDN/XCLK resolved input UNKNOWN");
        s->clock_valid = false;
        s->last_rise = s->xperiod = 0;
        return;
    }
    if (!reset) {
        if (!s->reset_asserted) {
            reset_registers(s, ns);
        }
        s->reset_asserted = true;
        s->clock_valid = false;
        s->last_rise = s->xperiod = 0;
        return;
    }
    if (s->reset_asserted) {
        s->ready_at = ns + 10000000;
        s->reset_asserted = false;
    }
    if (pwdn) {
        suspend(s, "hardware PWDN high");
        s->clock_valid = false;
        s->last_rise = s->xperiod = 0;
        return;
    }
    if (s->clock_valid && s->clock_level != clock) {
        s->last_clock_edge = ns;
        if (clock) {
            if (s->last_rise && ns > s->last_rise) {
                uint64_t measured = ns - s->last_rise;
                if (!s->xperiod || measured + 1 < s->xperiod ||
                    measured > s->xperiod + 1) {
                    if (s->xperiod) {
                        suspend(s, "resolved XCLK period changed");
                    }
                    s->xperiod = measured;
                } /* One-nanosecond quantization jitter is not a new clock. */
                timer_mod(s->watchdog, ns + 4 * s->xperiod);
            }
            s->last_rise = ns;
        }
    }
    s->clock_level = clock;
    s->clock_valid = true;
    if (!s->xperiod || ns - s->last_clock_edge > 4 * s->xperiod) {
        suspend(s, "waiting for resolved XCLK rising-edge period");
        return;
    }
    if (ns < s->ready_at) {
        s->dependency = "power/reset profile settling";
        return;
    }
    s->ready = true; /* SCCB can operate while DSP/DVP is held in reset. */
    if ((s->reg[1][9] & 0x10) || (s->reg[0][0xe0] & 0x14) ||
        (s->reg[0][5] & 1)) {
        suspend(s, "software standby, DSP bypass or DVP/JPEG reset");
        return;
    }
    if (!s->streaming && !s->failed && prepare_frame(s)) {
        s->streaming = true;
        s->dependency = NULL;
        s->pclk = !(s->com10 & 0x10); /* First edge is data-update edge. */
        s->cycle = s->frame_bytes = 0;
        s->frame_hash = FNV_OFFSET;
        timer_mod(s->timer, ns + s->half_period);
    }
}

static void physical_frame(void *opaque, uint64_t ns)
{
    OvService *service = opaque;
    if (service->activating || service->closing) {
        return;
    }
    for (unsigned i = 0; i < SENSOR_LIMIT; ++i) {
        if (service->slots[i].id[0]) {
            observe_sensor(&service->slots[i], ns);
        }
    }
}

static void model_wake(void *opaque)
{
    physical_frame(opaque, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
}

static bool sccb_address(void *opaque, bool reading, bool restart, int64_t ns)
{
    OvSensor *s = opaque;
    observe_sensor(s, ns);
    if (!s->ready || s->failed) {
        return false;
    }
    s->addressed = true;
    s->reading = reading;
    s->pointer_pending = !reading;
    s->final_nack = false;
    return true;
}
static bool sccb_write(void *opaque, uint8_t value, int64_t ns)
{
    OvSensor *s = opaque;
    if (!s->ready || !s->addressed || s->reading) {
        return false;
    }
    if (s->pointer_pending) {
        s->pointer = value;
        s->pointer_pending = false;
        return true;
    }
    uint8_t reg = s->pointer;
    if (reg == 0xff) {
        s->bank = value & 1;
        s->bank_select = value;
    } else if (s->bank == 1 && reg == 0x12 && (value & 0x80)) {
        reset_registers(s, ns);
    } else if (!(s->bank == 1 &&
                 (reg == 0x0a || reg == 0x0b || reg == 0x1c || reg == 0x1d))) {
        s->reg[s->bank][reg] = value;
        s->reg_known[s->bank][reg] = true;
        if (!s->bank && reg == 0x7c) {
            s->indirect_pointer = value;
        } else if (!s->bank && reg == 0x7d) {
            s->indirect_known[s->indirect_pointer] = true;
            s->indirect[s->indirect_pointer++] = value;
        }
        /* Configuration changes invalidate a partial physical frame, not a
         * completed immutable capture. No allocation or JSON in SCCB path. */
        s->payload_dirty = true;
        suspend(s, "SCCB configuration changed");
    }
    /* Public SS_CTRL[5] controls automatic register address increment. */
    if (s->reg[0][0xf8] & 0x20) {
        ++s->pointer;
    }
    return true;
}
static bool sccb_read(void *opaque, uint8_t *value, int64_t ns)
{
    OvSensor *s = opaque;
    if (!s->ready || !s->addressed || !s->reading || s->final_nack) {
        return false;
    }
    if ((s->pointer != 0xff && !s->reg_known[s->bank][s->pointer]) ||
        (!s->bank && s->pointer == 0x7d &&
         !s->indirect_known[s->indirect_pointer])) {
        s->dependency = "unqualified factory-reserved register reset value";
        return false; /* Never manufacture a zero for a datasheet XX default. */
    }
    *value = s->pointer == 0xff ? s->bank_select :
        !s->bank && s->pointer == 0x7d ? s->indirect[s->indirect_pointer++] :
        s->reg[s->bank][s->pointer];
    if (s->reg[0][0xf8] & 0x20) {
        ++s->pointer;
    }
    return true;
}
static void sccb_read_ack(void *opaque, bool nack)
{
    ((OvSensor *)opaque)->final_nack = nack;
}
static void sccb_cancel(void *opaque)
{
    OvSensor *s = opaque;
    s->addressed = s->reading = s->pointer_pending = s->final_nack = false;
}
static void sccb_stop(void *opaque, int64_t ns)
{
    /* SCCB register pointer survives STOP: unmodified SCCB_Read uses a
     * separate pointer-write transaction followed by a read transaction. */
    sccb_cancel(opaque);
}
static void sccb_power_reset(void *opaque, int64_t ns)
{
    OvSensor *s = opaque;
    sccb_cancel(s);
    /* Generic endpoint power_epoch is authoritative. The controller hook
     * must not fabricate a second reset just because one master got lost. */
    observe_sensor(s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
}
static int64_t sccb_ready_ns(void *opaque)
{
    return ((const OvSensor *)opaque)->ready_at;
}

static const S3I2CServiceOps sccb_ops = {
    .address = sccb_address, .write = sccb_write, .read = sccb_read,
    .read_ack = sccb_read_ack, .stop = sccb_stop, .cancel = sccb_cancel,
    .power_reset = sccb_power_reset, .ready_ns = sccb_ready_ns,
};

static OvSensor *find_sensor(OvService *service, const char *id)
{
    for (unsigned i = 0; i < SENSOR_LIMIT; ++i) {
        if (service->slots[i].id[0] && !strcmp(service->slots[i].id, id)) {
            return &service->slots[i];
        }
    }
    return NULL;
}

static bool parse_config(const QDict *component, bool *testimage,
                         char **identity, Error **errp)
{
    const QDict *attributes = component ? qobject_to(QDict,
        qdict_get(component, "attributes")) : NULL;
    const QDict *config = attributes ? qobject_to(QDict,
        qdict_get(attributes, "native_camera")) : NULL;
    const char *source = config ? qdict_get_try_str(config, "source") : NULL;
    const char *profile = config ? qdict_get_try_str(config, "timing_profile") : NULL;
    if (!config || qdict_size(config) != 2 || !source || !profile ||
        (strcmp(source, "color-bars") && strcmp(source, "testimage")) ||
        strcmp(profile, PROFILE)) {
        error_setg(errp, "ov2640-dvp requires exactly attributes.native_camera "
                   "{source:color-bars|testimage,timing_profile:%s}; no file paths", PROFILE);
        return false;
    }
    *testimage = !strcmp(source, "testimage");
    char *canonical = g_strdup_printf("ov2640-dvp|%s|%s|scene-v1|jpeg-t81-444-v1",
                                     PROFILE, source);
    *identity = g_compute_checksum_for_string(G_CHECKSUM_SHA256, canonical, -1);
    g_free(canonical);
    return true;
}

static bool preflight(void *opaque, const QDict *component,
                      char **identity, Error **errp)
{
    bool testimage;
    return parse_config(component, &testimage, identity, errp);
}

static char *serialize(QObject *object)
{
    GString *json = qobject_to_json(object);
    qobject_unref(object);
    return g_string_free(json, false);
}

static const char *format_name(uint8_t mode)
{
    return mode & 0x10 ? "jpeg" : mode & 0x40 ? "y8" :
           mode & 8 ? "rgb565" : "yuv422";
}

static void put_uint(QDict *dict, const char *key, uint64_t value)
{
    qdict_put(dict, key, qnum_from_uint(value));
}

static QDict *status_dict(OvSensor *s)
{
    QDict *out = qdict_new();
    ESP32S3ElectricalEndpoint endpoint = {0};
    esp32s3_electrical_model_endpoint(s->service->electrical, s->id,
        qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), &endpoint);
    qdict_put_str(out, "component_id", s->id);
    qdict_put_str(out, "config_identity", s->identity);
    qdict_put_str(out, "source", s->testimage ? "testimage" : "color-bars");
    qdict_put_str(out, "timing_profile", PROFILE);
    qdict_put_bool(out, "power_known", endpoint.power_known);
    qdict_put_bool(out, "powered", endpoint.powered);
    put_uint(out, "power_epoch", endpoint.power_epoch);
    put_uint(out, "generation", endpoint.generation);
    qdict_put_bool(out, "sccb_ready", s->ready);
    qdict_put_bool(out, "streaming", s->streaming);
    qdict_put_bool(out, "failed", s->failed);
    if (s->dependency) {
        qdict_put_str(out, "dependency", s->dependency);
    } else {
        qdict_put_null(out, "dependency");
    }
    put_uint(out, "frames", s->frames);
    put_uint(out, "bytes", s->bytes);
    put_uint(out, "xclk_period_ns", s->xperiod);
    put_uint(out, "half_period_ns", s->half_period);
    put_uint(out, "width", s->width);
    put_uint(out, "height", s->height);
    put_uint(out, "jpeg_byte_count", s->jpeg_len);
    put_uint(out, "capture_frame", s->capture_frame);
    put_uint(out, "capture_byte_count", s->capture_length);
    qdict_put_str(out, "format", format_name(s->image_mode));
    char hash[17];
    snprintf(hash, sizeof(hash), "%016" PRIx64, s->last_frame_hash);
    qdict_put_str(out, "last_frame_hash_fnv1a64", hash);
    return out;
}

static char *get_status(Object *obj, Error **errp)
{
    OvService *service = (OvService *)obj;
    QList *list = qlist_new();
    for (unsigned i = 0; i < SENSOR_LIMIT; ++i) {
        if (service->slots[i].id[0]) {
            qlist_append(list, status_dict(&service->slots[i]));
        }
    }
    return serialize(QOBJECT(list));
}

static bool request_uint(const QDict *dict, const char *key,
                         uint64_t *value, Error **errp)
{
    QNum *number = qobject_to(QNum, qdict_get(dict, key));
    if (!number || !qnum_get_try_uint(number, value)) {
        error_setg(errp, "OV2640 request %s requires an unsigned integer", key);
        return false;
    }
    return true;
}

static void set_capture_request(Object *obj, const char *value, Error **errp)
{
    OvService *service = (OvService *)obj;
    QObject *parsed = qobject_from_json(value, errp);
    if (!parsed) {
        return;
    }
    QDict *request = qobject_to(QDict, parsed);
    uint64_t offset, count, frame = 0;
    const char *id = request ? qdict_get_try_str(request, "component_id") : NULL;
    if (!request || !id || !*id || strlen(id) > 64 ||
        qdict_size(request) != (qdict_haskey(request, "frame") ? 4 : 3) ||
        !request_uint(request, "offset", &offset, errp) ||
        !request_uint(request, "count", &count, errp) ||
        (qdict_haskey(request, "frame") &&
         !request_uint(request, "frame", &frame, errp))) {
        if (!errp || !*errp) {
            error_setg(errp, "OV2640 capture request requires component_id,offset,count "
                       "and optional frame; count 0..4096");
        }
        goto out;
    }
    OvSensor *s = find_sensor(service, id);
    if (!s || !s->capture_length || count > 4096 ||
        offset > s->capture_length || count > s->capture_length - offset ||
        (frame && frame != s->capture_frame)) {
        error_setg(errp, "OV2640 capture component/frame/window unavailable");
        goto out;
    }
    g_strlcpy(service->request_id, id, sizeof(service->request_id));
    service->request_offset = offset;
    service->request_count = count;
    service->request_frame = frame;
out:
    qobject_unref(parsed);
}

static char *get_capture(Object *obj, Error **errp)
{
    OvService *service = (OvService *)obj;
    OvSensor *s = find_sensor(service, service->request_id);
    if (!s || !s->capture_length ||
        service->request_offset > s->capture_length ||
        service->request_count > s->capture_length - service->request_offset ||
        (service->request_frame && service->request_frame != s->capture_frame)) {
        error_setg(errp, "OV2640 capture lease unavailable or changed; set capture-request-json");
        return NULL;
    }
    QDict *out = qdict_new();
    qdict_put_str(out, "component_id", s->id);
    qdict_put_str(out, "config_identity", s->identity);
    qdict_put_str(out, "format", format_name(s->capture_mode));
    qdict_put_str(out, "source", s->capture_testimage ? "testimage" : "color-bars");
    qdict_put_str(out, "timing_profile", PROFILE);
    put_uint(out, "frame", s->capture_frame);
    put_uint(out, "power_epoch", s->capture_epoch);
    put_uint(out, "generation", s->capture_generation);
    put_uint(out, "width", s->capture_width);
    put_uint(out, "height", s->capture_height);
    put_uint(out, "image_mode", s->capture_mode);
    put_uint(out, "ctrl0", s->capture_ctrl0);
    put_uint(out, "scene_width", s->capture_scene_width);
    put_uint(out, "scene_height", s->capture_scene_height);
    put_uint(out, "crop_x", s->capture_crop_x);
    put_uint(out, "crop_y", s->capture_crop_y);
    put_uint(out, "crop_width", s->capture_crop_width);
    put_uint(out, "crop_height", s->capture_crop_height);
    qdict_put_bool(out, "mirror", s->capture_mirror);
    qdict_put_bool(out, "flip", s->capture_flip);
    put_uint(out, "byte_count", s->capture_length);
    put_uint(out, "frame_start_ns", s->capture_start_ns);
    put_uint(out, "frame_end_ns", s->capture_end_ns);
    put_uint(out, "half_period_ns", s->capture_half_period);
    put_uint(out, "line_bytes", s->capture_line_bytes);
    put_uint(out, "line_count", s->capture_lines);
    put_uint(out, "offset", service->request_offset);
    put_uint(out, "count", service->request_count);
    char hash[17];
    snprintf(hash, sizeof(hash), "%016" PRIx64, s->capture_hash);
    qdict_put_str(out, "frame_hash_fnv1a64", hash);
    QList *bytes = qlist_new(), *times = qlist_new();
    size_t end = service->request_offset + service->request_count;
    for (size_t k = service->request_offset; k < end; ++k) {
        qlist_append(bytes, qnum_from_uint(s->capture_last[k]));
        uint64_t cycle = (8 + k / s->capture_line_bytes) *
            (s->capture_line_bytes + 16) + k % s->capture_line_bytes;
        qlist_append(times, qnum_from_uint(s->capture_start_ns +
                     cycle * 2 * s->capture_half_period));
    }
    qdict_put(out, "bytes", bytes);
    qdict_put(out, "timestamps_ns", times);
    return serialize(QOBJECT(out));
}

static void slot_clear(OvSensor *s)
{
    OvService *service = s->service;
    if (s->id[0] && !strcmp(service->request_id, s->id)) {
        service->request_id[0] = 0;
        service->request_frame = 0;
    }
    if (s->id[0]) {
        esp32s3_i2c_unregister_service(service->electrical, s->id, s);
        /* Apply may already have removed the component. Release attempts are
         * intentionally not a new runtime dependency during destruction. */
        for (unsigned i = 0; i < G_N_ELEMENTS(outputs); ++i) {
            esp32s3_electrical_terminal_drive(service->electrical, s->id,
                                               outputs[i], false, false);
        }
    }
    if (s->timer) {
        timer_free(s->timer);
    }
    if (s->watchdog) {
        timer_free(s->watchdog);
    }
    g_free(s->jpeg);
    g_free(s->capture_current);
    g_free(s->capture_last);
    memset(s, 0, sizeof(*s));
    s->service = service;
}

static void activate(void *opaque, DeviceState *electrical)
{
    OvService *service = opaque;
    uint64_t ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    service->activating = true;
    if (!esp32s3_electrical_begin_update(electrical)) {
        error_setg(&error_fatal, "OV2640 activation atomic electrical update unavailable");
    }
    for (unsigned i = 0; i < SENSOR_LIMIT; ++i) {
        OvSensor *s = &service->slots[i];
        if (!s->id[0]) {
            continue;
        }
        const char *identity = NULL;
        const QDict *component = esp32s3_electrical_model_component(
            electrical, s->id, &identity);
        ESP32S3ElectricalEndpoint endpoint;
        ESP32S3ElectricalRoute route = esp32s3_electrical_model_endpoint(
            electrical, s->id, ns, &endpoint);
        if (!component || !identity ||
            route == ESP32S3_ELECTRICAL_ROUTE_NONE ||
            endpoint.model != ESP32S3_ELECTRICAL_OV2640_DVP ||
            strcmp(identity, s->identity)) {
            slot_clear(s);
        } else {
            s->drive_valid = false;
            /* Existing live slot survives only exact ID + factory identity.
             * Reassert finite branch intent atomically after graph edits. */
            uint16_t levels = s->drive_levels;
            drive(s, s->drive_oe, levels & 255, !!(levels & 256),
                  !!(levels & 512), !!(levels & 1024));
        }
    }
    for (unsigned index = 0; index < SENSOR_LIMIT; ++index) {
        ESP32S3ElectricalEndpoint endpoint;
        if (esp32s3_electrical_model_endpoint_at(electrical,
                ESP32S3_ELECTRICAL_OV2640_DVP, index, ns, &endpoint) ==
                ESP32S3_ELECTRICAL_ROUTE_NONE) {
            break;
        }
        if (find_sensor(service, endpoint.component_id)) {
            continue;
        }
        OvSensor *s = NULL;
        for (unsigned i = 0; i < SENSOR_LIMIT; ++i) {
            if (!service->slots[i].id[0]) {
                s = &service->slots[i];
                break;
            }
        }
        if (!s) {
            error_setg(&error_fatal, "OV2640 registered sensor bound exceeded");
        }
        const char *key = NULL;
        const QDict *component = esp32s3_electrical_model_component(
            electrical, endpoint.component_id, &key);
        char *identity = NULL;
        Error *error = NULL;
        if (!parse_config(component, &s->testimage, &identity, &error) ||
            !key || strcmp(key, identity)) {
            error_setg(&error_fatal, "OV2640 committed configuration identity unavailable%s%s",
                       error ? ": " : "", error ? error_get_pretty(error) : "");
        }
        g_strlcpy(s->id, endpoint.component_id, sizeof(s->id));
        g_strlcpy(s->identity, identity, sizeof(s->identity));
        g_free(identity);
        s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, pixel_tick, s);
        s->watchdog = timer_new_ns(QEMU_CLOCK_VIRTUAL, clock_watchdog, s);
        reset_registers(s, ns);
        if (!esp32s3_i2c_register_service(electrical, s->id, &sccb_ops, s)) {
            error_setg(&error_fatal, "OV2640 actual component SCCB service registration unavailable");
        }
    }
    bool settled = esp32s3_electrical_end_update(electrical);
    service->activating = false;
    if (!settled) {
        error_setg(&error_fatal, "OV2640 committed electrical settlement unavailable");
    }
    /* New slots observe the FIRST published solved frame, never Apply's
     * uncommitted batch. No borrowed QDict pointer survives activation. */
}

static void instance_init(Object *obj)
{
    OvService *service = (OvService *)obj;
    for (unsigned i = 0; i < SENSOR_LIMIT; ++i) {
        service->slots[i].service = service;
    }
    for (unsigned u = 0; u < 8; ++u) {
        for (unsigned x = 0; x < 8; ++x) {
            service->basis[u][x] = u ?
                0.5 * cos((2*x + 1) * u * G_PI / 16) : sqrt(0.125);
        }
    }
    object_property_add_str(obj, "status-json", get_status, NULL);
    object_property_add_str(obj, "capture-request-json", NULL, set_capture_request);
    object_property_add_str(obj, "capture-json", get_capture, NULL);
}

static void instance_finalize(Object *obj)
{
    OvService *service = (OvService *)obj;
    service->closing = true;
    if (!service->electrical) {
        return;
    }
    esp32s3_electrical_unregister_factory(service->electrical,
        ESP32S3_ELECTRICAL_OV2640_DVP, service);
    esp32s3_electrical_unsubscribe(service->electrical, physical_frame, service);
    esp32s3_electrical_remove_model_notify(service->electrical, model_wake, service);
    esp32s3_electrical_begin_update(service->electrical);
    for (unsigned i = 0; i < SENSOR_LIMIT; ++i) {
        slot_clear(&service->slots[i]);
    }
    esp32s3_electrical_end_update(service->electrical);
    object_unref(OBJECT(service->electrical));
}

static const TypeInfo service_type = {
    .name = TYPE_OV2640_SERVICE,
    .parent = TYPE_OBJECT,
    .instance_size = sizeof(OvService),
    .instance_init = instance_init,
    .instance_finalize = instance_finalize,
};
static void register_types(void)
{
    type_register_static(&service_type);
}
type_init(register_types)

void esp32s3_ov2640_service_create(Object *soc, DeviceState *electrical)
{
    Object *obj = object_new(TYPE_OV2640_SERVICE);
    OvService *service = (OvService *)obj;
    service->electrical = electrical;
    object_ref(OBJECT(electrical));
    object_property_add_child(soc, "ov2640-sensors", obj);
    if (!esp32s3_electrical_subscribe(electrical, physical_frame, service) ||
        !esp32s3_electrical_add_model_notify(electrical, model_wake, service) ||
        !esp32s3_electrical_register_factory(electrical,
            ESP32S3_ELECTRICAL_OV2640_DVP, preflight, activate, service)) {
        error_setg(&error_fatal, "OV2640 registered factory/observer capacity unavailable");
    }
    object_unref(obj); /* SOC child owns lifetime; no native ABI shim. */
}
