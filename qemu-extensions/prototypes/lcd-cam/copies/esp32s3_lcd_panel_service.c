/* SPDX-License-Identifier: GPL-2.0-or-later */
/* External digital sinks only. No controller, DMA or guest-memory access.
 * Apply/query may allocate; solved-edge decoding uses preallocated storage. */
#include "qemu/osdep.h"
#include "hw/misc/esp32s3_lcd_panel_service.h"
#include "hw/misc/esp32s3_electrical.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "qapi/qmp/qbool.h"
#include "qapi/qmp/qdict.h"
#include "qapi/qmp/qjson.h"
#include "qapi/qmp/qlist.h"
#include "qapi/qmp/qnum.h"
#include "qapi/qmp/qstring.h"

#define TYPE_LCD_PANEL_SERVICE "esp32s3-lcd-panel-service"
#define PANEL_LIMIT 32
#define HISTORY 64

typedef enum PanelError {
    E_POWER, E_RESET, E_CLOCK, E_CS, E_DC, E_DATA,
    E_COMMAND, E_PARAMETER, E_FORMAT, E_WINDOW, E_TIMING,
    E_COUNT
} PanelError;
static const char *const error_names[E_COUNT] = {
    "unknown_power", "unknown_reset", "unknown_clock", "unknown_cs",
    "unknown_dc", "unknown_data", "unsupported_command",
    "parameter_count", "unsupported_pixel_format", "address_window",
    "rgb_timing"
};
static const char *const data_roles[] = {
    "d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7",
    "d8", "d9", "d10", "d11", "d12", "d13", "d14", "d15"
};
typedef struct PanelConfig {
    bool rgb, pclk, de, hs, vs;
    unsigned width, height, bus, bpp, pixel_size, active_cycles, hp, hb, hf, vp, vb, vf;
    char identity[65];
} PanelConfig;
typedef struct FrameRecord {
    uint64_t sequence, start_ns, end_ns, epoch, pixels, errors;
    uint64_t clocks, lines, period_min_ns, period_max_ns;
    uint64_t observed_width, observed_height;
    uint64_t observed_wire_width;
    unsigned bus_width, bits_per_pixel;
    uint64_t hsync_width, h_back_porch, h_front_porch;
    uint64_t vsync_width, v_back_porch, v_front_porch;
    uint8_t digest[32];
    bool valid, visible;
} FrameRecord;
typedef struct PanelService PanelService;
typedef struct Panel {
    PanelService *owner;
    char id[65], identity[65];
    PanelConfig c;
    ESP32S3ElectricalEndpoint power;
    bool epoch_seen, powered, clock_known, clock, reset_known, reset;
    bool sleep, display, invert, little, memory, xwindow_valid, ywindow_valid, ram_format_valid;
    uint8_t command, parameters[16], nparam, expected, madctl, colmod;
    uint8_t pixel_bytes[3], pixel_n;
    unsigned x0, x1, y0, y1, x, y;
    uint8_t *working, *last;
    size_t bytes;
    GChecksum *checksum;
    uint64_t commands, data, pixels, resets, captures, evicted, errors[E_COUNT];
    uint64_t observed_epoch, sample_ns, error_last_ns[E_COUNT];
    uint64_t error_total, condition_mask, frame_errors, frame_pixels, start_ns;
    uint64_t frame_clocks, frame_lines, line_clocks, line_active, line_pixels, line_hs;
    uint64_t first_de, observed_width, observed_wire_width, observed_height, vs_lines;
    uint64_t observed_hp, observed_hb, observed_hf, first_active_line, last_active_line;
    uint64_t last_edge_ns, period_min, period_max;
    bool synced, line_started, hs, vs, de_seen, line_de_ended;
    bool rgb_pixel_invalid;
    FrameRecord records[HISTORY];
} Panel;
struct PanelService {
    Object parent;
    DeviceState *electrical;
    Panel panels[PANEL_LIMIT];
    bool subscribed, registered_st, registered_rgb, closing;
    char request_id[65], fb_id[65];
    uint64_t offset, count, fb_offset, fb_count;
    uint8_t *fb_snapshot;
    size_t fb_bytes;
    unsigned fb_width, fb_height;
    FrameRecord fb_record, snapshot[HISTORY];
    uint64_t snapshot_count, snapshot_total, snapshot_evicted;
    char fb_identity[65], request_identity[65];
    bool requested, fb_requested;
};

