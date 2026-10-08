/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Native register-boundary qualification, NOT measured hardware evidence.
 * LCD sinks are powered st7789-i80/rgb-panel graph services. Camera vectors
 * are explicitly named MCU GPIO-output -> physical net -> MCU matrix-input
 * loop fixtures, NOT OV2640/SCCB qualification. No injected input, IRQ,
 * framebuffer, sensor response, capture file or controller-private access.
 * Register oracle: ESP-IDF S3 lcd_cam_struct.h/lcd_ll.h/cam_ll.h; independent
 * RGB565 expansion, wiring permutations, porch arithmetic and SHA256 below.
 * All loops and virtual-time advances are finite (largest payload 5120B).
 */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qemu/bitops.h"
#include "qapi/error.h"
#include "qapi/qmp/qjson.h"
#include "qapi/qmp/qlist.h"
#include "qapi/qmp/qnum.h"
#include "qapi/qmp/qstring.h"
#include "hw/gpio/esp32s3_gpio.h"
#include "hw/gpio/esp32s3_iomux.h"

#define BASE 0x60041000ULL
#define GPIO(r) (0x60004000ULL + (r))
#define PAD(n) (0x60009000ULL + IO_MUX_GPIOn_REG_OFFSET(n))
#define OUT(n) GPIO(GPIO_FUNC_OUT_SEL_CFG_OFFSET(n))
#define IN(n) GPIO(GPIO_FUNC_IN_SEL_CFG_OFFSET(n))
#define INPUT (R_IO_MUX_GPIOn_FUN_IE_MASK | \
               (ESP32S3_IOMUX_MCU_SEL_GPIO << R_IO_MUX_GPIOn_MCU_SEL_SHIFT))
#define EN1 0x600c001cULL
#define RST1 0x600c0024ULL
#define GATE BIT(8)
#define DMA(ch, out, r) (0x6003f000ULL + (ch) * 0xc0 + (out) * 0x60 + (r))
#define SRC 0x3fc90000U
#define DST 0x3fc94000U
#define TD 0x3fc98000U
#define RD 0x3fc98400U
#define CLOCK 0x00
#define CAM_CTRL 0x04
#define CAM_CTRL1 0x08
#define CAM_CONV 0x0c
#define USER 0x14
#define MISC 0x18
#define CTRL 0x1c
#define CTRL1 0x20
#define CTRL2 0x24
#define CMD 0x28
#define ENA 0x64
#define RAW 0x68
#define ST 0x6c
#define CLR 0x70
#define UPDATE BIT(20)
#define START BIT(27)
#define RGB BIT(31)
#define DATA BIT(24)
#define COMMAND BIT(26)
#define BUS16 BIT(23)
#define BITREV BIT(21)
#define BYTESWAP16 BIT(22)
#define BYTESWAP8 BIT(19)
#define DESC(size, len, eof) ((size) | ((len) << 12) | BIT(31) | ((eof) ? BIT(30) : 0))
#define CONTROLLER "/machine/soc/lcd_cam"
#define PANELS "/machine/soc/lcd-panels"
#define ELECTRICAL "/machine/soc/electrical"
/* XTAL40MHz / 40 => 1MHz, first edge rising, idle low, no prescaler. */
#define CLK1 (BIT(29) | (40U << 9) | BIT(6) | BIT(8))

typedef struct Fixture {
    QTestState *q;
    int64_t now;
    uint32_t gpio_output;
} Fixture;
typedef struct Order {
    unsigned bits;
    bool reverse, swap;
} Order;