static bool uint_field(const QDict *d, const char *key, unsigned min,
                       unsigned max, unsigned *out, Error **errp)
{
    uint64_t value;
    QNum *n = qobject_to(QNum, qdict_get(d, key));
    if (!n || !qnum_get_try_uint(n, &value) || value < min || value > max) {
        error_setg(errp, "LCD panel '%s' requires integer %u..%u", key, min, max);
        return false;
    }
    *out = value;
    return true;
}
static bool bool_field(const QDict *d, const char *key, bool *out, Error **errp)
{
    QBool *b = qobject_to(QBool, qdict_get(d, key));
    if (!b) {
        error_setg(errp, "LCD panel '%s' requires boolean", key);
        return false;
    }
    *out = qbool_get_bool(b);
    return true;
}
static bool parse(const QDict *component, PanelConfig *c, Error **errp)
{
    const QDict *attributes = component ?
        qobject_to(QDict, qdict_get(component, "attributes")) : NULL;
    const QDict *d = attributes ?
        qobject_to(QDict, qdict_get(attributes, "native_lcd_panel")) : NULL;
    const char *model = component ? qdict_get_try_str(component, "type") : NULL;
    static const char *const common[] = {"width", "height", "bus_width"};
    static const char *const rgb[] = {"bits_per_pixel", "pclk_active_high", "de_active_high",
        "hsync_active_high", "vsync_active_high", "hsync_pulse_width",
        "h_back_porch", "h_front_porch", "vsync_pulse_width", "v_back_porch", "v_front_porch"};
    if (!d || !model || (strcmp(model, "st7789-i80") && strcmp(model, "rgb-panel"))) {
        error_setg(errp, "LCD panel requires registered type and attributes.native_lcd_panel");
        return false;
    }
    *c = (PanelConfig){.rgb = !strcmp(model, "rgb-panel")};
    for (const QDictEntry *e = qdict_first(d); e; e = qdict_next(d, e)) {
        bool known = false;
        for (unsigned i = 0; i < G_N_ELEMENTS(common); ++i) {
            known |= !strcmp(qdict_entry_key(e), common[i]);
        }
        for (unsigned i = 0; c->rgb && i < G_N_ELEMENTS(rgb); ++i) {
            known |= !strcmp(qdict_entry_key(e), rgb[i]);
        }
        if (!known) {
            error_setg(errp, "Unknown LCD panel parameter '%s'", qdict_entry_key(e));
            return false;
        }
    }
    if (!uint_field(d, "width", 1, c->rgb ? 1024 : 240, &c->width, errp) ||
        !uint_field(d, "height", 1, c->rgb ? 1024 : 320, &c->height, errp) ||
        !uint_field(d, "bus_width", 8, 16, &c->bus, errp)) {
        return false;
    }
    if (c->bus != 8 && c->bus != 16) {
        error_setg(errp, "LCD panel bus_width must be 8 or 16");
        return false;
    }
    if (c->rgb &&
        (!uint_field(d, "bits_per_pixel", 8, 24, &c->bpp, errp) ||
         !bool_field(d, "pclk_active_high", &c->pclk, errp) ||
         !bool_field(d, "de_active_high", &c->de, errp) ||
         !bool_field(d, "hsync_active_high", &c->hs, errp) ||
         !bool_field(d, "vsync_active_high", &c->vs, errp) ||
         !uint_field(d, "hsync_pulse_width", 1, 4096, &c->hp, errp) ||
         !uint_field(d, "h_back_porch", 0, 4096, &c->hb, errp) ||
         !uint_field(d, "h_front_porch", 0, 4096, &c->hf, errp) ||
         !uint_field(d, "vsync_pulse_width", 1, 4096, &c->vp, errp) ||
         !uint_field(d, "v_back_porch", 0, 4096, &c->vb, errp) ||
         !uint_field(d, "v_front_porch", 0, 4096, &c->vf, errp))) {
        return false;
    }
    if (c->rgb) {
        if ((c->bpp != 8 && c->bpp != 16 && c->bpp != 24) ||
            c->bpp < c->bus || (c->width * c->bpp) % c->bus) {
            error_setg(errp, "RGB bits_per_pixel requires 8/16/24, at least the bus width and whole wire cycles per row");
            return false;
        }
        c->pixel_size = c->bpp / 8;
        c->active_cycles = c->width * c->bpp / c->bus;
    }
    /* Explicit fixed field order, independent of input JSON key order. */
    char *canonical = g_strdup_printf("lcd-panel-functional-v2:%u:%u:%u:%u:%u:%u:%u:%u:%u:%u:%u:%u:%u:%u:%u",
        c->rgb, c->width, c->height, c->bus, c->bpp, c->pclk, c->de, c->hs, c->vs,
        c->hp, c->hb, c->hf, c->vp, c->vb, c->vf);
    char *digest = g_compute_checksum_for_string(G_CHECKSUM_SHA256, canonical, -1);
    g_strlcpy(c->identity, digest, sizeof(c->identity));
    g_free(digest);
    g_free(canonical);
    return true;
}
static bool preflight(void *opaque, const QDict *component, char **identity, Error **errp)
{
    PanelConfig c;
    if (!parse(component, &c, errp)) {
        return false;
    }
    void *memory = g_try_malloc_n((size_t)c.width * c.height, 6);
    if (!memory) {
        error_setg(errp, "LCD panel bounded framebuffer allocation unavailable before Apply");
        return false;
    }
    g_free(memory);
    *identity = g_strdup(c.identity);
    return true;
}
static void fault(Panel *p, PanelError error)
{
    ++p->errors[error];
    ++p->error_total;
    ++p->frame_errors;
    p->error_last_ns[error] = p->sample_ns;
}
static void condition(Panel *p, PanelError error, bool bad)
{
    uint64_t bit = UINT64_C(1) << error;
    if (bad && !(p->condition_mask & bit)) {
        fault(p, error);
    }
    p->condition_mask = bad ? p->condition_mask | bit : p->condition_mask & ~bit;
}
static bool sample(Panel *p, const char *role, uint64_t ns, bool *level)
{
    bool valid = false;
    double voltage;
    return esp32s3_electrical_terminal_sample(p->owner->electrical, p->id,
        role, ns, &voltage, &valid, level) && valid;
}
static bool bus_sample(Panel *p, uint64_t ns, unsigned bits, uint32_t *word)
{
    *word = 0;
    for (unsigned i = 0; i < bits; ++i) {
        bool level;
        if (!sample(p, data_roles[i], ns, &level)) {
            fault(p, E_DATA);
            return false;
        }
        *word |= (uint32_t)level << i;
    }
    return true;
}
static void reset_state(Panel *p)
{
    ++p->resets;
    p->clock_known = false;
    p->sleep = true;
    p->display = p->invert = p->little = p->memory = false;
    p->xwindow_valid = p->ywindow_valid = p->ram_format_valid = true;
    p->command = p->nparam = p->expected = p->madctl = p->pixel_n = 0;
    p->colmod = 0x66;
    p->x0 = p->y0 = p->x = p->y = 0;
    p->x1 = p->c.width - 1;
    p->y1 = p->c.height - 1;
    p->synced = p->line_started = p->de_seen = p->line_de_ended = false;
    p->hs = p->vs = false;
    p->last_edge_ns = p->period_min = p->period_max = 0;
    p->frame_errors = p->frame_pixels = p->frame_clocks = p->frame_lines = 0;
    p->line_clocks = p->line_active = p->line_pixels = p->line_hs = p->vs_lines = 0;
    p->observed_width = p->observed_wire_width = p->observed_height = 0;
    p->rgb_pixel_invalid = false;
    if (p->c.rgb) { memset(p->working, 0, p->bytes); }
    /* ST7789 SWRESET and RESX reset the command/control state, not RAM.
     * Completed immutable captures and lifetime counters always survive. */
}
static void capture(Panel *p, uint64_t ns)
{
    uint64_t sequence = p->captures++;
    FrameRecord *r = &p->records[sequence % HISTORY];
    *r = (FrameRecord){.sequence = sequence, .start_ns = p->start_ns,
        .end_ns = ns, .epoch = p->power.power_epoch, .pixels = p->frame_pixels,
        .errors = p->frame_errors + !!p->condition_mask, .clocks = p->frame_clocks, .lines = p->frame_lines,
        .period_min_ns = p->period_min, .period_max_ns = p->period_max,
        .observed_width = p->observed_width, .observed_height = p->observed_height,
        .observed_wire_width = p->observed_wire_width,
        .bus_width = p->c.bus, .bits_per_pixel = p->c.rgb ? p->c.bpp : (p->colmod & 7) == 5 ? 16 : 18,
        .hsync_width = p->observed_hp, .h_back_porch = p->observed_hb, .h_front_porch = p->observed_hf,
        .vsync_width = p->vs_lines,
        .v_back_porch = p->observed_height && p->first_active_line >= p->vs_lines ?
            p->first_active_line - p->vs_lines : 0,
        .v_front_porch = p->observed_height && p->frame_lines > p->last_active_line ?
            p->frame_lines - p->last_active_line - 1 : 0,
        .valid = !p->frame_errors && !p->condition_mask,
        .visible = p->c.rgb || (!p->sleep && p->display)};
    g_checksum_reset(p->checksum);
    g_checksum_update(p->checksum, p->working, p->bytes);
    gsize digest_size = sizeof(r->digest);
    g_checksum_get_digest(p->checksum, r->digest, &digest_size);
    if (p->c.rgb) {
        uint8_t *previous = p->last;
        p->last = p->working;
        p->working = previous;
    } else {
        memcpy(p->last, p->working, p->bytes);
    }
    if (sequence >= HISTORY) {
        ++p->evicted;
    }
}
static void rgb565(uint16_t value, uint8_t *dst)
{
    unsigned r = (value >> 11) & 31, g = (value >> 5) & 63, b = value & 31;
    dst[0] = (r << 3) | (r >> 2);
    dst[1] = (g << 2) | (g >> 4);
    dst[2] = (b << 3) | (b >> 2);
}
static void st_pixel(Panel *p, const uint8_t color[3], uint64_t ns)
{
    unsigned x = p->x, y = p->y;
    if (p->madctl & 0x20) {
        unsigned t = x; x = y; y = t;
    }
    if (x >= p->c.width || y >= p->c.height) {
        fault(p, E_WINDOW);
        p->memory = false;
        return;
    }
    if (p->madctl & 0x40) { x = p->c.width - 1 - x; }
    if (p->madctl & 0x80) { y = p->c.height - 1 - y; }
    uint8_t *dst = p->working + ((size_t)y * p->c.width + x) * 3;
    dst[0] = color[(p->madctl & 8) ? 2 : 0];
    dst[1] = color[1];
    dst[2] = color[(p->madctl & 8) ? 0 : 2];
    ++p->pixels;
    ++p->frame_pixels;
    if (++p->x > p->x1) {
        p->x = p->x0;
        if (++p->y > p->y1) {
            p->y = p->y0;
            p->observed_width = p->x1 - p->x0 + 1;
            p->observed_height = p->y1 - p->y0 + 1;
            capture(p, ns);
            p->frame_pixels = p->frame_errors = 0;
            p->start_ns = ns;
        }
    }
}
static void st_parameters(Panel *p)
{
    switch (p->command) {
    case 0x2a: case 0x2b: {
        unsigned a = (p->parameters[0] << 8) | p->parameters[1];
        unsigned b = (p->parameters[2] << 8) | p->parameters[3];
        unsigned limit = p->command == 0x2a ?
            ((p->madctl & 0x20) ? p->c.height : p->c.width) :
            ((p->madctl & 0x20) ? p->c.width : p->c.height);
        bool valid = a <= b && b < limit;
        if (!valid) { fault(p, E_WINDOW); }
        if (p->command == 0x2a) {
            p->xwindow_valid = valid;
            if (valid) { p->x0 = a; p->x1 = b; }
        } else {
            p->ywindow_valid = valid;
            if (valid) { p->y0 = a; p->y1 = b; }
        }
        break;
    }
    case 0x36: p->madctl = p->parameters[0]; break;
    case 0x3a:
        p->colmod = p->parameters[0];
        if (p->colmod != 0x55 && p->colmod != 0x65 &&
            p->colmod != 0x56 && p->colmod != 0x66) { fault(p, E_FORMAT); }
        break;
    case 0xb0:
        p->little = (p->parameters[1] & 8) != 0;
        p->ram_format_valid = p->parameters[0] == 0 && !(p->parameters[1] & 7) &&
            (!p->little || p->c.bus == 8);
        if (!p->ram_format_valid) { fault(p, E_FORMAT); }
        break;
    default: break; /* Stored analog initialization parameters: no analog emulation. */
    }
}
static uint8_t command_length(uint8_t cmd)
{
    switch (cmd) {
    case 0x2a: case 0x2b: return 4;
    case 0x36: case 0x3a: case 0x26: case 0x35: case 0x51: case 0x53:
    case 0x55: case 0x5e: case 0xb7: case 0xba: case 0xbb: case 0xc0:
    case 0xc3: case 0xc4: case 0xc5: case 0xc6: return 1;
    case 0xb0: case 0xc2: case 0xd0: case 0x44: return 2;
    case 0xb1: case 0xb3: return 3;
    case 0xb2: return 5;
    case 0xe0: case 0xe1: return 14;
    case 0x00: case 0x01: case 0x10: case 0x11: case 0x12: case 0x13:
    case 0x20: case 0x21: case 0x28: case 0x29: case 0x2c: case 0x3c:
    case 0x34: case 0x38: case 0x39: return 0;
    default: return UINT8_MAX;
    }
}
static void st_word(Panel *p, bool dc, uint32_t word, uint64_t ns)
{
    if (!dc) {
        if ((p->expected != UINT8_MAX && p->nparam != p->expected) || p->pixel_n) {
            fault(p, E_PARAMETER);
        }
        ++p->commands;
        p->memory = false;
        p->pixel_n = p->nparam = 0;
        p->command = word & 0xff;
        p->expected = command_length(p->command);
        switch (p->command) {
        case 0x01: reset_state(p); return;
        case 0x10: p->sleep = true; break;
        case 0x11: p->sleep = false; break;
        case 0x28: p->display = false; break;
        case 0x29: p->display = true; break;
        case 0x20: p->invert = false; break;
        case 0x21: p->invert = true; break;
        case 0x2c: case 0x3c:
            p->memory = p->xwindow_valid && p->ywindow_valid && p->ram_format_valid;
            if (p->command == 0x2c) {
                p->x = p->x0; p->y = p->y0;
                p->frame_pixels = p->frame_errors = 0;
                p->start_ns = ns;
            }
            break;
        default:
            if (p->expected == UINT8_MAX) { fault(p, E_COMMAND); }
            break;
        }
        return;
    }
    ++p->data;
    if (!p->memory) {
        if (p->expected == UINT8_MAX || p->nparam >= p->expected) {
            fault(p, E_PARAMETER);
            return;
        }
        p->parameters[p->nparam++] = word & 0xff;
        if (p->nparam == p->expected) { st_parameters(p); }
        return;
    }
    uint8_t color[3];
    if ((p->colmod & 7) == 5) {
        if (p->c.bus == 16) {
            rgb565(word, color);
        } else {
            p->pixel_bytes[p->pixel_n++] = word;
            if (p->pixel_n != 2) { return; }
            uint16_t v = p->little ? (p->pixel_bytes[1] << 8) | p->pixel_bytes[0] :
                                    (p->pixel_bytes[0] << 8) | p->pixel_bytes[1];
            rgb565(v, color);
            p->pixel_n = 0;
        }
    } else if ((p->colmod & 7) == 6 && p->c.bus == 8) {
        p->pixel_bytes[p->pixel_n++] = word;
        if (p->pixel_n != 3) { return; }
        for (unsigned i = 0; i < 3; ++i) {
            color[i] = (p->pixel_bytes[i] & 0xfc) | (p->pixel_bytes[i] >> 6);
        }
        p->pixel_n = 0;
    } else {
        fault(p, E_FORMAT);
        return;
    }
    st_pixel(p, color, ns);
}
static void st_sample(Panel *p, uint64_t ns)
{
    bool clock, cs, dc;
    bool clock_valid = sample(p, "wr", ns, &clock);
    condition(p, E_CLOCK, !clock_valid);
    if (!clock_valid) { p->clock_known = false; p->pixel_n = 0; p->memory = false; return; }
    bool edge = p->clock_known && !p->clock && clock;
    p->clock_known = true; p->clock = clock;
    bool cs_valid = sample(p, "cs", ns, &cs);
    condition(p, E_CS, !cs_valid);
    if (!edge || !cs_valid || cs) { return; }
    bool dc_valid = sample(p, "dc", ns, &dc);
    condition(p, E_DC, !dc_valid);
    if (!dc_valid) { p->pixel_n = 0; p->memory = false; return; }
    uint32_t word;
    /* Commands/parameters use D7..D0 even on a sixteen-bit physical bus. */
    unsigned bits = dc && p->memory ? p->c.bus : 8;
    if (!bus_sample(p, ns, bits, &word)) { p->pixel_n = 0; p->memory = false; return; }
    st_word(p, dc, word, ns);
}
static void rgb_line_end(Panel *p)
{
    if (!p->line_started) { return; }
    bool active_line = p->frame_lines >= p->c.vp + p->c.vb &&
        p->frame_lines < p->c.vp + p->c.vb + p->c.height;
    if (p->line_clocks != p->c.hp + p->c.hb + p->c.active_cycles + p->c.hf ||
        p->line_hs != p->c.hp || p->pixel_n ||
        p->line_active != (active_line ? p->c.active_cycles : 0) ||
        p->line_pixels != (active_line ? p->c.width : 0) ||
        (active_line && p->first_de != p->c.hp + p->c.hb)) {
        fault(p, E_TIMING);
    }
    if (p->line_active) {
        if (!p->observed_height) { p->first_active_line = p->frame_lines; }
        p->last_active_line = p->frame_lines;
        p->observed_hp = p->line_hs;
        p->observed_hb = p->first_de >= p->line_hs ? p->first_de - p->line_hs : 0;
        p->observed_hf = p->line_clocks >= p->first_de + p->line_active ?
            p->line_clocks - p->first_de - p->line_active : 0;
        ++p->observed_height;
        p->observed_width = MAX(p->observed_width, p->line_pixels);
        p->observed_wire_width = MAX(p->observed_wire_width, p->line_active);
    }
    ++p->frame_lines;
}
static void rgb_byte(Panel *p, uint8_t byte, bool valid)
{
    p->pixel_bytes[p->pixel_n++] = byte;
    p->rgb_pixel_invalid |= !valid;
    if (p->pixel_n != p->c.pixel_size) { return; }
    uint64_t x = p->line_pixels++;
    uint64_t y = p->frame_lines - (p->c.vp + p->c.vb);
    ++p->frame_pixels;
    if (x >= p->c.width || y >= p->c.height) {
        fault(p, E_TIMING);
    } else if (!p->rgb_pixel_invalid) {
        uint8_t *dst = p->working + ((size_t)y * p->c.width + x) * 3;
        ++p->pixels;
        if (p->c.bpp == 16) {
            /* Explicit RGB565-LE stream profile, including an eight-bit
             * serial bus. Controller byte swaps remain actual wire swaps. */
            rgb565(p->pixel_bytes[0] | (p->pixel_bytes[1] << 8), dst);
        } else if (p->c.bpp == 24) {
            /* Declared RGB888 R,G,B stream, not a universal panel format. */
            dst[0] = p->pixel_bytes[0];
            dst[1] = p->pixel_bytes[1];
            dst[2] = p->pixel_bytes[2];
        } else {
            unsigned r = (byte >> 5) & 7, g = (byte >> 2) & 7, b = byte & 3;
            dst[0] = (r << 5) | (r << 2) | (r >> 1);
            dst[1] = (g << 5) | (g << 2) | (g >> 1);
            dst[2] = b * 85;
        }
    }
    p->pixel_n = 0;
    p->rgb_pixel_invalid = false;
}
static void rgb_sample(Panel *p, uint64_t ns)
{
    bool clock, de, hs, vs;
    bool valid = sample(p, "pclk", ns, &clock);
    condition(p, E_CLOCK, !valid);
    if (!valid) { p->clock_known = false; return; }
    bool edge = p->clock_known && p->clock != clock && clock == p->c.pclk;
    p->clock_known = true; p->clock = clock;
    if (!edge) { return; }
    bool de_valid = sample(p, "de", ns, &de);
    bool hs_valid = sample(p, "hsync", ns, &hs);
    bool vs_valid = sample(p, "vsync", ns, &vs);
    if (!de_valid || !hs_valid || !vs_valid) {
        fault(p, E_TIMING);
        return;
    }
    de = de == p->c.de; hs = hs == p->c.hs; vs = vs == p->c.vs;
    bool frame_start = vs && !p->vs;
    bool line_start = hs && !p->hs;
    if (frame_start) {
        if (p->synced) {
            rgb_line_end(p);
            if (p->frame_lines != p->c.vp + p->c.vb + p->c.height + p->c.vf ||
                p->vs_lines != p->c.vp ||
                p->observed_height != p->c.height ||
                p->frame_pixels != (uint64_t)p->c.width * p->c.height) {
                fault(p, E_TIMING);
            }
            capture(p, ns);
        }
        p->synced = true;
        p->line_started = false;
        p->frame_errors = p->frame_pixels = p->frame_clocks = p->frame_lines = 0;
        p->observed_width = p->observed_wire_width = p->observed_height = p->vs_lines = 0;
        p->observed_hp = p->observed_hb = p->observed_hf = 0;
        p->first_active_line = p->last_active_line = 0;
        p->period_min = p->period_max = p->last_edge_ns = 0;
        p->start_ns = ns;
        memset(p->working, 0, p->bytes);
        p->pixel_n = 0;
        p->rgb_pixel_invalid = false;
        if (!line_start) { fault(p, E_TIMING); }
    }
    if (p->synced && line_start) {
        rgb_line_end(p);
        p->line_started = true;
        p->line_clocks = p->line_active = p->line_pixels = p->line_hs = 0;
        p->de_seen = p->line_de_ended = false;
        p->pixel_n = 0;
        p->rgb_pixel_invalid = false;
        if (vs) { ++p->vs_lines; }
    }
    p->hs = hs; p->vs = vs;
    if (!p->synced || !p->line_started) { return; }
    if (p->frame_clocks) {
        uint64_t period = ns - p->last_edge_ns;
        if (!period) { fault(p, E_TIMING); }
        if (!p->period_min || period < p->period_min) { p->period_min = period; }
        p->period_max = MAX(p->period_max, period);
    }
    p->last_edge_ns = ns;
    ++p->frame_clocks;
    if (hs) { ++p->line_hs; }
    if (de) {
        if (!p->de_seen) { p->first_de = p->line_clocks; p->de_seen = true; }
        if (p->line_de_ended) { fault(p, E_TIMING); }
        ++p->line_active;
        uint32_t word = 0;
        bool known = bus_sample(p, ns, p->c.bus, &word);
        rgb_byte(p, word, known);
        if (p->c.bus == 16) { rgb_byte(p, word >> 8, known); }
    } else if (p->de_seen) { p->line_de_ended = true; }
    ++p->line_clocks;
}
static void physical_frame(void *opaque, uint64_t ns)
{
    PanelService *s = opaque;
    if (s->closing) { return; }
    for (unsigned i = 0; i < PANEL_LIMIT; ++i) {
        Panel *p = &s->panels[i];
        if (!p->id[0] || !p->working) { continue; }
        p->sample_ns = ns;
        ESP32S3ElectricalEndpoint e = {0};
        if (esp32s3_electrical_model_endpoint(s->electrical, p->id, ns, &e) ==
            ESP32S3_ELECTRICAL_ROUTE_NONE) { continue; }
        uint64_t previous_epoch = p->observed_epoch;
        p->power = e;
        condition(p, E_POWER, !e.power_known);
        if (!e.power_known) { p->clock_known = false; continue; }
        if (!p->epoch_seen || previous_epoch != e.power_epoch) {
            /* Power-on RAM is unspecified on silicon. This declared digital
             * profile starts black only at a new physical power epoch. */
            if (!p->c.rgb && p->epoch_seen) { memset(p->working, 0, p->bytes); }
            reset_state(p);
            p->epoch_seen = true;
            p->observed_epoch = e.power_epoch;
        }
        if (!e.powered) {
            if (p->powered) { reset_state(p); }
            p->powered = false;
            p->clock_known = false;
            continue;
        }
        p->powered = true;
        bool reset;
        bool reset_valid = sample(p, "reset", ns, &reset);
        condition(p, E_RESET, !reset_valid);
        if (!reset_valid) { p->clock_known = p->reset_known = false; continue; }
        if (!reset) {
            if (!p->reset_known || p->reset) { reset_state(p); }
            p->reset_known = true; p->reset = false;
            continue;
        }
        p->reset_known = true; p->reset = true;
        if (p->c.rgb) { rgb_sample(p, ns); }
        else { st_sample(p, ns); }
    }
}
static void clear_panel(Panel *p)
{
    PanelService *owner = p->owner;
    if (owner->requested && !strcmp(owner->request_id, p->id)) { owner->requested = false; }
    if (owner->fb_requested && !strcmp(owner->fb_id, p->id)) {
        owner->fb_requested = false;
        g_free(owner->fb_snapshot);
        owner->fb_snapshot = NULL;
    }
    if (p->checksum) { g_checksum_free(p->checksum); }
    g_free(p->working);
    g_free(p->last);
    memset(p, 0, sizeof(*p));
    p->owner = owner;
}
static Panel *find_panel(PanelService *s, const char *id)
{
    for (unsigned i = 0; i < PANEL_LIMIT; ++i) {
        if (s->panels[i].id[0] && !strcmp(s->panels[i].id, id)) { return &s->panels[i]; }
    }
    return NULL;
}
static void activate(void *opaque, DeviceState *electrical)
{
    PanelService *s = opaque;
    for (unsigned i = 0; i < PANEL_LIMIT; ++i) {
        Panel *p = &s->panels[i];
        const char *identity = NULL;
        const QDict *component = p->id[0] ?
            esp32s3_electrical_model_component(electrical, p->id, &identity) : NULL;
        if (p->id[0] && (!component || !identity || strcmp(p->identity, identity))) {
            clear_panel(p);
        }
    }
    const ESP32S3ElectricalModel models[] = {
        ESP32S3_ELECTRICAL_ST7789_I80, ESP32S3_ELECTRICAL_RGB_PANEL
    };
    uint64_t ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    for (unsigned m = 0; m < G_N_ELEMENTS(models); ++m) {
        for (unsigned index = 0; index < PANEL_LIMIT; ++index) {
            ESP32S3ElectricalEndpoint e;
            if (esp32s3_electrical_model_endpoint_at(electrical, models[m], index, ns, &e) ==
                ESP32S3_ELECTRICAL_ROUTE_NONE) { break; }
            if (find_panel(s, e.component_id)) { continue; }
            Panel *p = NULL;
            for (unsigned i = 0; i < PANEL_LIMIT; ++i) {
                if (!s->panels[i].id[0]) { p = &s->panels[i]; break; }
            }
            if (!p) { error_setg(&error_fatal, "LCD panel registered model capacity exceeded"); }
            const char *identity = NULL;
            const QDict *component = esp32s3_electrical_model_component(electrical,
                e.component_id, &identity);
            PanelConfig c;
            Error *error = NULL;
            if (!parse(component, &c, &error) || !identity || strcmp(identity, c.identity)) {
                if (error) { error_propagate(&error_fatal, error); }
                error_setg(&error_fatal, "LCD panel committed identity mismatch");
            }
            p->c = c;
            g_strlcpy(p->id, e.component_id, sizeof(p->id));
            g_strlcpy(p->identity, identity, sizeof(p->identity));
            p->bytes = (size_t)c.width * c.height * 3;
            p->working = g_malloc0(p->bytes);
            p->last = g_malloc0(p->bytes);
            p->checksum = g_checksum_new(G_CHECKSUM_SHA256);
            reset_state(p);
        }
    }
}
static void put_u64(QDict *d, const char *name, uint64_t value)
{
    qdict_put(d, name, qnum_from_uint(value));
}
static char *serialize(QObject *object)
{
    GString *json = qobject_to_json(object);
    qobject_unref(object);
    return g_string_free(json, false);
}
static void digest_hex(const uint8_t digest[32], char out[65])
{
    static const char digits[] = "0123456789abcdef";
    for (unsigned i = 0; i < 32; ++i) {
        out[2 * i] = digits[digest[i] >> 4]; out[2 * i + 1] = digits[digest[i] & 15];
    }
    out[64] = 0;
}
static QDict *record_dict(const FrameRecord *r)
{
    QDict *d = qdict_new();
    char hash[65]; digest_hex(r->digest, hash);
    put_u64(d, "sequence", r->sequence); put_u64(d, "start_ns", r->start_ns);
    put_u64(d, "end_ns", r->end_ns); put_u64(d, "power_epoch", r->epoch);
    put_u64(d, "pixels", r->pixels); put_u64(d, "errors", r->errors);
    put_u64(d, "pclk_samples", r->clocks); put_u64(d, "lines", r->lines);
    put_u64(d, "period_min_ns", r->period_min_ns); put_u64(d, "period_max_ns", r->period_max_ns);
    put_u64(d, "observed_width", r->observed_width); put_u64(d, "observed_height", r->observed_height);
    put_u64(d, "observed_wire_width", r->observed_wire_width);
    put_u64(d, "bus_width", r->bus_width); put_u64(d, "bits_per_pixel", r->bits_per_pixel);
    put_u64(d, "observed_hsync_width", r->hsync_width); put_u64(d, "observed_h_back_porch", r->h_back_porch);
    put_u64(d, "observed_h_front_porch", r->h_front_porch); put_u64(d, "observed_vsync_width", r->vsync_width);
    put_u64(d, "observed_v_back_porch", r->v_back_porch); put_u64(d, "observed_v_front_porch", r->v_front_porch);
    qdict_put_bool(d, "valid", r->valid); qdict_put_bool(d, "visible", r->visible);
    qdict_put_str(d, "sha256_rgb888", hash);
    return d;
}
static QDict *status(Panel *p)
{
    QDict *d = qdict_new(), *errors = qdict_new(), *timing = qdict_new();
    ESP32S3ElectricalEndpoint e = {0};
    esp32s3_electrical_model_endpoint(p->owner->electrical, p->id,
        qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), &e);
    qdict_put_str(d, "component_id", p->id); qdict_put_str(d, "config_identity", p->identity);
    qdict_put_str(d, "model", p->c.rgb ? "rgb-panel" : "st7789-i80");
    qdict_put_bool(d, "power_known", e.power_known); qdict_put_bool(d, "powered", e.powered);
    put_u64(d, "power_epoch", e.power_epoch); put_u64(d, "generation", e.generation);
    put_u64(d, "power_on_ns", e.power_on_ns);
    if (e.power_known) {
        qdict_put(d, "vdd_v", qnum_from_double(e.vdd_v));
        qdict_put(d, "gnd_v", qnum_from_double(e.gnd_v));
    } else { qdict_put_null(d, "vdd_v"); qdict_put_null(d, "gnd_v"); }
    put_u64(d, "width", p->c.width); put_u64(d, "height", p->c.height); put_u64(d, "bus_width", p->c.bus);
    put_u64(d, "bits_per_pixel", p->c.rgb ? p->c.bpp : (p->colmod & 7) == 5 ? 16 : 18);
    qdict_put_str(d, "pixel_stream_profile", p->c.rgb ?
        (p->c.bpp == 8 ? "rgb332" : p->c.bpp == 16 ? "rgb565-le" : "rgb888-r-g-b") :
        "st7789-register-selected");
    put_u64(d, "commands", p->commands); put_u64(d, "data_samples", p->data);
    put_u64(d, "pixels", p->pixels); put_u64(d, "resets", p->resets); put_u64(d, "captures", p->captures);
    put_u64(d, "first_retained_sequence", p->evicted); put_u64(d, "evicted_count", p->evicted);
    put_u64(d, "error_total", p->error_total); put_u64(d, "active_unknown_mask", p->condition_mask);
    for (unsigned i = 0; i < E_COUNT; ++i) { put_u64(errors, error_names[i], p->errors[i]); }
    qdict_put(d, "errors", errors);
    QDict *error_ns = qdict_new();
    for (unsigned i = 0; i < E_COUNT; ++i) {
        if (p->errors[i]) { put_u64(error_ns, error_names[i], p->error_last_ns[i]); }
        else { qdict_put_null(error_ns, error_names[i]); }
    }
    qdict_put(d, "error_last_ns", error_ns);
    qdict_put_bool(d, "sleep", p->sleep); qdict_put_bool(d, "display_on", p->display);
    qdict_put_bool(d, "inversion", p->invert); put_u64(d, "madctl", p->madctl); put_u64(d, "colmod", p->colmod);
    qdict_put_bool(timing, "pclk_active_high", p->c.pclk); qdict_put_bool(timing, "de_active_high", p->c.de);
    qdict_put_bool(timing, "hsync_active_high", p->c.hs); qdict_put_bool(timing, "vsync_active_high", p->c.vs);
    put_u64(timing, "hsync_pulse_width", p->c.hp); put_u64(timing, "h_back_porch", p->c.hb);
    put_u64(timing, "h_front_porch", p->c.hf); put_u64(timing, "vsync_pulse_width", p->c.vp);
    put_u64(timing, "v_back_porch", p->c.vb); put_u64(timing, "v_front_porch", p->c.vf);
    qdict_put(d, "configured_timing", timing);
    return d;
}
static char *get_status(Object *obj, Error **errp)
{
    PanelService *s = (PanelService *)obj;
    QDict *d = qdict_new(); QList *panels = qlist_new();
    qdict_put_int(d, "version", 1);
    for (unsigned i = 0; i < PANEL_LIMIT; ++i) {
        if (s->panels[i].id[0]) { qlist_append(panels, status(&s->panels[i])); }
    }
    qdict_put(d, "panels", panels);
    return serialize(QOBJECT(d));
}
static bool request(PanelService *s, const char *value, bool framebuffer, Error **errp)
{
    QObject *object = qobject_from_json(value, errp);
    QDict *d = qobject_to(QDict, object);
    uint64_t offset, count, sequence = 0;
    const char *id = d ? qdict_get_try_str(d, "componentId") : NULL;
    QNum *o = d ? qobject_to(QNum, qdict_get(d, "offset")) : NULL;
    QNum *n = d ? qobject_to(QNum, qdict_get(d, "count")) : NULL;
    QNum *seq = d ? qobject_to(QNum, qdict_get(d, "sequence")) : NULL;
    if (!object) { return false; }
    Panel *p = id ? find_panel(s, id) : NULL;
    bool frozen = framebuffer && p && s->fb_requested &&
        !strcmp(s->fb_id, id) && !strcmp(s->fb_identity, p->identity) &&
        seq && qnum_get_try_uint(seq, &sequence) && sequence == s->fb_record.sequence;
    if (!d || qdict_size(d) != (framebuffer ? 4 : 3) || !p || !o || !n ||
        !qnum_get_try_uint(o, &offset) || !qnum_get_try_uint(n, &count) ||
        !count || count > (framebuffer ? 65536 : HISTORY) ||
        (framebuffer ? (!seq || !qnum_get_try_uint(seq, &sequence) ||
                       !p->captures || offset > p->bytes ||
                       (!frozen && sequence != p->captures - 1)) :
                       (offset < p->evicted || offset > p->captures))) {
        error_setg(errp, "LCD request requires registered componentId, retained offset/count and framebuffer sequence");
        qobject_unref(object);
        return false;
    }
    if (framebuffer) {
        if (!frozen) {
            uint8_t *snapshot = g_try_malloc(p->bytes);
            if (!snapshot) {
                error_setg(errp, "LCD immutable framebuffer snapshot allocation unavailable");
                qobject_unref(object);
                return false;
            }
            memcpy(snapshot, p->last, p->bytes);
            g_free(s->fb_snapshot);
            s->fb_snapshot = snapshot;
            s->fb_bytes = p->bytes; s->fb_width = p->c.width; s->fb_height = p->c.height;
            s->fb_record = p->records[sequence % HISTORY];
            g_strlcpy(s->fb_identity, p->identity, sizeof(s->fb_identity));
        }
        g_strlcpy(s->fb_id, id, sizeof(s->fb_id));
        s->fb_offset = offset; s->fb_count = count; s->fb_requested = true;
    } else {
        s->snapshot_count = MIN(count, p->captures - offset);
        for (unsigned i = 0; i < s->snapshot_count; ++i) {
            s->snapshot[i] = p->records[(offset + i) % HISTORY];
        }
        s->snapshot_total = p->captures; s->snapshot_evicted = p->evicted;
        g_strlcpy(s->request_identity, p->identity, sizeof(s->request_identity));
        g_strlcpy(s->request_id, id, sizeof(s->request_id));
        s->offset = offset; s->count = count; s->requested = true;
    }
    qobject_unref(object);
    return true;
}
static void set_capture(Object *obj, const char *value, Error **errp)
{
    request((PanelService *)obj, value, false, errp);
}
static void set_framebuffer(Object *obj, const char *value, Error **errp)
{
    request((PanelService *)obj, value, true, errp);
}
static char *get_capture(Object *obj, Error **errp)
{
    PanelService *s = (PanelService *)obj;
    Panel *p = s->requested ? find_panel(s, s->request_id) : NULL;
    if (!p || strcmp(p->identity, s->request_identity)) {
        error_setg(errp, "LCD capture request absent, removed or replaced"); return NULL;
    }
    QDict *d = qdict_new(); QList *frames = qlist_new();
    for (unsigned i = 0; i < s->snapshot_count; ++i) { qlist_append(frames, record_dict(&s->snapshot[i])); }
    put_u64(d, "total_at_request", s->snapshot_total);
    put_u64(d, "first_retained_sequence_at_request", s->snapshot_evicted);
    qdict_put_int(d, "version", 1); qdict_put_str(d, "kind", "lcd-panel-capture");
    qdict_put(d, "status", status(p)); qdict_put(d, "frames", frames); put_u64(d, "offset", s->offset);
    return serialize(QOBJECT(d));
}
static char *get_framebuffer(Object *obj, Error **errp)
{
    PanelService *s = (PanelService *)obj;
    Panel *p = s->fb_requested ? find_panel(s, s->fb_id) : NULL;
    if (!p || strcmp(p->identity, s->fb_identity)) {
        error_setg(errp, "LCD framebuffer request absent, removed or replaced"); return NULL;
    }
    size_t count = MIN(s->fb_count, s->fb_bytes - s->fb_offset);
    char *hex = g_malloc(count * 2 + 1);
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < count; ++i) {
        uint8_t b = s->fb_snapshot[s->fb_offset + i]; hex[2 * i] = digits[b >> 4]; hex[2 * i + 1] = digits[b & 15];
    }
    hex[count * 2] = 0;
    QDict *d = record_dict(&s->fb_record);
    qdict_put_int(d, "version", 1); qdict_put_str(d, "kind", "lcd-panel-framebuffer");
    qdict_put_str(d, "component_id", p->id); qdict_put_str(d, "config_identity", p->identity);
    qdict_put_str(d, "format", "rgb888"); qdict_put_str(d, "hex", hex);
    put_u64(d, "width", s->fb_width); put_u64(d, "height", s->fb_height);
    put_u64(d, "offset", s->fb_offset); put_u64(d, "count", count); put_u64(d, "total_bytes", s->fb_bytes);
    g_free(hex);
    return serialize(QOBJECT(d));
}
static void instance_init(Object *obj)
{
    PanelService *s = (PanelService *)obj;
    for (unsigned i = 0; i < PANEL_LIMIT; ++i) { s->panels[i].owner = s; }
    object_property_add_str(obj, "status-json", get_status, NULL);
    object_property_add_str(obj, "capture-request-json", NULL, set_capture);
    object_property_add_str(obj, "capture-json", get_capture, NULL);
    object_property_add_str(obj, "framebuffer-request-json", NULL, set_framebuffer);
    object_property_add_str(obj, "framebuffer-json", get_framebuffer, NULL);
}
static void instance_finalize(Object *obj)
{
    PanelService *s = (PanelService *)obj;
    s->closing = true;
    if (!s->electrical) { return; }
    if (s->registered_st) { esp32s3_electrical_unregister_factory(s->electrical, ESP32S3_ELECTRICAL_ST7789_I80, s); }
    if (s->registered_rgb) { esp32s3_electrical_unregister_factory(s->electrical, ESP32S3_ELECTRICAL_RGB_PANEL, s); }
    if (s->subscribed) { esp32s3_electrical_unsubscribe(s->electrical, physical_frame, s); }
    for (unsigned i = 0; i < PANEL_LIMIT; ++i) { clear_panel(&s->panels[i]); }
    g_free(s->fb_snapshot);
    object_unref(OBJECT(s->electrical));
}
static const TypeInfo service_type = {
    .name = TYPE_LCD_PANEL_SERVICE, .parent = TYPE_OBJECT,
    .instance_size = sizeof(PanelService), .instance_init = instance_init,
    .instance_finalize = instance_finalize,
};
static void register_types(void) { type_register_static(&service_type); }
type_init(register_types)
void esp32s3_lcd_panel_service_create(Object *soc, DeviceState *electrical)
{
    Object *obj = object_new(TYPE_LCD_PANEL_SERVICE);
    PanelService *s = (PanelService *)obj;
    s->electrical = electrical;
    object_ref(OBJECT(electrical));
    object_property_add_child(soc, "lcd-panels", obj);
    s->subscribed = esp32s3_electrical_subscribe(electrical, physical_frame, s);
    s->registered_st = esp32s3_electrical_register_factory(electrical,
        ESP32S3_ELECTRICAL_ST7789_I80, preflight, activate, s);
    s->registered_rgb = esp32s3_electrical_register_factory(electrical,
        ESP32S3_ELECTRICAL_RGB_PANEL, preflight, activate, s);
    if (!s->subscribed || !s->registered_st || !s->registered_rgb) {
        error_setg(&error_fatal, "LCD panel factory/solved-frame observer unavailable");
    }
    object_unref(obj);
}