static uint32_t rd(Fixture *f, unsigned r)
{
    return qtest_readl(f->q, BASE + r);
}
static void wr(Fixture *f, unsigned r, uint32_t v)
{
    qtest_writel(f->q, BASE + r, v);
}
static void step(Fixture *f, int64_t ns)
{
    g_assert_cmpint(ns, >=, 0);
    f->now = qtest_clock_step(f->q, ns);
}
static QDict *object(const char *json)
{
    QObject *o = qobject_from_json(json, &error_abort);
    g_assert_nonnull(qobject_to(QDict, o));
    return qobject_to(QDict, o);
}
static QDict *get_json(Fixture *f, const char *path, const char *property)
{
    QDict *reply = qtest_qmp(f->q, "{'execute':'qom-get','arguments':"
        "{'path':%s,'property':%s}}", path, property);
    g_assert_false(qdict_haskey(reply, "error"));
    QDict *result = object(qdict_get_str(reply, "return"));
    qobject_unref(reply);
    return result;
}
static void set_json(Fixture *f, const char *path, const char *property, QDict *d)
{
    GString *json = qobject_to_json(QOBJECT(d));
    QDict *reply = qtest_qmp(f->q, "{'execute':'qom-set','arguments':"
        "{'path':%s,'property':%s,'value':%s}}", path, property, json->str);
    g_assert_false(qdict_haskey(reply, "error"));
    qobject_unref(reply);
    g_string_free(json, true);
}
static QDict *component(QDict *p, const char *id, const char *kind)
{
    QDict *c = qdict_new();
    qdict_put_str(c, "id", id);
    qdict_put_str(c, "name", id);
    qdict_put_str(c, "kind", kind);
    qdict_put_str(c, "type", kind);
    qdict_put(c, "parameters", qdict_new());
    qdict_put(c, "terminals", qlist_new());
    qlist_append(qdict_get_qlist(p, "components"), c);
    return c;
}
static void terminal(QDict *c, const char *id, const char *role,
                     const char *domain, int gpio)
{
    QDict *t = qdict_new();
    qdict_put_str(t, "id", id);
    qdict_put_str(t, "name", id);
    qdict_put_str(t, "role", role);
    qdict_put_str(t, "domain", domain);
    qdict_put_str(t, "direction", "unspecified");
    if (gpio >= 0) {
        qdict_put_int(t, "gpio", gpio);
    }
    qlist_append(qdict_get_qlist(c, "terminals"), t);
}
static void quantity(QDict *c, const char *key, double value, const char *unit)
{
    QDict *v = qdict_new();
    qdict_put(v, "value", qnum_from_double(value));
    qdict_put_str(v, "unit", unit);
    qdict_put(qdict_get_qdict(c, "parameters"), key, v);
}
static QList *net(QDict *p, const char *id)
{
    QDict *n = qdict_new();
    QList *ends = qlist_new();
    qdict_put_str(n, "id", id);
    qdict_put_str(n, "name", id);
    qdict_put(n, "endpoints", ends);
    qlist_append(qdict_get_qlist(p, "nets"), n);
    return ends;
}
static QDict *project(const char *name, unsigned pads, QList **vdd, QList **gnd)
{
    QDict *p = object("{\"version\":3,\"id\":\"lcdcam-register-boundary\","
        "\"name\":\"LCD CAM register-boundary\","
        "\"profile\":{\"chip\":\"esp32s3\",\"board\":\"esp32-s3-devkitc-1\","
        "\"module\":\"esp32-s3-wroom-1\"},\"firmware\":{},"
        "\"runtime\":{\"electrical\":{\"driver_profile\":\"s3-explicit-finite-v1\",\"mode\":\"dc\"}},"
        "\"geometry\":{\"components\":{},\"nets\":{}},\"components\":[],\"nets\":[]}");
    qdict_put_str(p, "id", name);
    qdict_put_str(p, "name", name);
    QDict *c = component(p, "U", "mcu");
    terminal(c, "U.vdd", "vdd", "power", -1);
    terminal(c, "U.gnd", "gnd", "ground", -1);
    for (unsigned i = 0; i < pads; ++i) {
        g_autofree char *id = g_strdup_printf("U.io%u", i);
        g_autofree char *role = g_strdup_printf("io%u", i);
        terminal(c, id, role, "digital", i);
    }
    c = component(p, "V", "voltage-source");
    terminal(c, "V.p", "p", "power", -1);
    terminal(c, "V.n", "n", "ground", -1);
    quantity(c, "voltage", 3.3, "V");
    c = component(p, "G", "ground");
    terminal(c, "G.ref", "ref", "ground", -1);
    *vdd = net(p, "vdd");
    qlist_append_str(*vdd, "V.p");
    qlist_append_str(*vdd, "U.vdd");
    *gnd = net(p, "gnd");
    qlist_append_str(*gnd, "V.n");
    qlist_append_str(*gnd, "G.ref");
    qlist_append_str(*gnd, "U.gnd");
    return p;
}
static void pull(QDict *p, unsigned index, QList *wire, QList *rail)
{
    g_autofree char *id = g_strdup_printf("R%u", index);
    g_autofree char *a = g_strdup_printf("R%u.a", index);
    g_autofree char *b = g_strdup_printf("R%u.b", index);
    QDict *c = component(p, id, "resistor");
    terminal(c, a, "a", "passive", -1);
    terminal(c, b, "b", "passive", -1);
    quantity(c, "resistance", 10000, "ohm");
    qlist_append_str(wire, a);
    qlist_append_str(rail, b);
}
/* Fault: 1=missing WR, 2=WR attached to DC, 3=missing data0, 4=absent peer. */
static void panel_graph(Fixture *f, bool rgb, unsigned bits,
                        unsigned width, unsigned height, bool inverted, int fault)
{
    QList *vdd, *gnd;
    QDict *p = project(rgb ? "native-rgb-panel" : "native-i80-panel", 27, &vdd, &gnd);
    QDict *c = component(p, "D", rgb ? "rgb-panel" : "st7789-i80");
    qdict_put_str(c, "kind", "device");
    QDict *attrs = qdict_new(), *cfg = qdict_new();
    qdict_put_str(attrs, "native_model", rgb ? "rgb-panel" : "st7789-i80");
    qdict_put_int(cfg, "width", width);
    qdict_put_int(cfg, "height", height);
    qdict_put_int(cfg, "bus_width", bits);
    if (rgb) {
        qdict_put_int(cfg, "bits_per_pixel", bits);
        qdict_put_bool(cfg, "pclk_active_high", !inverted);
        qdict_put_bool(cfg, "de_active_high", !inverted);
        qdict_put_bool(cfg, "hsync_active_high", !inverted);
        qdict_put_bool(cfg, "vsync_active_high", !inverted);
        qdict_put_int(cfg, "hsync_pulse_width", 2);
        qdict_put_int(cfg, "h_back_porch", 2);
        qdict_put_int(cfg, "h_front_porch", 2);
        qdict_put_int(cfg, "vsync_pulse_width", 1);
        qdict_put_int(cfg, "v_back_porch", 1);
        qdict_put_int(cfg, "v_front_porch", 1);
    }
    qdict_put(attrs, "native_lcd_panel", cfg);
    qdict_put(c, "attributes", attrs);
    terminal(c, "D.vdd", "vdd", "power", -1);
    terminal(c, "D.gnd", "gnd", "ground", -1);
    qlist_append_str(vdd, "D.vdd");
    qlist_append_str(gnd, "D.gnd");
    for (unsigned i = 0; i < bits + (rgb ? 5 : 4); ++i) {
        const char *control[] = {"wr", "dc", "cs", "reset"};
        const char *rgb_control[] = {"pclk", "de", "hsync", "vsync", "reset"};
        g_autofree char *role = i < bits ? g_strdup_printf("d%u", i) :
            g_strdup(rgb ? rgb_control[i - bits] : control[i - bits]);
        g_autofree char *id = g_strdup_printf("D.%s", role);
        unsigned pad = i < bits ? 4 + i : 20 + i - bits;
        terminal(c, id, role, "digital", -1);
        QList *wire = net(p, role);
        qlist_append_str(wire, id);
        bool reset = !strcmp(role, "reset");
        bool connected = !(fault == 1 && !strcmp(role, "wr")) &&
                         !(fault == 3 && i == 0);
        if (!reset && connected) {
            g_autofree char *pin = g_strdup_printf("U.io%u", fault == 2 &&
                !strcmp(role, "wr") ? 21 : pad);
            qlist_append_str(wire, pin);
        }
        pull(p, i, wire, reset || !strcmp(role, "cs") ? vdd : gnd);
    }
    if (fault == 4) {
        /* Remove peer rather than claiming absence still captured pixels. */
        QList *components = qdict_get_qlist(p, "components");
        QList *replacement = qlist_new();
        const QListEntry *e;
        QLIST_FOREACH_ENTRY(components, e) {
            QDict *item = qobject_to(QDict, qlist_entry_obj(e));
            if (strcmp(qdict_get_str(item, "id"), "D")) {
                qlist_append_obj(replacement, qobject_ref(QOBJECT(item)));
            }
        }
        qdict_put(p, "components", replacement);
        /* Remove its endpoints so graph remains valid. */
        QLIST_FOREACH_ENTRY(qdict_get_qlist(p, "nets"), e) {
            QDict *n = qobject_to(QDict, qlist_entry_obj(e));
            QList *ends = qlist_new();
            const QListEntry *a;
            QLIST_FOREACH_ENTRY(qdict_get_qlist(n, "endpoints"), a) {
                QString *str = qobject_to(QString, qlist_entry_obj(a));
                if (!g_str_has_prefix(qstring_get_str(str), "D.")) {
                    qlist_append_obj(ends, qobject_ref(QOBJECT(str)));
                }
            }
            qdict_put(n, "endpoints", ends);
        }
    }
    set_json(f, ELECTRICAL, "project-json", p);
    qobject_unref(p);
    for (unsigned i = 0; i < bits; ++i) {
        qtest_writel(f->q, OUT(4 + i), 133 + i);
    }
    qtest_writel(f->q, OUT(20), 154);
    qtest_writel(f->q, OUT(21), rgb ? 150 : 153);
    qtest_writel(f->q, OUT(22), rgb ? 151 : 132);
    if (rgb) {
        qtest_writel(f->q, OUT(23), 152);
    }
    qtest_writel(f->q, GPIO(A_GPIO_ENABLE_W1TS),
                 (((1U << bits) - 1) << 4) | BIT(20) | BIT(21) | BIT(22) |
                 (rgb ? BIT(23) : 0));
    step(f, 0);
}
static Fixture start(void)
{
    Fixture f = { .q = qtest_init("-machine esp32s3 -S -L pc-bios "
        "-global driver=esp32s3.gpio,property=strap_mode,value=0x00") };
    qtest_irq_intercept_out(f.q, CONTROLLER);
    qtest_writel(f.q, EN1, qtest_readl(f.q, EN1) | GATE);
    for (unsigned i = 0; i < 39; ++i) {
        qtest_writel(f.q, PAD(i), INPUT);
    }
    f.now = qtest_clock_step(f.q, 0);
    return f;
}
static bool pin(Fixture *f, unsigned n)
{
    return !!(qtest_readl(f->q, GPIO(n < 32 ? A_GPIO_IN : A_GPIO_IN1)) & BIT(n % 32));
}
static void descriptor(Fixture *f, uint32_t address, unsigned size,
                       unsigned length, bool eof, uint32_t buffer, uint32_t next)
{
    qtest_writel(f->q, address, DESC(size, length, eof));
    qtest_writel(f->q, address + 4, buffer);
    qtest_writel(f->q, address + 8, next);
}
static void dma_start(Fixture *f, bool out, unsigned channel, uint32_t address)
{
    qtest_writel(f->q, DMA(channel, out, 0x00), out ? BIT(2) : 0);
    qtest_writel(f->q, DMA(channel, out, 0x04), BIT(12));
    qtest_writel(f->q, DMA(channel, out, 0x14), UINT32_MAX);
    qtest_writel(f->q, DMA(channel, out, 0x48), 5);
    qtest_writel(f->q, DMA(channel, out, 0x20),
                 (address & 0xfffff) | (out ? BIT(21) : BIT(22)));
}
static void tx_payload(Fixture *f, const uint8_t *bytes, unsigned length)
{
    g_assert_cmpuint(length, >, 0);
    g_assert_cmpuint(length, <=, 8192);
    qtest_memwrite(f->q, SRC, bytes, length);
    unsigned offset = 0, i = 0;
    while (offset < length) {
        unsigned n = MIN(2048, length - offset);
        descriptor(f, TD + 12 * i, n, n, offset + n == length,
                   SRC + offset, offset + n == length ? 0 : TD + 12 * (i + 1));
        offset += n;
        ++i;
    }
    dma_start(f, true, 0, TD);
}
static void irq(Fixture *f, uint32_t raw, uint32_t mask)
{
    g_assert_cmphex(rd(f, RAW), ==, raw);
    g_assert_cmphex(rd(f, ST), ==, raw & mask);
    g_assert_cmpint(qtest_get_irq(f->q, 0), ==, !!(raw & mask));
}
static void i80(Fixture *f, unsigned command, const uint8_t *data,
                unsigned length, Order o)
{
    if (length) {
        tx_payload(f, data, length);
    }
    wr(f, CLR, 15);
    wr(f, CLOCK, CLK1);
    wr(f, MISC, BIT(28)); /* command DC=0, data DC=1; idle=0 */
    wr(f, CMD, command);
    unsigned cycles = length / (o.bits / 8);
    uint32_t user = COMMAND | (o.bits == 16 ? BUS16 : 0) |
        (o.reverse ? BITREV : 0) | (o.swap ? (o.bits == 16 ? BYTESWAP16 : BYTESWAP8) : 0) |
        (length ? DATA | (cycles - 1) : 0);
    wr(f, USER, user | UPDATE);
    wr(f, USER, user | START);
    /* One command plus data; completion must wait past the final hold. */
    step(f, (cycles + 1) * 1000 + 500);
    g_assert_cmphex(rd(f, RAW) & BIT(1), ==, BIT(1));
    g_assert_false(pin(f, 20));
    g_assert_true(pin(f, 22));
}
static uint16_t reverse_bits(uint16_t value, unsigned bits)
{
    uint16_t result = 0;
    for (unsigned i = 0; i < bits; ++i) {
        result = (result << 1) | ((value >> i) & 1);
    }
    return result;
}
/* Reference starts with intended physical pixels, inverts wire transforms to
 * obtain DMA bytes, and expands RGB565 without reading controller status. */
static void make_pixels(uint8_t *dma, uint8_t *rgb, unsigned pixels, Order o)
{
    for (unsigned i = 0; i < pixels; ++i) {
        unsigned r = (i * 3 + 1) & 31, g = (i * 7 + 5) & 63, b = (i * 11 + 9) & 31;
        uint16_t color = (r << 11) | (g << 5) | b;
        rgb[3 * i] = (r << 3) | (r >> 2);
        rgb[3 * i + 1] = (g << 2) | (g >> 4);
        rgb[3 * i + 2] = (b << 3) | (b >> 2);
        if (o.bits == 16) {
            uint16_t raw = o.reverse ? reverse_bits(color, 16) : color;
            if (o.swap) {
                raw = (raw << 8) | (raw >> 8);
            }
            dma[2 * i] = raw;
            dma[2 * i + 1] = raw >> 8;
        } else {
            uint8_t high = color >> 8, low = color;
            if (o.reverse) {
                high = reverse_bits(high, 8);
                low = reverse_bits(low, 8);
            }
            dma[2 * i + (o.swap ? 1 : 0)] = high;
            dma[2 * i + (o.swap ? 0 : 1)] = low;
        }
    }
}
static QDict *capture(Fixture *f)
{
    QDict *request = object("{\"componentId\":\"D\",\"offset\":0,\"count\":64}");
    set_json(f, PANELS, "capture-request-json", request);
    qobject_unref(request);
    return get_json(f, PANELS, "capture-json");
}
static QDict *frame_at(QDict *capture, unsigned n)
{
    const QListEntry *e;
    QLIST_FOREACH_ENTRY(qdict_get_qlist(capture, "frames"), e) {
        if (!n--) {
            return qobject_to(QDict, qlist_entry_obj(e));
        }
    }
    g_assert_not_reached();
}
static void assert_frame(QDict *frame, const uint8_t *rgb, unsigned pixels)
{
    g_autofree char *hash = g_compute_checksum_for_data(G_CHECKSUM_SHA256, rgb, pixels * 3);
    g_assert_true(qdict_get_bool(frame, "valid"));
    g_assert_cmpuint(qdict_get_int(frame, "errors"), ==, 0);
    g_assert_cmpuint(qdict_get_int(frame, "pixels"), ==, pixels);
    g_assert_cmpstr(qdict_get_str(frame, "sha256_rgb888"), ==, hash);
}
static void assert_framebuffer(Fixture *f, unsigned sequence,
                               const uint8_t *rgb, unsigned length)
{
    QDict *request = qdict_new();
    qdict_put_str(request, "componentId", "D");
    qdict_put_int(request, "sequence", sequence);
    qdict_put_int(request, "offset", 0);
    qdict_put_int(request, "count", length);
    set_json(f, PANELS, "framebuffer-request-json", request);
    qobject_unref(request);
    QDict *fb = get_json(f, PANELS, "framebuffer-json");
    const char *hex = qdict_get_str(fb, "hex");
    g_assert_cmpuint(strlen(hex), ==, length * 2);
    for (unsigned i = 0; i < length; ++i) {
        unsigned value = (g_ascii_xdigit_value(hex[2 * i]) << 4) |
                         g_ascii_xdigit_value(hex[2 * i + 1]);
        g_assert_cmphex(value, ==, rgb[i]);
    }
    qobject_unref(fb);
}
static void panel_init(Fixture *f, unsigned bits)
{
    Order order = {.bits = bits};
    i80(f, 0x11, NULL, 0, order);
    i80(f, 0x29, NULL, 0, order);
    const uint8_t format[] = {0x55, 0};
    i80(f, 0x3a, format, bits / 8, order);
}
static void test_i80_order(gconstpointer opaque)
{
    const Order *o = opaque;
    Fixture f = start();
    panel_graph(&f, false, o->bits, 64, 40, false, 0);
    panel_init(&f, o->bits);
    uint8_t dma[5120], rgb[7680];
    make_pixels(dma, rgb, 2560, *o);
    i80(&f, 0x2c, dma, sizeof(dma), *o);
    QDict *c = capture(&f);
    g_assert_cmpuint(qlist_size(qdict_get_qlist(c, "frames")), ==, 1);
    assert_frame(frame_at(c, 0), rgb, 2560);
    g_assert_true(qdict_get_bool(frame_at(c, 0), "visible"));
    g_assert_cmpuint(qdict_get_int(qdict_get_qdict(c, "status"), "data_samples"), ==,
                     1 + sizeof(dma) / (o->bits / 8));
    assert_framebuffer(&f, 0, rgb, sizeof(rgb));
    qobject_unref(c);
    qtest_quit(f.q);
}
static void test_i80_wire_hold(void)
{
    Fixture f = start();
    panel_graph(&f, false, 8, 1, 1, false, 0);
    panel_init(&f, 8);
    const uint8_t bytes[] = {0xf8, 0x00};
    tx_payload(&f, bytes, sizeof(bytes));
    /* OUT EOF is legal at FIFO fill, not panel completion. */
    g_assert_cmphex(qtest_readl(f.q, DMA(0, true, 8)) & BIT(1), ==, BIT(1));
    wr(&f, CLR, 15);
    wr(&f, ENA, BIT(1));
    wr(&f, CMD, 0x2c);
    wr(&f, USER, COMMAND | DATA | 1 | UPDATE);
    wr(&f, USER, COMMAND | DATA | 1 | START);
    step(&f, 500);
    irq(&f, 0, BIT(1));
    g_assert_true(pin(&f, 20));
    g_assert_false(pin(&f, 21));
    g_assert_false(pin(&f, 22));
    for (unsigned i = 0; i < 8; ++i) {
        g_assert_cmpint(pin(&f, 4 + i), ==, !!(0x2c & BIT(i)));
    }
    step(&f, 1000);
    g_assert_true(pin(&f, 21));
    for (unsigned i = 0; i < 8; ++i) {
        g_assert_cmpint(pin(&f, 4 + i), ==, !!(0xf8 & BIT(i)));
    }
    step(&f, 1000); /* final data rising edge */
    irq(&f, 0, BIT(1));
    g_assert_false(pin(&f, 22));
    step(&f, 500); /* final falling edge/hold */
    irq(&f, 0, BIT(1));
    g_assert_false(pin(&f, 20));
    step(&f, 499);
    irq(&f, 0, BIT(1));
    step(&f, 1);
    irq(&f, BIT(1), BIT(1));
    g_assert_true(pin(&f, 22));
    wr(&f, CLR, BIT(1));
    irq(&f, 0, BIT(1));
    qtest_quit(f.q);
}
static void test_panel_wiring(gconstpointer opaque)
{
    unsigned fault = GPOINTER_TO_UINT(opaque);
    Fixture f = start();
    panel_graph(&f, false, 8, 1, 1, false, fault);
    panel_init(&f, 8);
    const uint8_t bytes[] = {0xff, 0xff};
    i80(&f, 0x2c, bytes, sizeof(bytes), (Order){.bits = 8});
    QDict *s = get_json(&f, PANELS, "status-json");
    QList *panels = qdict_get_qlist(s, "panels");
    if (fault == 4) {
        g_assert_cmpuint(qlist_size(panels), ==, 0);
    } else {
        QDict *p = qobject_to(QDict, qlist_entry_obj(qlist_first(panels)));
        g_assert_cmpuint(qdict_get_int(p, "captures"), ==, 0);
    }
    qobject_unref(s);
    qtest_quit(f.q);
}
static void test_i80_gate_reset_underflow(void)
{
    Fixture f = start();
    panel_graph(&f, false, 8, 1, 1, false, 0);
    panel_init(&f, 8);
    wr(&f, CLR, 15);
    wr(&f, CMD, 0x2c);
    wr(&f, USER, COMMAND | DATA | 1 | START);
    step(&f, 1500); /* command went out; no data descriptor exists */
    QDict *status = get_json(&f, CONTROLLER, "status-json");
    g_assert_true(qdict_get_bool(status, "lcd_stalled"));
    g_assert_cmpuint(qdict_get_int(status, "tx_underflow"), ==, 1);
    qobject_unref(status);
    irq(&f, 0, 0);
    const uint8_t payload[] = {0x07, 0xe0};
    tx_payload(&f, payload, sizeof(payload));
    step(&f, 500);
    qtest_writel(f.q, EN1, qtest_readl(f.q, EN1) & ~GATE);
    step(&f, 100000);
    irq(&f, 0, 0);
    qtest_writel(f.q, RST1, qtest_readl(f.q, RST1) | GATE);
    g_assert_cmphex(rd(&f, USER), ==, 0);
    g_assert_cmphex(rd(&f, RAW), ==, 0);
    wr(&f, ENA, 15); /* held reset ignores MMIO */
    g_assert_cmphex(rd(&f, ENA), ==, 0);
    qtest_writel(f.q, RST1, qtest_readl(f.q, RST1) & ~GATE);
    qtest_writel(f.q, EN1, qtest_readl(f.q, EN1) | GATE);
    step(&f, 10000);
    irq(&f, 0, 0);
    i80(&f, 0x2c, payload, sizeof(payload), (Order){.bits = 8});
    QDict *c = capture(&f);
    const uint8_t green[] = {0, 255, 0};
    assert_frame(frame_at(c, 0), green, 1);
    qobject_unref(c);
    qtest_quit(f.q);
}
/* 4x3 active, 2/2/2 horizontal and 1/1/1 vertical pulse/back/front:
 * 10 clocks x 6 lines. VSYNC belongs to controller at pulse end (10us),
 * not the GDMA EOF or first active pixel (24.5us). */
static void rgb_setup(Fixture *f, bool inverted)
{
    wr(f, CLOCK, CLK1 ^ (inverted ? BIT(7) | BIT(8) : 0));
    wr(f, CTRL, RGB | 3 | (2 << 11) | (5 << 21));
    wr(f, CTRL1, 1 | (3 << 8) | (9 << 20));
    wr(f, CTRL2, BIT(9) | (1 << 16) |
       (inverted ? BIT(7) | BIT(8) | BIT(23) : 0));
    wr(f, MISC, BIT(25)); /* continuous */
    wr(f, USER, BUS16 | UPDATE);
    wr(f, USER, BUS16 | START);
}
static void test_rgb(gconstpointer opaque)
{
    bool inverted = GPOINTER_TO_UINT(opaque);
    Fixture f = start();
    panel_graph(&f, true, 16, 4, 3, inverted, 0);
    uint8_t dma[96], rgb[36];
    make_pixels(dma, rgb, 12, (Order){.bits = 16});
    for (unsigned i = 1; i < 4; ++i) {
        memcpy(dma + 24 * i, dma, 24);
    }
    tx_payload(&f, dma, sizeof(dma));
    wr(&f, ENA, BIT(0));
    rgb_setup(&f, inverted);
    step(&f, 9999);
    irq(&f, 0, BIT(0));
    step(&f, 1);
    irq(&f, BIT(0), BIT(0));
    wr(&f, CLR, BIT(0));
    /* Divider UPDATE pending mid-frame: remaining frame stays 1MHz. */
    wr(&f, CLOCK, (CLK1 & ~(255U << 9)) | (80U << 9) |
       (inverted ? BIT(7) : 0));
    if (inverted) {
        wr(&f, CLOCK, rd(&f, CLOCK) & ~BIT(8));
    }
    wr(&f, USER, BUS16 | START | UPDATE);
    step(&f, 50499); /* first next-frame edge at 60.5us (new divider=2us) */
    QDict *before = capture(&f);
    g_assert_cmpuint(qlist_size(qdict_get_qlist(before, "frames")), ==, 0);
    qobject_unref(before);
    step(&f, 501); /* new period starts at frame boundary: first edge=61us */
    QDict *c = capture(&f);
    g_assert_cmpuint(qlist_size(qdict_get_qlist(c, "frames")), ==, 1);
    QDict *frame = frame_at(c, 0);
    assert_frame(frame, rgb, 12);
    g_assert_cmpuint(qdict_get_int(frame, "pclk_samples"), ==, 60);
    g_assert_cmpuint(qdict_get_int(frame, "lines"), ==, 6);
    g_assert_cmpuint(qdict_get_int(frame, "observed_width"), ==, 4);
    g_assert_cmpuint(qdict_get_int(frame, "observed_height"), ==, 3);
    g_assert_cmpuint(qdict_get_int(frame, "observed_hsync_width"), ==, 2);
    g_assert_cmpuint(qdict_get_int(frame, "observed_h_back_porch"), ==, 2);
    g_assert_cmpuint(qdict_get_int(frame, "observed_h_front_porch"), ==, 2);
    g_assert_cmpuint(qdict_get_int(frame, "observed_vsync_width"), ==, 1);
    g_assert_cmpuint(qdict_get_int(frame, "observed_v_back_porch"), ==, 1);
    g_assert_cmpuint(qdict_get_int(frame, "observed_v_front_porch"), ==, 1);
    g_assert_cmpuint(qdict_get_int(frame, "period_min_ns"), ==, 1000);
    /* Boundary gap includes new half-period; do not normalize it away. */
    g_assert_cmpuint(qdict_get_int(frame, "period_max_ns"), ==, 1000);
    qobject_unref(c);
    wr(&f, CLR, 15);
    step(&f, 19000);
    irq(&f, BIT(0), BIT(0));
    step(&f, 101000); /* second frame captured at 181us */
    c = capture(&f);
    g_assert_cmpuint(qlist_size(qdict_get_qlist(c, "frames")), ==, 2);
    frame = frame_at(c, 1);
    assert_frame(frame, rgb, 12);
    g_assert_cmpuint(qdict_get_int(frame, "period_min_ns"), ==, 2000);
    g_assert_cmpuint(qdict_get_int(frame, "period_max_ns"), ==, 2000);
    assert_framebuffer(&f, 1, rgb, sizeof(rgb));
    qobject_unref(c);
    wr(&f, USER, BUS16); /* explicit stop does NOT assert TRANS_DONE */
    g_assert_cmphex(rd(&f, RAW) & BIT(1), ==, 0);
    qtest_quit(f.q);
}
static void test_rgb_starvation(void)
{
    Fixture f = start();
    panel_graph(&f, true, 16, 4, 3, false, 0);
    rgb_setup(&f, false);
    step(&f, 24500);
    QDict *s = get_json(&f, CONTROLLER, "status-json");
    g_assert_true(qdict_get_bool(s, "lcd_stalled"));
    g_assert_cmpuint(qdict_get_int(s, "lcd_frames"), ==, 0);
    g_assert_cmpuint(qdict_get_int(s, "lcd_words"), ==, 0);
    qobject_unref(s);
    step(&f, 60000);
    QDict *c = capture(&f);
    g_assert_cmpuint(qlist_size(qdict_get_qlist(c, "frames")), ==, 0);
    qobject_unref(c);
    uint8_t dma[96], rgb[36];
    make_pixels(dma, rgb, 12, (Order){.bits = 16});
    for (unsigned i = 1; i < 4; ++i) {
        memcpy(dma + i * 24, dma, 24);
    }
    tx_payload(&f, dma, sizeof(dma));
    step(&f, 100000);
    c = capture(&f);
    g_assert_cmpuint(qlist_size(qdict_get_qlist(c, "frames")), >=, 1);
    assert_frame(frame_at(c, 0), rgb, 12);
    qobject_unref(c);
    qtest_quit(f.q);
}
/* Camera-loop physical GPIO roles: 0..15 data sources, 16 PCLK, 17 HREF,
 * 18 VSYNC; receivers 20..35 data, 36 PCLK, 37 HREF, 38 VSYNC. Reserved
 * module pads are used ONLY in this paused controller-loop qtest, not in
 * ordinary firmware/hardware qualification. Every source is a real finite
 * GPIO output; qtest never sets GPIO input or controller IRQ directly. */
static void cam_graph(Fixture *f, unsigned bits, int missing)
{
    QList *vdd, *gnd;
    QDict *p = project("cam-mcu-physical-loop-register-boundary-only", 39, &vdd, &gnd);
    for (unsigned i = 0; i < 19; ++i) {
        if (i >= bits && i < 16) {
            continue;
        }
        g_autofree char *id = g_strdup_printf("loop%u", i);
        g_autofree char *source = g_strdup_printf("U.io%u", i);
        g_autofree char *sink = g_strdup_printf("U.io%u", i + 20);
        QList *wire = net(p, id);
        qlist_append_str(wire, source);
        if ((int)i != missing) {
            qlist_append_str(wire, sink);
        } else {
            g_autofree char *absent = g_strdup_printf("disconnected%u", i);
            qlist_append_str(net(p, absent), sink);
        }
        pull(p, i, wire, gnd);
        qtest_writel(f->q, OUT(i), 256);
        qtest_writel(f->q, IN(i < 16 ? 133 + i : i == 16 ? 149 : i == 17 ? 150 : 152),
                     (i + 20) | BIT(7));
    }
    set_json(f, ELECTRICAL, "project-json", p);
    qobject_unref(p);
    f->gpio_output = 0;
    qtest_writel(f->q, GPIO(A_GPIO_OUT), 0);
    qtest_writel(f->q, GPIO(A_GPIO_ENABLE_W1TS), ((1U << bits) - 1) | BIT(16) | BIT(17) | BIT(18));
    step(f, 0);
}
static void cam_drive(Fixture *f, uint16_t data, bool pclk, bool href, bool vsync)
{
    f->gpio_output = data | (pclk ? BIT(16) : 0) |
        (href ? BIT(17) : 0) | (vsync ? BIT(18) : 0);
    qtest_writel(f->q, GPIO(A_GPIO_OUT), f->gpio_output);
    step(f, 100);
}
static void cam_setup(Fixture *f, unsigned bits, bool frame_eof,
                      bool reverse, bool swap, bool inverted, bool swap8)
{
    wr(f, ENA, BIT(2) | BIT(3));
    wr(f, CAM_CONV, swap8 ? BIT(21) : 0);
    wr(f, CAM_CTRL, (frame_eof ? BIT(8) : 0) | BIT(7) |
       (reverse ? BIT(6) : 0) | (swap ? BIT(5) : 0) | BIT(4));
    wr(f, CAM_CTRL1, 7 | (bits == 16 ? BIT(24) : 0) |
       (inverted ? BIT(22) | BIT(25) | BIT(27) : 0) | BIT(29));
    cam_drive(f, 0, inverted, inverted, inverted); /* establish actual idle */
    cam_drive(f, 0, inverted, inverted, !inverted); /* synchronize VSYNC */
    cam_drive(f, 0, inverted, inverted, inverted);
    irq(f, BIT(2), BIT(2) | BIT(3));
    wr(f, CLR, 15);
}
static void cam_word(Fixture *f, uint16_t word, bool inverted)
{
    cam_drive(f, word, inverted, !inverted, inverted);
    cam_drive(f, word, !inverted, !inverted, inverted);
    cam_drive(f, word, inverted, !inverted, inverted);
}
static void rx_chain(Fixture *f, unsigned length, unsigned segment, bool owned)
{
    qtest_memset(f->q, DST, 0xa5, length + 16);
    for (unsigned offset = 0, i = 0; offset < length; offset += segment, ++i) {
        unsigned n = MIN(segment, length - offset);
        descriptor(f, RD + 12 * i, n, 0, false, DST + offset,
                   offset + n < length ? RD + 12 * (i + 1) : 0);
        if (!owned) {
            qtest_writel(f->q, RD + 12 * i, n);
        }
    }
    dma_start(f, false, 1, RD);
}
static void test_cam_orders(gconstpointer opaque)
{
    unsigned flags = GPOINTER_TO_UINT(opaque);
    unsigned bits = flags & 1 ? 16 : 8;
    bool reverse = flags & 2, swap = flags & 4, frame_eof = flags & 8;
    bool inverted = flags & 16, pair = bits == 8 && swap;
    Fixture f = start();
    cam_graph(&f, bits, -1);
    rx_chain(&f, 24, 8, true);
    cam_setup(&f, bits, frame_eof, reverse, bits == 16 && swap, inverted, pair);
    uint8_t expected[24], actual[40];
    for (unsigned i = 0; i < 24 / (bits / 8); ++i) {
        uint16_t word = (0x1357U + i * 0x271U) & (bits == 16 ? 65535 : 255);
        uint16_t ordered = reverse ? reverse_bits(word, bits) : word;
        if (bits == 16 && swap) {
            ordered = (ordered << 8) | (ordered >> 8);
        }
        if (bits == 16) {
            expected[2 * i] = ordered;
            expected[2 * i + 1] = ordered >> 8;
        } else {
            expected[pair ? i ^ 1 : i] = ordered;
        }
        cam_word(&f, word, inverted);
        if (!i) {
            irq(&f, BIT(3), BIT(2) | BIT(3)); /* actual HREF line event */
        }
        if ((i + 1) * (bits / 8) == 8) {
            uint32_t dma_raw = qtest_readl(f.q, DMA(1, false, 8));
            g_assert_cmphex(dma_raw & BIT(1), ==, frame_eof ? 0 : BIT(1));
            g_assert_cmphex(rd(&f, RAW) & BIT(2), ==, 0);
        }
    }
    /* HREF-low clocks and VSYNC-high clocks cannot append bytes. */
    cam_drive(&f, 0xeeee, inverted, inverted, inverted);
    cam_drive(&f, 0xeeee, !inverted, inverted, inverted);
    cam_drive(&f, 0xeeee, inverted, inverted, !inverted);
    g_assert_cmphex(rd(&f, RAW) & BIT(2), ==, BIT(2));
    qtest_memread(f.q, DST, actual, sizeof(actual));
    g_assert_cmpmem(actual, sizeof(expected), expected, sizeof(expected));
    for (unsigned i = 24; i < sizeof(actual); ++i) {
        g_assert_cmphex(actual[i], ==, 0xa5);
    }
    for (unsigned i = 0; i < 3; ++i) {
        uint32_t d = qtest_readl(f.q, RD + 12 * i);
        g_assert_cmphex(d & BIT(31), ==, 0);
        g_assert_cmpuint((d >> 12) & 4095, ==, 8);
        g_assert_cmphex(d & BIT(30), ==, (!frame_eof || i == 2) ? BIT(30) : 0);
    }
    g_assert_cmphex(qtest_readl(f.q, DMA(1, false, 8)) & BIT(1), ==, BIT(1));
    g_assert_cmphex(qtest_readl(f.q, DMA(1, false, 0x28)), ==, RD + 24);
    qtest_quit(f.q);
}
static void test_cam_slow_reset_errors(void)
{
    Fixture f = start();
    cam_graph(&f, 8, -1);
    cam_setup(&f, 8, true, false, false, false, false);
    for (unsigned i = 0; i < 16; ++i) {
        cam_word(&f, 0x40 + i, false); /* FIFO buffers real edges without DMA */
    }
    rx_chain(&f, 24, 8, true);
    for (unsigned i = 16; i < 24; ++i) {
        cam_word(&f, 0x40 + i, false);
    }
    cam_drive(&f, 0, false, false, true);
    uint8_t data[24];
    qtest_memread(f.q, DST, data, sizeof(data));
    for (unsigned i = 0; i < sizeof(data); ++i) {
        g_assert_cmphex(data[i], ==, 0x40 + i);
    }
    /* No consumer: finite 32-byte FIFO, 33rd edge reports overflow; no
     * successful EOF, fake bytes or endlessly scheduled internal work. */
    qtest_writel(f.q, DMA(1, false, 0), BIT(0));
    cam_drive(&f, 0, false, false, false);
    for (unsigned i = 0; i < 33; ++i) {
        cam_word(&f, i, false);
    }
    QDict *s = get_json(&f, CONTROLLER, "status-json");
    g_assert_cmpuint(qdict_get_int(s, "rx_overflow"), ==, 1);
    g_assert_cmpuint(qdict_get_int(s, "rx_fifo_bytes"), ==, 32);
    qobject_unref(s);
    cam_drive(&f, 0, false, false, true);
    s = get_json(&f, CONTROLLER, "status-json");
    g_assert_cmpuint(qdict_get_int(s, "dropped_frames"), ==, 1);
    qobject_unref(s);
    wr(&f, CAM_CTRL1, BIT(30));
    g_assert_cmphex(rd(&f, CAM_CTRL1) & BIT(29), ==, 0);
    rx_chain(&f, 8, 8, false); /* real descriptor owner fault */
    cam_setup(&f, 8, true, false, false, false, false);
    cam_word(&f, 0x99, false);
    g_assert_cmphex(qtest_readl(f.q, DMA(1, false, 8)) & BIT(3), ==, BIT(3));
    g_assert_cmphex(qtest_readl(f.q, DST), ==, 0xa5a5a5a5);
    wr(&f, CAM_CTRL1, BIT(30));
    rx_chain(&f, 8, 8, true);
    cam_setup(&f, 8, true, false, false, false, false);
    for (unsigned i = 0; i < 5; ++i) {
        cam_word(&f, 0x80 + i, false);
    }
    cam_drive(&f, 0, false, false, true);
    uint32_t d = qtest_readl(f.q, RD);
    g_assert_cmpuint((d >> 12) & 4095, ==, 5);
    g_assert_cmphex(d & (BIT(30) | BIT(31)), ==, BIT(30));
    qtest_memread(f.q, DST, data, 8);
    for (unsigned i = 0; i < 8; ++i) {
        g_assert_cmphex(data[i], ==, i < 5 ? 0x80 + i : 0xa5);
    }
    qtest_quit(f.q);
}
static void test_cam_absent_wire(gconstpointer opaque)
{
    int missing = GPOINTER_TO_INT(opaque);
    Fixture f = start();
    cam_graph(&f, 8, missing);
    rx_chain(&f, 8, 8, true);
    wr(&f, CAM_CTRL, BIT(8) | BIT(4));
    wr(&f, CAM_CTRL1, BIT(29));
    cam_drive(&f, 0, false, false, false);
    cam_drive(&f, 0, false, false, true);
    cam_drive(&f, 0, false, false, false);
    for (unsigned i = 0; i < 8; ++i) {
        cam_word(&f, 0xff, false);
    }
    cam_drive(&f, 0, false, false, true);
    g_assert_cmphex(qtest_readl(f.q, DST), ==, 0xa5a5a5a5);
    g_assert_cmphex(qtest_readl(f.q, DMA(1, false, 8)) & BIT(1), ==, 0);
    QDict *s = get_json(&f, CONTROLLER, "status-json");
    g_assert_cmpuint(qdict_get_int(s, "cam_bytes"), ==, 0);
    if (!missing) {
        g_assert_cmpuint(qdict_get_int(s, "invalid_edges"), >=, 1);
    }
    qobject_unref(s);
    qtest_quit(f.q);
}
static void test_immutable_bounded_capture(void)
{
    Fixture f = start();
    panel_graph(&f, false, 8, 1, 1, false, 0);
    panel_init(&f, 8);
    const uint8_t red[] = {0xf8, 0}, blue[] = {0, 0x1f};
    const uint8_t red_rgb[] = {255, 0, 0}, blue_rgb[] = {0, 0, 255};
    i80(&f, 0x2c, red, sizeof(red), (Order){.bits = 8});
    QDict *c = capture(&f);
    assert_frame(frame_at(c, 0), red_rgb, 1);
    qobject_unref(c);
    assert_framebuffer(&f, 0, red_rgb, sizeof(red_rgb));
    i80(&f, 0x2c, blue, sizeof(blue), (Order){.bits = 8});
    /* Getter does not advance either frozen window to the new frame. */
    c = get_json(&f, PANELS, "capture-json");
    g_assert_cmpuint(qdict_get_int(c, "total_at_request"), ==, 1);
    g_assert_cmpuint(qlist_size(qdict_get_qlist(c, "frames")), ==, 1);
    assert_frame(frame_at(c, 0), red_rgb, 1);
    qobject_unref(c);
    QDict *fb = get_json(&f, PANELS, "framebuffer-json");
    g_assert_cmpuint(qdict_get_int(fb, "sequence"), ==, 0);
    g_assert_cmpstr(qdict_get_str(fb, "hex"), ==, "ff0000");
    qobject_unref(fb);
    c = capture(&f);
    g_assert_cmpuint(qlist_size(qdict_get_qlist(c, "frames")), ==, 2);
    assert_frame(frame_at(c, 1), blue_rgb, 1);
    qobject_unref(c);
    assert_framebuffer(&f, 1, blue_rgb, sizeof(blue_rgb));
    /* Bounds are enforced by the public service, not silently truncated at
     * an unbounded caller request. Read-only captures need no output files. */
    QDict *reply = qtest_qmp(f.q, "{'execute':'qom-set','arguments':"
        "{'path':%s,'property':'capture-request-json',"
        "'value':'{\"componentId\":\"D\",\"offset\":0,\"count\":65}'}}", PANELS);
    g_assert_true(qdict_haskey(reply, "error"));
    qobject_unref(reply);
    reply = qtest_qmp(f.q, "{'execute':'qom-set','arguments':"
        "{'path':%s,'property':'framebuffer-request-json',"
        "'value':'{\"componentId\":\"D\",\"sequence\":1,\"offset\":0,\"count\":65537}'}}", PANELS);
    g_assert_true(qdict_haskey(reply, "error"));
    qobject_unref(reply);
    qtest_quit(f.q);
}
static void test_lcd_clock_divider(gconstpointer opaque)
{
    unsigned field = GPOINTER_TO_UINT(opaque);
    unsigned divider = field == 0 ? 256 : field == 1 ? 2 : field;
    unsigned half_ns = divider * 25 / 2; /* XTAL40MHz, EQU_SYSCLK=1 */
    Fixture f = start();
    panel_graph(&f, false, 8, 1, 1, false, 0);
    wr(&f, CLOCK, BIT(29) | (field << 9) | BIT(6) | BIT(8));
    wr(&f, CMD, 0x11);
    wr(&f, CLR, 15);
    wr(&f, ENA, BIT(1));
    wr(&f, USER, COMMAND | UPDATE | START);
    step(&f, half_ns - 1);
    g_assert_false(pin(&f, 20));
    step(&f, 1);
    g_assert_true(pin(&f, 20));
    irq(&f, 0, BIT(1));
    step(&f, half_ns);
    g_assert_false(pin(&f, 20));
    irq(&f, 0, BIT(1));
    step(&f, half_ns - 1);
    irq(&f, 0, BIT(1));
    step(&f, 1);
    irq(&f, BIT(1), BIT(1));
    qtest_quit(f.q);
}
static void test_lcd_signal_delay(gconstpointer opaque)
{
    unsigned mode = GPOINTER_TO_UINT(opaque);
    Fixture f = start();
    panel_graph(&f, false, 8, 1, 1, false, 0);
    panel_init(&f, 8);
    const uint8_t bytes[] = {0xf8, 0};
    const uint8_t red[] = {255, 0, 0};
    tx_payload(&f, bytes, sizeof(bytes));
    /* LCD module10MHz, PCLK1MHz. WR falls first: delayed command/data
     * settle at +50ns (LCD rising) or +100ns (LCD falling), before WR rises. */
    wr(&f, CLOCK, BIT(29) | (4 << 9) | 9 | BIT(7));
    wr(&f, 0x30, mode);
    wr(&f, 0x38, mode == 1 ? 0x5555 : 0xaaaa);
    wr(&f, CMD, 0x2c);
    wr(&f, CLR, 15);
    wr(&f, ENA, BIT(1));
    wr(&f, USER, COMMAND | DATA | 1 | UPDATE | START);
    unsigned delay_ns = mode == 1 ? 50 : 100;
    step(&f, 500);
    g_assert_false(pin(&f, 20));
    g_assert_true(pin(&f, 8)); /* previous COLMOD data0x55 bit4 */
    step(&f, delay_ns - 1);
    g_assert_true(pin(&f, 8));
    step(&f, 1);
    g_assert_false(pin(&f, 8)); /* command0x2c bit4 */
    step(&f, 1000 - delay_ns);
    g_assert_false(pin(&f, 20));
    g_assert_false(pin(&f, 21));
    g_assert_false(pin(&f, 8));
    step(&f, delay_ns - 1);
    g_assert_false(pin(&f, 21));
    step(&f, 1);
    g_assert_true(pin(&f, 21));
    g_assert_true(pin(&f, 8)); /* first pixel byte0xf8 bit4 */
    step(&f, 2000 - delay_ns);
    irq(&f, BIT(1), BIT(1));
    QDict *c = capture(&f);
    g_assert_cmpuint(qlist_size(qdict_get_qlist(c, "frames")), ==, 1);
    assert_frame(frame_at(c, 0), red, 1);
    qobject_unref(c);
    qtest_quit(f.q);
}
static void test_cam_vsync_filter_and_rearm(void)
{
    Fixture f = start();
    cam_graph(&f, 8, -1);
    cam_drive(&f, 0, false, false, false);
    rx_chain(&f, 4, 4, true);
    wr(&f, ENA, BIT(2));
    wr(&f, CAM_CTRL, BIT(29) | (40 << 9) | BIT(8) | (1 << 1) | BIT(4));
    wr(&f, CAM_CTRL1, 7 | BIT(23) | BIT(29));
    cam_drive(&f, 0, false, false, true);
    step(&f, 1000); /* one CAM clock: below the threshold of two */
    irq(&f, 0, BIT(2));
    cam_drive(&f, 0, false, false, false);
    step(&f, 2000);
    irq(&f, 0, BIT(2)); /* short pulse was rejected, not delayed into an IRQ */
    cam_drive(&f, 0, false, false, true);
    step(&f, 1999);
    irq(&f, 0, BIT(2));
    step(&f, 1);
    irq(&f, BIT(2), BIT(2));
    /* The official HAL arms during this already-high VSYNC pulse. */
    wr(&f, CAM_CTRL1, 7 | BIT(23));
    wr(&f, CAM_CTRL1, 7 | BIT(23) | BIT(29));
    wr(&f, CLR, 15);
    cam_drive(&f, 0, false, false, false);
    step(&f, 2000);
    const uint8_t expected[] = {0x12, 0x34, 0x56, 0x78};
    for (unsigned i = 0; i < sizeof(expected); ++i) { cam_word(&f, expected[i], false); }
    cam_drive(&f, 0, false, false, true);
    step(&f, 1999);
    irq(&f, 0, BIT(2));
    g_assert_cmphex(qtest_readl(f.q, RD) & BIT(30), ==, 0);
    step(&f, 1);
    irq(&f, BIT(2), BIT(2));
    uint8_t actual[4];
    qtest_memread(f.q, DST, actual, sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), expected, sizeof(expected));
    g_assert_cmphex(qtest_readl(f.q, RD) & (BIT(31) | BIT(30)), ==, BIT(30));
    g_assert_cmpuint((qtest_readl(f.q, RD) >> 12) & 4095, ==, sizeof(expected));
    qtest_quit(f.q);
}
static void test_cam_overflow_closes_actual_packet(void)
{
    Fixture f = start();
    cam_graph(&f, 8, -1);
    rx_chain(&f, 4, 4, true);
    cam_setup(&f, 8, true, false, false, false, false);
    for (unsigned i = 0; i < 37; ++i) { cam_word(&f, 0x20 + i, false); }
    QDict *status = get_json(&f, CONTROLLER, "status-json");
    g_assert_cmpuint(qdict_get_int(status, "cam_bytes"), ==, 4);
    g_assert_cmpuint(qdict_get_int(status, "rx_overflow"), ==, 1);
    g_assert_cmpuint(qdict_get_int(status, "cam_frames"), ==, 0);
    qobject_unref(status);
    /* A data overflow is not a synthetic UHCI ERR_EOF. A real subsequent
     * VSYNC closes exactly the four fulfilled bytes, while image validity
     * remains false in the CAM status. No two frames can merge into DW0. */
    g_assert_cmphex(qtest_readl(f.q, RD) & BIT(30), ==, 0);
    cam_drive(&f, 0, false, false, true);
    uint32_t descriptor_word = qtest_readl(f.q, RD);
    g_assert_cmpuint((descriptor_word >> 12) & 4095, ==, 4);
    g_assert_cmphex(descriptor_word & (BIT(31) | BIT(30)), ==, BIT(30));
    g_assert_cmphex(qtest_readl(f.q, DMA(1, false, 8)) & BIT(1), ==, BIT(1));
    uint8_t actual[4];
    const uint8_t expected[] = {0x20, 0x21, 0x22, 0x23};
    qtest_memread(f.q, DST, actual, sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), expected, sizeof(expected));
    status = get_json(&f, CONTROLLER, "status-json");
    g_assert_cmpuint(qdict_get_int(status, "cam_frames"), ==, 0);
    g_assert_cmpuint(qdict_get_int(status, "dropped_frames"), ==, 1);
    qobject_unref(status);
    qtest_quit(f.q);
}
static void test_st_reset_ram_and_c2(gconstpointer opaque)
{
    bool hardware = GPOINTER_TO_UINT(opaque);
    Fixture f = start();
    panel_graph(&f, false, 8, 2, 1, false, 0);
    panel_init(&f, 8);
    const uint8_t analog[] = {1, 0xff};
    i80(&f, 0xc2, analog, sizeof(analog), (Order){.bits = 8});
    QDict *status = get_json(&f, PANELS, "status-json");
    QDict *panel = qobject_to(QDict, qlist_entry_obj(
        qlist_first(qdict_get_qlist(status, "panels"))));
    g_assert_cmpuint(qdict_get_int(qdict_get_qdict(panel, "errors"), "parameter_count"), ==, 0);
    qobject_unref(status);
    const uint8_t initial[] = {0xf8, 0, 0, 0x1f};
    const uint8_t initial_rgb[] = {255, 0, 0, 0, 0, 255};
    i80(&f, 0x2c, initial, sizeof(initial), (Order){.bits = 8});
    QDict *c = capture(&f);
    assert_frame(frame_at(c, 0), initial_rgb, 2);
    qobject_unref(c);
    if (hardware) {
        /* Add an explicit real MCU output to RESX's pulled-up net. Apply
         * keeps the same factory identity/power epoch and does not inject RAM. */
        QDict *p = get_json(&f, ELECTRICAL, "project-json");
        const QListEntry *entry;
        QLIST_FOREACH_ENTRY(qdict_get_qlist(p, "nets"), entry) {
            QDict *wire = qobject_to(QDict, qlist_entry_obj(entry));
            if (!strcmp(qdict_get_str(wire, "id"), "reset")) {
                qlist_append_str(qdict_get_qlist(wire, "endpoints"), "U.io23");
            }
        }
        set_json(&f, ELECTRICAL, "project-json", p);
        qobject_unref(p);
        qtest_writel(f.q, GPIO(A_GPIO_OUT_W1TS), BIT(23));
        qtest_writel(f.q, OUT(23), 256);
        qtest_writel(f.q, GPIO(A_GPIO_ENABLE_W1TS), BIT(23));
        qtest_writel(f.q, GPIO(A_GPIO_OUT_W1TC), BIT(23));
        step(&f, 1000);
        qtest_writel(f.q, GPIO(A_GPIO_OUT_W1TS), BIT(23));
    } else {
        i80(&f, 0x01, NULL, 0, (Order){.bits = 8});
    }
    panel_init(&f, 8);
    const uint8_t column[] = {0, 1, 0, 1};
    const uint8_t green[] = {7, 0xe0};
    const uint8_t retained[] = {255, 0, 0, 0, 255, 0};
    i80(&f, 0x2a, column, sizeof(column), (Order){.bits = 8});
    i80(&f, 0x2c, green, sizeof(green), (Order){.bits = 8});
    c = capture(&f);
    QDict *frame = frame_at(c, 1);
    g_assert_true(qdict_get_bool(frame, "valid"));
    g_assert_cmpuint(qdict_get_int(frame, "pixels"), ==, 1);
    g_autofree char *hash = g_compute_checksum_for_data(G_CHECKSUM_SHA256,
                                                       retained, sizeof(retained));
    g_assert_cmpstr(qdict_get_str(frame, "sha256_rgb888"), ==, hash);
    assert_framebuffer(&f, 1, retained, sizeof(retained));
    qobject_unref(c);
    qtest_quit(f.q);
}
static void test_st_independent_mcu_color_field(gconstpointer opaque)
{
    unsigned format = GPOINTER_TO_UINT(opaque);
    unsigned bits = (format & 7) == 5 ? 16 : 8;
    Fixture f = start();
    panel_graph(&f, false, bits, 1, 1, false, 0);
    panel_init(&f, bits);
    const uint8_t mode[] = {format, 0};
    i80(&f, 0x3a, mode, bits / 8, (Order){.bits = bits});
    const uint8_t rgb565_le[] = {0, 0xf8};
    const uint8_t rgb666[] = {0xfc, 0, 0};
    const uint8_t red[] = {255, 0, 0};
    i80(&f, 0x2c, bits == 16 ? rgb565_le : rgb666,
         bits == 16 ? sizeof(rgb565_le) : sizeof(rgb666), (Order){.bits = bits});
    QDict *c = capture(&f);
    assert_frame(frame_at(c, 0), red, 1);
    qobject_unref(c);
    qtest_quit(f.q);
}
int main(int argc, char **argv)
{
    static const Order orders[] = {
        {8, false, false}, {8, true, false}, {8, false, true}, {8, true, true},
        {16, false, false}, {16, true, false}, {16, false, true}, {16, true, true},
    };
    g_test_init(&argc, &argv, NULL);
    for (unsigned i = 0; i < G_N_ELEMENTS(orders); ++i) {
        g_autofree char *name = g_strdup_printf("/esp32s3/lcd-cam/i80/bus%u/reverse%u-swap%u",
            orders[i].bits, orders[i].reverse, orders[i].swap);
        qtest_add_data_func(name, &orders[i], test_i80_order);
    }
    qtest_add_func("/esp32s3/lcd-cam/i80/command-wire-hold-vs-gdma-eof", test_i80_wire_hold);
    qtest_add_func("/esp32s3/lcd-cam/i80/gate-reset-underflow", test_i80_gate_reset_underflow);
    qtest_add_func("/esp32s3/lcd-cam/panels/immutable-bounded-qom-windows", test_immutable_bounded_capture);
    for (unsigned fault = 1; fault <= 4; ++fault) {
        g_autofree char *name = g_strdup_printf("/esp32s3/lcd-cam/i80/physical-wire-fault%u", fault);
        qtest_add_data_func(name, GUINT_TO_POINTER(fault), test_panel_wiring);
    }
    qtest_add_data_func("/esp32s3/lcd-cam/rgb/positive-porches-divider-vsync", NULL, test_rgb);
    qtest_add_data_func("/esp32s3/lcd-cam/rgb/inverted-porches-divider-vsync", GUINT_TO_POINTER(1), test_rgb);
    qtest_add_func("/esp32s3/lcd-cam/rgb/starvation-real-dma-recovery", test_rgb_starvation);
    for (unsigned field = 0; field <= 2; ++field) {
        g_autofree char *name = g_strdup_printf("/esp32s3/lcd-cam/clock/divider-field-%u", field);
        qtest_add_data_func(name, GUINT_TO_POINTER(field), test_lcd_clock_divider);
    }
    qtest_add_data_func("/esp32s3/lcd-cam/phase/lcd-rising-delay", GUINT_TO_POINTER(1),
                        test_lcd_signal_delay);
    qtest_add_data_func("/esp32s3/lcd-cam/phase/lcd-falling-delay", GUINT_TO_POINTER(2),
                        test_lcd_signal_delay);
    qtest_add_func("/esp32s3/lcd-cam/cam-physical-loop/vsync-filter-official-hal-rearm",
                   test_cam_vsync_filter_and_rearm);
    qtest_add_func("/esp32s3/lcd-cam/cam-physical-loop/overflow-real-vsync-fulfilled-packet",
                   test_cam_overflow_closes_actual_packet);
    qtest_add_data_func("/esp32s3/lcd-cam/st7789/software-reset-ram-retention-c2", NULL,
                        test_st_reset_ram_and_c2);
    qtest_add_data_func("/esp32s3/lcd-cam/st7789/resx-ram-retention-c2", GUINT_TO_POINTER(1),
                        test_st_reset_ram_and_c2);
    qtest_add_data_func("/esp32s3/lcd-cam/st7789/colmod-rgb18-mcu16", GUINT_TO_POINTER(0x65),
                        test_st_independent_mcu_color_field);
    qtest_add_data_func("/esp32s3/lcd-cam/st7789/colmod-rgb16-mcu18", GUINT_TO_POINTER(0x56),
                        test_st_independent_mcu_color_field);
    for (unsigned flags = 0; flags < 32; ++flags) {
        g_autofree char *name = g_strdup_printf("/esp32s3/lcd-cam/cam-physical-loop/order-eof-polarity-%02u", flags);
        qtest_add_data_func(name, GUINT_TO_POINTER(flags), test_cam_orders);
    }
    qtest_add_func("/esp32s3/lcd-cam/cam-physical-loop/slow-consumer-overflow-owner-reset-short-frame",
                   test_cam_slow_reset_errors);
    qtest_add_data_func("/esp32s3/lcd-cam/cam-physical-loop/absent-data0", NULL, test_cam_absent_wire);
    qtest_add_data_func("/esp32s3/lcd-cam/cam-physical-loop/absent-pclk", GINT_TO_POINTER(16), test_cam_absent_wire);
    qtest_add_data_func("/esp32s3/lcd-cam/cam-physical-loop/absent-href", GINT_TO_POINTER(17), test_cam_absent_wire);
    qtest_add_data_func("/esp32s3/lcd-cam/cam-physical-loop/absent-vsync", GINT_TO_POINTER(18), test_cam_absent_wire);
    return g_test_run();
}
