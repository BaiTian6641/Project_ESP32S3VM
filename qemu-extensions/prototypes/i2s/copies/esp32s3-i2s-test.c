/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Register-boundary qualification, NOT ordinary ESP-IDF qualification.
 * Loopback-only: two real S3 controllers, four explicitly powered physical
 * nets, native GPIO matrix resolution and guest-RAM GDMA descriptors. No
 * injected IRQ, external audio peer, synthetic clock or injected sample.
 * Metadata cases additionally register a powered native sample peer whose DIN
 * taps the real wire and whose DOUT is electrically isolated. Read-only
 * attribution windows never supply payload values or a timing oracle.
 */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qemu/bitops.h"
#include "qemu/bswap.h"
#include "qapi/error.h"
#include "qapi/qmp/qjson.h"
#include "qapi/qmp/qlist.h"
#include "qapi/qmp/qnum.h"
#include "hw/gpio/esp32s3_gpio.h"
#include "hw/gpio/esp32s3_iomux.h"
#include "hw/misc/esp32s3_rtc_cntl.h"
#include "hw/xtensa/esp32s3_clk_defs.h"
#include "hw/misc/esp32s3_i2s.h"
#include "hw/misc/esp32s3_i2s_peer.h"

#define I2S(c) ((c) ? 0x6002d000ULL : 0x6000f000ULL)
#define GPIO(r) (0x60004000ULL + (r))
#define PAD(n) (0x60009000ULL + IO_MUX_GPIOn_REG_OFFSET(n))
#define IN(n) GPIO(GPIO_FUNC_IN_SEL_CFG_OFFSET(n))
#define OUT(n) GPIO(GPIO_FUNC_OUT_SEL_CFG_OFFSET(n))
#define INPUT (R_IO_MUX_GPIOn_FUN_IE_MASK | \
               (ESP32S3_IOMUX_MCU_SEL_GPIO << R_IO_MUX_GPIOn_MCU_SEL_SHIFT))
#define SYS_EN 0x600c0018ULL
#define SYS_RST 0x600c0020ULL
#define SYS_CLOCK (0x600c0000ULL + A_SYSTEM_SYSCLK_CONF)
#define RTC_OPTIONS (0x60008000ULL + A_RTC_CNTL_OPTIONS0)
#define GATE(c) BIT((c) ? 21 : 4)
#define DMA(ch, out, r) (0x6003f000ULL + (ch) * 0xc0 + (out) * 0x60 + (r))
#define SRC(c) (0x3fc90000U + (c) * 0x2000)
#define DST(c) (0x3fc94000U + (c) * 0x2000)
#define TD(c) (0x3fc98000U + (c) * 0x100)
#define RD(c) (0x3fc98400U + (c) * 0x100)
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
#define RX_TDM 0x50
#define TX_TDM 0x54
#define HUNG 0x60
#define EOF_NUM 0x64
#define SINGLE 0x68
#define STATE 0x6c
#define DATE 0x80
#define START BIT(2)
#define SLAVE BIT(3)
#define UPDATE BIT(8)
#define BYPASS BIT(12)
#define TDM BIT(19)
#define PDM BIT(20)
#define CLOCK_ON (BIT(26) | BIT(29))
#define DESC(size, len, eof) ((size) | ((len) << 12) | BIT(31) | ((eof) ? BIT(30) : 0))

static const unsigned pads[] = {18, 4, 19, 5, 20, 7, 6, 21};
static const unsigned tx_data[] = {25, 30};
static const unsigned rx_bclk[] = {26, 31};
static const unsigned rx_ws[] = {27, 32};
static const uint16_t master_raw_words[] = {0x5aa5, 0xa55a};

typedef struct Format {
    unsigned width, slot, slots, mask, ws_width;
    bool philips, mono, packed24, left_align, big_endian, lsb;
    bool slave_loop, rx_master;
    bool skip;
} Format;

typedef struct Fixture {
    QTestState *q;
    char *directory, *log;
    int64_t now;
    uint64_t solved_ns;
    bool metadata_peer, metadata_raw, metadata_tdm, metadata_master;
    unsigned metadata_controller, metadata_width, metadata_capacity;
    unsigned metadata_slots, metadata_mask, metadata_ws_width;
} Fixture;

typedef struct Event {
    uint64_t ns, sequence, frame;
    uint32_t sample;
    uint16_t slot;
    uint8_t width, flags;
} Event;

static uint32_t rd(Fixture *f, unsigned c, unsigned reg)
{
    return qtest_readl(f->q, I2S(c) + reg);
}

static void wr(Fixture *f, unsigned c, unsigned reg, uint32_t value)
{
    qtest_writel(f->q, I2S(c) + reg, value);
}

static void step(Fixture *f, int64_t ns)
{
    f->now = qtest_clock_step(f->q, ns);
}

static void assert_running(Fixture *f, bool expected)
{
    QDict *reply = qtest_qmp(f->q, "{'execute':'query-status'}");
    g_assert_false(qdict_haskey(reply, "error"));
    g_assert_cmpint(qdict_get_bool(qdict_get_qdict(reply, "return"), "running"),
                    ==, expected);
    qobject_unref(reply);
}

static QDict *object(const char *json)
{
    QObject *o = qobject_from_json(json, &error_abort);
    g_assert_nonnull(qobject_to(QDict, o));
    return qobject_to(QDict, o);
}

static QDict *component(QDict *p, const char *id, const char *kind)
{
    QDict *c = qdict_new();
    qdict_put_str(c, "id", id);
    qdict_put_str(c, "name", id);
    qdict_put_str(c, "type", kind);
    qdict_put_str(c, "kind", kind);
    qdict_put(c, "parameters", qdict_new());
    qdict_put(c, "terminals", qlist_new());
    qlist_append(qdict_get_qlist(p, "components"), c);
    return c;
}

static QDict *terminal(QDict *c, const char *id, const char *role,
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
    return t;
}

static void quantity(QDict *c, const char *name, double value, const char *unit)
{
    QDict *q = qdict_new();
    qdict_put(q, "value", qnum_from_double(value));
    qdict_put_str(q, "unit", unit);
    qdict_put(qdict_get_qdict(c, "parameters"), name, q);
}

static void net(QDict *p, const char *id, const char *const *endpoints)
{
    const QListEntry *e;
    QLIST_FOREACH_ENTRY(qdict_get_qlist(p, "nets"), e) {
        QDict *existing = qobject_to(QDict, qlist_entry_obj(e));
        if (!strcmp(qdict_get_str(existing, "id"), id)) {
            for (; *endpoints; ++endpoints) {
                qlist_append_str(qdict_get_qlist(existing, "endpoints"), *endpoints);
            }
            return;
        }
    }
    QDict *n = qdict_new();
    QList *ends = qlist_new();
    qdict_put_str(n, "id", id);
    qdict_put_str(n, "name", id);
    for (; *endpoints; ++endpoints) {
        qlist_append_str(ends, *endpoints);
    }
    qdict_put(n, "endpoints", ends);
    qlist_append(qdict_get_qlist(p, "nets"), n);
}
#define NET(p, id, ...) net(p, id, (const char *const[]){__VA_ARGS__, NULL})

static void apply_project(Fixture *f, bool clocks, bool source_pull,
                          bool source_high)
{
    QDict *p = object("{\"version\":3,\"id\":\"i2s-loopback-only\","
        "\"name\":\"I2S physical loopback only\","
        "\"profile\":{\"chip\":\"esp32s3\",\"board\":\"esp32-s3-devkitc-1\","
        "\"module\":\"esp32-s3-wroom-1\"},\"firmware\":{},"
        "\"runtime\":{\"electrical\":{\"driver_profile\":\"s3-explicit-finite-v1\",\"mode\":\"dc\"}},"
        "\"geometry\":{\"components\":{},\"nets\":{}},\"components\":[],\"nets\":[]}");
    QDict *c = component(p, "U1", "mcu");
    terminal(c, "U1.vdd", "vdd", "power", -1);
    terminal(c, "U1.gnd", "gnd", "ground", -1);
    for (unsigned i = 0; i < G_N_ELEMENTS(pads); ++i) {
        g_autofree char *id = g_strdup_printf("U1.io%u", pads[i]);
        g_autofree char *role = g_strdup_printf("io%u", pads[i]);
        terminal(c, id, role, "digital", pads[i]);
    }
    if (clocks) {
        for (unsigned pad = 8; pad <= 11; ++pad) {
            g_autofree char *id = g_strdup_printf("U1.io%u", pad);
            g_autofree char *role = g_strdup_printf("io%u", pad);
            terminal(c, id, role, "digital", pad);
        }
    }
    if (f->metadata_peer) {
        terminal(c, "U1.io17", "io17", "digital", 17);
    }
    c = component(p, "G", "ground");
    terminal(c, "G.ref", "ref", "ground", -1);
    c = component(p, "V", "voltage-source");
    terminal(c, "V.p", "p", "power", -1);
    terminal(c, "V.n", "n", "ground", -1);
    quantity(c, "voltage", 3.3, "V");
    for (unsigned i = 0; i < (clocks ? 6 : 4); ++i) {
        if (i == 4 && !source_pull) {
            continue;
        }
        g_autofree char *id = g_strdup_printf("R%u", i);
        g_autofree char *a = g_strdup_printf("R%u.a", i);
        g_autofree char *b = g_strdup_printf("R%u.b", i);
        c = component(p, id, "resistor");
        terminal(c, a, "a", "passive", -1);
        terminal(c, b, "b", "passive", -1);
        quantity(c, "resistance", 100000, "ohm");
    }
    if (clocks && source_pull && source_high) {
        NET(p, "vdd", "V.p", "U1.vdd", "R4.b");
    } else {
        NET(p, "vdd", "V.p", "U1.vdd");
    }
    if (clocks && source_pull && !source_high) {
        NET(p, "gnd", "G.ref", "V.n", "U1.gnd", "R2.b", "R3.b", "R4.b", "R5.b");
    } else if (clocks) {
        NET(p, "gnd", "G.ref", "V.n", "U1.gnd", "R2.b", "R3.b", "R5.b");
    } else {
        NET(p, "gnd", "G.ref", "V.n", "U1.gnd", "R2.b", "R3.b");
    }
    if (f->metadata_peer) {
        /* Passive high idle establishes a genuine falling setup edge at
         * native master activation. The registered slave does not invent
         * a startup edge when the initial clock was already held low. */
        NET(p, "vdd", "R0.b", "R1.b");
    } else {
        NET(p, "gnd", "R0.b", "R1.b");
    }
    NET(p, "bclk", "U1.io18", "U1.io4", "R0.a");
    NET(p, "ws", "U1.io19", "U1.io5", "R1.a");
    NET(p, "tx0-rx1", "U1.io20", "U1.io7", "R2.a");
    NET(p, "tx1-rx0", "U1.io6", "U1.io21", "R3.a");
    if (clocks) {
        if (source_pull) {
            NET(p, "mclk1-source", "U1.io8", "U1.io9", "R4.a");
        } else {
            NET(p, "mclk1-source", "U1.io8", "U1.io9");
        }
        NET(p, "mclk0-output", "U1.io10", "U1.io11", "R5.a");
    }
    if (f->metadata_peer) {
        QDict *peer = component(p, "P", "i2s-sample-peer");
        qdict_put_str(peer, "kind", "device");
        static const char *const roles[] = {"vdd", "gnd", "bclk", "ws", "din", "dout"};
        static const char *const ids[] = {"P.vdd", "P.gnd", "P.bclk", "P.ws", "P.din", "P.dout"};
        for (unsigned i = 0; i < G_N_ELEMENTS(roles); ++i) {
            QDict *t = terminal(peer, ids[i], roles[i],
                                i == 0 ? "power" : i == 1 ? "ground" : "digital", -1);
            bool output = i == 5 || (f->metadata_master && (i == 2 || i == 3));
            qdict_put_str(t, "direction", output ? "output" : "input");
        }
        QDict *parameters = object("{\"role\":\"slave\",\"format\":\"msb\","
            "\"dataBits\":16,\"slotBits\":16,\"slots\":2,\"slotMask\":3,"
            "\"wsWidth\":16,\"wsPol\":false,\"bitShift\":false,"
            "\"leftAlign\":false,\"lsbFirst\":false,"
            "\"sampleRateNumeratorHz\":156250,\"sampleRateDenominator\":1,"
            "\"repeat\":true,\"captureCapacity\":64}");
        qdict_put_int(parameters, "dataBits", f->metadata_width);
        qdict_put_int(parameters, "slotBits", f->metadata_width);
        qdict_put_int(parameters, "slots", f->metadata_slots);
        qdict_put_int(parameters, "slotMask", f->metadata_mask);
        qdict_put_int(parameters, "wsWidth", f->metadata_raw ? 1 : f->metadata_ws_width);
        if (f->metadata_tdm) {
            qdict_put_str(parameters, "format", "tdm");
        }
        qdict_put_int(parameters, "captureCapacity", f->metadata_capacity);
        qdict_put_int(parameters, "sampleRateNumeratorHz", 5000000);
        qdict_put_int(parameters, "sampleRateDenominator",
                      f->metadata_slots * f->metadata_width);
        if (f->metadata_master) {
            qdict_put_str(parameters, "role", "master");
        }
        QList *vector = qlist_new();
        if (f->metadata_raw) {
            qdict_put_str(parameters, "format", "raw-pdm");
            for (unsigned i = 0; i < 32; ++i) {
                unsigned bit = f->metadata_master ?
                    (master_raw_words[i % 2] >> (15 - i / 2)) & 1 : i % 2;
                qlist_append(vector, qnum_from_uint(bit));
            }
            qdict_put(parameters, "rawBits", vector);
        } else {
            qlist_append(vector, qnum_from_uint(0x5a));
            qlist_append(vector, qnum_from_uint(0xa5));
            qdict_put(parameters, "txSamples", vector);
        }
        QDict *attributes = qdict_new();
        qdict_put(attributes, "native_i2s_peer", parameters);
        qdict_put(peer, "attributes", attributes);
        QDict *bias = component(p, "RP", "resistor");
        terminal(bias, "RP.a", "a", "passive", -1);
        terminal(bias, "RP.b", "b", "passive", -1);
        quantity(bias, "resistance", 10000, "ohm");
        NET(p, "vdd", "P.vdd");
        NET(p, "gnd", "P.gnd", "RP.b");
        NET(p, "bclk", "P.bclk");
        NET(p, "ws", "P.ws");
        if (f->metadata_controller) {
            NET(p, "tx1-rx0", "P.din");
        } else {
            NET(p, "tx0-rx1", "P.din");
        }
        NET(p, "peer-isolated-dout", "P.dout", "RP.a", "U1.io17");
    }
    /* Native ProjectDocument Apply is a paused quiescent transaction.
     * Later topology fixtures preserve the pre-existing run state without
     * advancing virtual time, resetting the shifters, or rewriting clocks. */
    QDict *status = qtest_qmp(f->q, "{'execute':'query-status'}");
    g_assert_false(qdict_haskey(status, "error"));
    bool running = qdict_get_bool(qdict_get_qdict(status, "return"), "running");
    qobject_unref(status);
    if (running) {
        qtest_qmp_assert_success(f->q, "{'execute':'stop'}");
        qtest_qmp_eventwait(f->q, "STOP");
    }
    GString *json = qobject_to_json(QOBJECT(p));
    QDict *reply = qtest_qmp(f->q, "{'execute':'qom-set','arguments':"
        "{'path':'/machine/soc/electrical','property':'project-json','value':%s}}", json->str);
    if (qdict_haskey(reply, "error")) {
        g_test_message("Native ProjectDocument Apply failed: %s",
                       qdict_get_str(qdict_get_qdict(reply, "error"), "desc"));
    }
    g_assert_false(qdict_haskey(reply, "error"));
    qobject_unref(reply);
    g_string_free(json, true);
    qobject_unref(p);
    if (running) {
        qtest_qmp_assert_success(f->q, "{'execute':'cont'}");
        qtest_qmp_eventwait(f->q, "RESUME");
    }
}

/* Snapshot evidence is the solved physical net, never controller state. */
static bool level(Fixture *f, unsigned gpio)
{
    QDict *reply = qtest_qmp(f->q, "{'execute':'qom-get','arguments':"
        "{'path':'/machine/soc/electrical','property':'snapshot-json'}}");
    g_assert_false(qdict_haskey(reply, "error"));
    QDict *snap = object(qdict_get_str(reply, "return"));
    const char *timestamp = qdict_get_str(snap, "timestamp_ns");
    f->solved_ns = g_ascii_strtoull(timestamp, NULL, 10);
    g_assert_cmpuint(f->solved_ns, <=, f->now);
    bool found = false, result = false;
    const QListEntry *e;
    QLIST_FOREACH_ENTRY(qdict_get_qlist(snap, "pads"), e) {
        QDict *p = qobject_to(QDict, qlist_entry_obj(e));
        if (qdict_get_int(p, "gpio") != gpio) {
            continue;
        }
        QNum *v = qobject_to(QNum, qdict_get(p, "voltage_v"));
        g_assert_nonnull(v);
        g_assert_true(qdict_get_bool(p, "valid"));
        g_assert_true(qdict_get_bool(p, "digital_valid"));
        double volts = qnum_get_double(v);
        g_assert_true(volts < 0.1 || volts > 3.0);
        result = volts > 3.0;
        found = true;
        break;
    }
    g_assert_true(found);
    g_assert_cmpint(!!(qtest_readl(f->q, GPIO(A_GPIO_IN)) & BIT(gpio)), ==, result);
    qobject_unref(snap);
    qobject_unref(reply);
    return result;
}

static void pll_power(Fixture *f, bool on)
{
    uint32_t options = qtest_readl(f->q, RTC_OPTIONS);
    options &= ~(R_RTC_CNTL_OPTIONS0_SW_SYS_RESET_MASK |
                 R_RTC_CNTL_OPTIONS0_BBPLL_FORCE_PD_MASK);
    if (!on) {
        options &= ~R_RTC_CNTL_OPTIONS0_BBPLL_FORCE_PU_MASK;
        options |= R_RTC_CNTL_OPTIONS0_BBPLL_FORCE_PD_MASK;
    }
    qtest_writel(f->q, RTC_OPTIONS, options);
}

static void gate(Fixture *f, unsigned c, bool enabled)
{
    uint32_t value = qtest_readl(f->q, SYS_EN);
    qtest_writel(f->q, SYS_EN, enabled ? value | GATE(c) : value & ~GATE(c));
}

static Fixture start(void)
{
    Fixture f = {0};
    GError *error = NULL;
    f.directory = g_dir_make_tmp("s3-i2s-qtest-XXXXXX", &error);
    g_assert_no_error(error);
    f.log = g_build_filename(f.directory, "guest-errors.log", NULL);
    f.q = qtest_initf("-machine esp32s3 -S -L pc-bios "
        "-global driver=esp32s3.gpio,property=strap_mode,value=0x00 "
        "-global driver=esp32s3-i2s,property=record-directory,value=%s "
        "-d guest_errors -D %s", f.directory, f.log);
    gate(&f, 0, true);
    gate(&f, 1, true);
    for (unsigned i = 0; i < G_N_ELEMENTS(pads); ++i) {
        qtest_writel(f.q, PAD(pads[i]), INPUT);
    }
    qtest_writel(f.q, OUT(18), 22);
    qtest_writel(f.q, OUT(19), 24);
    qtest_writel(f.q, OUT(20), 25);
    qtest_writel(f.q, OUT(6), 30);
    qtest_writel(f.q, GPIO(A_GPIO_ENABLE_W1TS), BIT(18) | BIT(19) | BIT(20) | BIT(6));
    for (unsigned c = 0; c < 2; ++c) {
        qtest_writel(f.q, IN(rx_bclk[c]), 4 | BIT(7));
        qtest_writel(f.q, IN(rx_ws[c]), 5 | BIT(7));
        qtest_writel(f.q, IN(tx_data[c]), (c ? 7 : 21) | BIT(7));
    }
    qtest_writel(f.q, IN(28), 4 | BIT(7));
    qtest_writel(f.q, IN(29), 5 | BIT(7));
    apply_project(&f, false, false, false);
    /* Qtest CONT runs dummy vCPUs, never guest firmware. It enables the
     * native QEMU virtual timers; -S alone leaves clock_step unable to run
     * timer deadlines. Strict dependencies can now genuinely pause the VM. */
    qtest_qmp_assert_success(f.q, "{'execute':'cont'}");
    qtest_qmp_eventwait(f.q, "RESUME");
    step(&f, 0);
    return f;
}

static GArray *events(Fixture *f, unsigned c)
{
    g_autofree char *name = g_strdup_printf("i2s%u.rx.bin", c);
    g_autofree char *path = g_build_filename(f->directory, name, NULL);
    g_autofree char *bytes = NULL;
    gsize size;
    GError *error = NULL;
    g_assert_true(g_file_get_contents(path, &bytes, &size, &error));
    g_assert_no_error(error);
    g_assert_cmpuint(size, >=, 16);
    g_assert_cmpmem(bytes, 8, "S3I2SRX1", 8);
    g_assert_cmpuint(ldl_le_p(bytes + 8), ==, c);
    g_assert_cmpuint(ldl_le_p(bytes + 12), ==, 1);
    g_assert_cmpuint((size - 16) % 32, ==, 0);
    GArray *a = g_array_new(false, false, sizeof(Event));
    for (gsize offset = 16; offset < size; offset += 32) {
        const uint8_t *b = (const uint8_t *)bytes + offset;
        Event event = {ldq_le_p(b), ldq_le_p(b + 8), ldq_le_p(b + 16),
                       ldl_le_p(b + 24), lduw_le_p(b + 28), b[30], b[31]};
        g_assert_cmpuint(event.sequence, ==, a->len);
        if (a->len) {
            g_assert_cmpuint(event.ns, >=, g_array_index(a, Event, a->len - 1).ns);
        }
        g_array_append_val(a, event);
    }
    return a;
}

static void cleanup(Fixture *f)
{
    for (unsigned c = 0; c < 2; ++c) {
        g_autofree char *name = g_strdup_printf("i2s%u.rx.bin", c);
        g_autofree char *path = g_build_filename(f->directory, name, NULL);
        g_assert_cmpint(unlink(path), ==, 0);
    }
    g_assert_cmpint(unlink(f->log), ==, 0);
    g_assert_cmpint(rmdir(f->directory), ==, 0);
    g_free(f->log);
    g_free(f->directory);
}

static void descriptor(Fixture *f, uint32_t address, unsigned size,
                       unsigned length, bool eof, uint32_t buffer, uint32_t next)
{
    uint8_t d[12];
    stl_le_p(d, DESC(size, length, eof));
    stl_le_p(d + 4, buffer);
    stl_le_p(d + 8, next);
    qtest_memwrite(f->q, address, d, sizeof(d));
}

static void dma_start(Fixture *f, unsigned c, bool out, uint32_t address)
{
    qtest_writel(f->q, DMA(c, out, 0x00), out ? BIT(2) : 0);
    qtest_writel(f->q, DMA(c, out, 0x04), BIT(12));
    qtest_writel(f->q, DMA(c, out, 0x48), c + 3);
    qtest_writel(f->q, DMA(c, out, 0x10), 0x1f);
    qtest_writel(f->q, DMA(c, out, 0x20), (address & 0xfffff) | BIT(out ? 21 : 22));
}

static unsigned bytes_per_sample(const Format *v)
{
    return v->width == 24 && v->packed24 ? 4 : (v->width + 7) / 8;
}

static uint32_t word(unsigned c, unsigned index, unsigned width)
{
    /* Signed endpoints, walking patterns and asymmetric channels. */
    static const uint32_t vector[] = {0x80ff017e, 0x7f00fe81, 0xffffffff,
        0x80000000, 0x13579bdf, 0x2468ace0, 0x00000001, 0xaaaaaaaa};
    uint32_t value = vector[(index + c * 3) % G_N_ELEMENTS(vector)];
    return width == 32 ? value : value & ((1U << width) - 1);
}

static void source(Fixture *f, unsigned c, const Format *v, unsigned count)
{
    uint8_t data[2048];
    unsigned n = bytes_per_sample(v);
    g_assert_cmpuint(count * n, <=, sizeof(data));
    for (unsigned i = 0; i < count; ++i) {
        uint32_t w = word(c, i, n * 8);
        for (unsigned b = 0; b < n; ++b) {
            data[i * n + b] = w >> (8 * (v->big_endian ? n - b - 1 : b));
        }
    }
    qtest_memwrite(f->q, SRC(c), data, count * n);
    descriptor(f, TD(c), count * n, count * n, true, SRC(c), 0);
    dma_start(f, c, true, TD(c));
}

static uint32_t conf(const Format *v)
{
    return BYPASS | TDM | (v->mono ? BIT(5) | BIT(9) : 0) |
           (v->packed24 ? BIT(16) : 0) | (v->left_align ? BIT(15) : 0) |
           (v->big_endian ? BIT(7) : 0) | (v->lsb ? BIT(18) : 0);
}

static void configure(Fixture *f, unsigned c, const Format *v)
{
    unsigned frame = v->slot * v->slots;
    uint32_t conf1 = (v->ws_width - 1) | ((v->width - 1) << 13) |
        ((frame / 2 - 1) << 18) | ((v->slot - 1) << 24) |
        (v->philips ? BIT(29) : 0);
    wr(f, c, TX_CONF, BIT(0));
    wr(f, c, RX_CONF, BIT(0));
    wr(f, c, TX_CONF1, conf1);
    wr(f, c, RX_CONF1, conf1);
    wr(f, c, TX_TDM, v->mask | ((v->slots - 1) << 16) | (v->skip ? BIT(20) : 0));
    wr(f, c, RX_TDM, v->mask | ((v->slots - 1) << 16));
    wr(f, c, SINGLE, 0x5a);
    wr(f, c, TX_CLK, CLOCK_ON | 8); /* Real XTAL40M / 8: 100ns half-edge. */
    wr(f, c, RX_CLK, CLOCK_ON | 8);
    wr(f, c, TX_CONF, conf(v) | (c ? SLAVE : 0));
    wr(f, c, RX_CONF, conf(v) | SLAVE);
}

static void receiver(Fixture *f, unsigned c, unsigned size)
{
    qtest_memset(f->q, DST(c), 0xcd, size);
    descriptor(f, RD(c), size, 0, false, DST(c), 0);
    dma_start(f, c, false, RD(c));
}

static void launch(Fixture *f, const Format *v, bool duplex)
{
    wr(f, 1, RX_CONF, conf(v) | SLAVE | START);
    if (v->rx_master) {
        g_assert_true(duplex);
        wr(f, 1, TX_CONF, conf(v) | SLAVE | BIT(27) | START);
        wr(f, 0, TX_CONF, conf(v) | SLAVE | BIT(27) | START);
        wr(f, 0, RX_CONF, conf(v) | START);
    } else {
        if (duplex) {
            wr(f, 0, RX_CONF, conf(v) | SLAVE | START);
            wr(f, 1, TX_CONF, conf(v) | SLAVE | START |
               (v->slave_loop ? BIT(27) : 0));
        }
        wr(f, 0, TX_CONF, conf(v) | START);
    }
}

static unsigned active_before(const Format *v, unsigned slot)
{
    return ctpop32(v->mask & ((1U << slot) - 1));
}

static uint32_t serial_word(const Format *v, unsigned c, unsigned frame,
                            unsigned slot)
{
    if (!(v->mask & BIT(slot))) {
        return 0x5a;
    }
    unsigned valid_bits = bytes_per_sample(v) * 8;
    unsigned index = frame * (v->skip ? v->slots : ctpop32(v->mask)) +
                     (v->skip ? slot : active_before(v, slot));
    uint32_t w = word(c, index, valid_bits);
    return v->left_align ? w << (v->slot - valid_bits) : w;
}

static void check_events(GArray *a, const Format *v, unsigned source_controller,
                         unsigned frames, uint64_t origin)
{
    unsigned expected = frames * (v->mono ? 1 : ctpop32(v->mask));
    g_assert_cmpuint(a->len, ==, expected);
    unsigned index = 0;
    for (unsigned frame = 0; frame < frames; ++frame) {
        for (unsigned slot = 0; slot < v->slots; ++slot) {
            if (!(v->mask & BIT(slot)) || (v->mono && slot != 0)) {
                continue;
            }
            Event e = g_array_index(a, Event, index++);
            uint32_t expected_word = serial_word(v, source_controller, frame, slot);
            unsigned storage = bytes_per_sample(v) * 8;
            if (v->left_align && v->slot > storage) {
                expected_word >>= v->slot - storage;
            }
            g_assert_cmphex(e.sample, ==, expected_word);
            g_assert_cmpuint(e.slot, ==, slot);
            g_assert_cmpuint(e.width, ==, storage);
            g_assert_cmpuint(e.flags, ==, 3);
            g_assert_cmpuint(e.frame, ==, frame + (v->philips && slot + 1 == v->slots));
            unsigned final_bit = frame * v->slot * v->slots + (slot + 1) * v->slot - 1 + v->philips;
            g_assert_cmpuint(e.ns, ==, origin + final_bit * 200ULL + 100);
        }
    }
}

static void test_vectors(gconstpointer opaque)
{
    const Format *v = opaque;
    Fixture f = start();
    for (unsigned c = 0; c < 2; ++c) {
        configure(&f, c, v);
        source(&f, c, v, 32);
        receiver(&f, c, 512);
    }
    if (v->rx_master) {
        qtest_writel(f.q, OUT(18), 26);
        qtest_writel(f.q, OUT(19), 27);
    }
    unsigned framebits = v->slot * v->slots;
    uint64_t origin = f.now;
    launch(&f, v, true);
    /* Every bit has independently checked physical BCLK/WS/SD on both
     * ends. Stop after exactly four complete frames, including Philips delay. */
    unsigned total = 4 * framebits + v->philips;
    for (unsigned bit = 0; bit < total; ++bit) {
        unsigned logical = (bit + framebits - v->philips) % framebits;
        unsigned frame = bit >= v->philips ? (bit - v->philips) / framebits : 0;
        unsigned slot = logical / v->slot;
        unsigned lane_bit = logical % v->slot;
        for (unsigned c = 0; c < 2; ++c) {
            bool expected = false;
            if (bit >= v->philips) {
                uint32_t w = serial_word(v, c, frame, slot);
                unsigned shift = v->lsb ? lane_bit : v->slot - lane_bit - 1;
                expected = !!(w & BIT(shift));
            }
            g_assert_cmpint(level(&f, c ? 21 : 7), ==, expected);
        }
        g_assert_false(level(&f, 4));
        g_assert_cmpint(level(&f, 5), ==, bit % framebits >= v->ws_width);
        step(&f, 99);
        g_assert_false(level(&f, 4));
        step(&f, 1);
        g_assert_true(level(&f, 4));
        g_assert_cmpuint(f.solved_ns, ==, f.now);
        if (bit + 1 < total) {
            step(&f, 100);
        }
    }
    for (unsigned c = 0; c < 2; ++c) {
        wr(&f, c, RX_CONF, conf(v) | SLAVE);
        wr(&f, c, TX_CONF, conf(v) | (c ? SLAVE : 0));
    }
    qtest_quit(f.q);
    for (unsigned c = 0; c < 2; ++c) {
        GArray *a = events(&f, c);
        check_events(a, v, !c, 4, origin);
        g_array_unref(a);
    }
    cleanup(&f);
}

static const Format standard = {16, 16, 2, 3, 16};

static void test_descriptor_eof_continue(void)
{
    Fixture f = start();
    qtest_irq_intercept_out_named(f.q, "/machine/soc/gdma", "CHAN_IN");
    configure(&f, 0, &standard);
    configure(&f, 1, &standard);
    /* TX byte boundaries deliberately split words; actual demand crosses
     * four real TX descriptors and an RX ring. EOF=6 ends partial 16B buffers. */
    uint8_t data[128];
    for (unsigned i = 0; i < 64; ++i) {
        stw_le_p(data + 2 * i, word(0, i, 16));
    }
    qtest_memwrite(f.q, SRC(0), data, sizeof(data));
    static const unsigned lengths[] = {5, 7, 12, 104};
    unsigned offset = 0;
    for (unsigned i = 0; i < 4; ++i) {
        descriptor(&f, TD(0) + 12 * i, lengths[i], lengths[i], i == 3,
                   SRC(0) + offset, i == 3 ? 0 : TD(0) + 12 * (i + 1));
        offset += lengths[i];
        descriptor(&f, RD(1) + 12 * i, 16, 0, false, DST(1) + 16 * i,
                   i == 3 ? RD(1) : RD(1) + 12 * (i + 1));
    }
    qtest_memset(f.q, DST(1), 0xcd, 64);
    dma_start(&f, 0, true, TD(0));
    dma_start(&f, 1, false, RD(1));
    wr(&f, 1, EOF_NUM, 6);
    launch(&f, &standard, false);
    step(&f, 16 * 9 * 200 - 100); /* Nine real 16-bit samples. */
    for (unsigned i = 0; i < 3; ++i) {
        uint32_t d = qtest_readl(f.q, RD(1) + 12 * i);
        g_assert_cmphex(d & BIT(31), ==, 0);
        g_assert_cmpuint((d >> 12) & 0xfff, ==, 6);
        g_assert_cmphex(d & BIT(30), ==, BIT(30));
        uint8_t actual[16];
        qtest_memread(f.q, DST(1) + 16 * i, actual, sizeof(actual));
        g_assert_cmpmem(actual, 6, data + 6 * i, 6);
        for (unsigned b = 6; b < 16; ++b) {
            g_assert_cmphex(actual[b], ==, 0xcd);
        }
    }
    /* Recycle before hardware prefetches the next ring allocation. */
    descriptor(&f, RD(1), 16, 0, false, DST(1), RD(1) + 12);
    step(&f, 3 * 16 * 200);
    uint32_t last = qtest_readl(f.q, RD(1) + 36);
    g_assert_cmphex(last & BIT(31), ==, 0);
    g_assert_cmpuint((last >> 12) & 0xfff, ==, 6);
    g_assert_cmphex(last & BIT(30), ==, BIT(30));
    uint8_t last_data[16];
    qtest_memread(f.q, DST(1) + 48, last_data, sizeof(last_data));
    g_assert_cmpmem(last_data, 6, data + 18, 6);
    for (unsigned b = 6; b < 16; ++b) {
        g_assert_cmphex(last_data[b], ==, 0xcd);
    }
    g_assert_cmphex(qtest_readl(f.q, DMA(1, 0, 0x28)), ==, RD(1) + 36);
    g_assert_cmphex(qtest_readl(f.q, DMA(1, 0, 0x34)), ==, RD(1));
    g_assert_cmphex(qtest_readl(f.q, DMA(1, 0, 0x30)), ==, RD(1) + 12);
    g_assert_cmphex(qtest_readl(f.q, DMA(1, 0, 0x08)) & 3, ==, 3);
    g_assert_cmphex(qtest_readl(f.q, DMA(1, 0, 0x0c)) & 3, ==, 3);
    g_assert_true(qtest_get_irq(f.q, 1));
    for (unsigned i = 0; i < 4; ++i) {
        g_assert_cmphex(qtest_readl(f.q, TD(0) + 12 * i) & BIT(31), ==, 0);
    }
    /* No LINK.START: the prefetched cursor survives the physical EOF. */
    descriptor(&f, RD(1) + 12, 16, 0, false, DST(1) + 16, RD(1) + 24);
    qtest_writel(f.q, DMA(1, 0, 0x14), 0x1f);
    g_assert_false(qtest_get_irq(f.q, 1));
    step(&f, 3 * 16 * 200);
    uint8_t next[6];
    qtest_memread(f.q, DST(1), next, sizeof(next));
    g_assert_cmpmem(next, sizeof(next), data + 24, sizeof(next));
    g_assert_cmpuint((qtest_readl(f.q, RD(1)) >> 12) & 0xfff, ==, 6);
    g_assert_cmphex(qtest_readl(f.q, DMA(1, 0, 0x28)), ==, RD(1));
    qtest_quit(f.q);
    GArray *a = events(&f, 1);
    g_assert_cmpuint(a->len, ==, 15);
    g_array_unref(a);
    cleanup(&f);
}

static void test_underflow_full_frame(void)
{
    Fixture f = start();
    qtest_irq_intercept_out_named(f.q, "/machine/soc/i2s0", SYSBUS_DEVICE_GPIO_IRQ);
    configure(&f, 0, &standard);
    configure(&f, 1, &standard);
    source(&f, 0, &standard, 4); /* Two asymmetric stereo frames only. */
    receiver(&f, 1, 256);
    wr(&f, 0, HUNG, BIT(11) | (7U << 8) | 1);
    wr(&f, 0, ENA, BIT(3));
    launch(&f, &standard, false);
    step(&f, 12 * 32 * 200 - 100);
    g_assert_cmphex(rd(&f, 0, RAW) & BIT(3), ==, BIT(3));
    g_assert_cmphex(rd(&f, 0, ST) & BIT(3), ==, BIT(3));
    g_assert_true(qtest_get_irq(f.q, 0));
    wr(&f, 0, CLR, BIT(3));
    g_assert_cmphex(rd(&f, 0, ST), ==, 0);
    g_assert_false(qtest_get_irq(f.q, 0));
    qtest_quit(f.q);
    GArray *a = events(&f, 1);
    g_assert_cmpuint(a->len, ==, 24);
    for (unsigned i = 0; i < a->len; ++i) {
        Event e = g_array_index(a, Event, i);
        unsigned index = i < 4 ? i : 2 + i % 2;
        g_assert_cmphex(e.sample, ==, word(0, index, 16));
        g_assert_cmpuint(e.slot, ==, i % 2);
        g_assert_cmpuint(e.flags, ==, 3);
    }
    g_array_unref(a);
    cleanup(&f);
}

static void test_real_fifo_overflow(void)
{
    Fixture f = start();
    qtest_irq_intercept_out_named(f.q, "/machine/soc/i2s1", SYSBUS_DEVICE_GPIO_IRQ);
    configure(&f, 0, &standard);
    configure(&f, 1, &standard);
    source(&f, 0, &standard, 256);
    /* Deliberately no RX GDMA: bounded hardware FIFO actually fills. */
    wr(&f, 1, HUNG, BIT(11) | (7U << 8) | 1);
    wr(&f, 1, ENA, BIT(2));
    launch(&f, &standard, false);
    step(&f, 176 * 16 * 200 - 100);
    g_assert_cmphex(rd(&f, 1, RAW) & BIT(2), ==, BIT(2));
    g_assert_cmphex(rd(&f, 1, ST) & BIT(2), ==, BIT(2));
    g_assert_true(qtest_get_irq(f.q, 0));
    wr(&f, 0, TX_CONF, conf(&standard));
    receiver(&f, 1, 256);
    /* A subsequent actual RX sample pumps the old FIFO, not a test hook. */
    wr(&f, 0, TX_CONF, conf(&standard) | START);
    step(&f, 32 * 200);
    uint8_t recovered[256];
    qtest_memread(f.q, DST(1), recovered, sizeof(recovered));
    for (unsigned i = 0; i < 128; ++i) {
        g_assert_cmphex(lduw_le_p(recovered + 2 * i), ==, word(0, i, 16));
    }
    qtest_quit(f.q);
    GArray *a = events(&f, 1);
    g_assert_cmpuint(a->len, >=, 176);
    for (unsigned i = 0; i < 176; ++i) {
        Event e = g_array_index(a, Event, i);
        g_assert_cmphex(e.sample, ==, word(0, i, 16));
        g_assert_cmpuint(e.flags, ==, i < 128 ? 3 : 5);
    }
    g_array_unref(a);
    cleanup(&f);
}

static uint64_t next_rise(Fixture *f, unsigned limit)
{
    bool previous = level(f, 4);
    for (unsigned i = 0; i < limit; ++i) {
        step(f, 1);
        bool current = level(f, 4);
        if (!previous && current) {
            return f->now;
        }
        previous = current;
    }
    g_assert_not_reached();
}

static void test_clock_gate_divider_lifecycle(void)
{
    Fixture f = start();
    configure(&f, 0, &standard);
    configure(&f, 1, &standard);
    source(&f, 0, &standard, 512);
    receiver(&f, 1, 1024);
    launch(&f, &standard, false);
    uint64_t first = next_rise(&f, 101);
    g_assert_cmpuint(first, ==, 100);
    g_assert_cmpuint(next_rise(&f, 201) - first, ==, 200);
    step(&f, 57); /* Update at a non-edge; observable period, not reg only. */
    uint64_t changed = f.now;
    wr(&f, 0, TX_CLK, CLOCK_ON | 16);
    g_assert_cmpuint(next_rise(&f, 401) - changed, ==, 400);
    first = f.now;
    g_assert_cmpuint(next_rise(&f, 401) - first, ==, 400);
    /* Fractional n=8+1/2 is represented by x=0,y=1,z=1. */
    wr(&f, 0, TX_DIV, 1 | (1U << 9));
    wr(&f, 0, TX_CLK, CLOCK_ON | 8);
    changed = f.now;
    first = next_rise(&f, 301);
    g_assert_cmpuint(first - changed, ==, 212);
    g_assert_cmpuint(next_rise(&f, 301) - first, ==, 213);
    first = f.now;
    g_assert_cmpuint(next_rise(&f, 301) - first, ==, 212);
    /* BBPLL is powered at reset even with the CPU on XTAL. Select its real
     * 160MHz module output: /8 gives 50ns BCLK, with no CPU-mux workaround. */
    g_assert_cmphex(qtest_readl(f.q, SYS_CLOCK) &
                    R_SYSTEM_SYSCLK_CONF_SOC_CLK_SEL_MASK, ==, 0);
    pll_power(&f, true);
    wr(&f, 0, TX_DIV, 0);
    wr(&f, 0, TX_CLK, CLOCK_ON | BIT(28) | 8);
    first = next_rise(&f, 51);
    g_assert_cmpuint(next_rise(&f, 51) - first, ==, 50);
    pll_power(&f, false);
    bool pll_held = level(&f, 4);
    for (unsigned i = 0; i < 8; ++i) {
        step(&f, 1000);
        g_assert_cmpint(level(&f, 4), ==, pll_held);
    }
    pll_power(&f, true);
    first = next_rise(&f, 51);
    g_assert_cmpuint(next_rise(&f, 51) - first, ==, 50);
    g_assert_cmphex(qtest_readl(f.q, SYS_CLOCK) &
                    R_SYSTEM_SYSCLK_CONF_SOC_CLK_SEL_MASK, ==, 0);
    gate(&f, 0, false);
    bool held = level(&f, 4);
    for (unsigned i = 0; i < 8; ++i) {
        step(&f, 1000);
        g_assert_cmpint(level(&f, 4), ==, held);
    }
    gate(&f, 0, true);
    next_rise(&f, 301);
    /* Stop releases all three physical TX nets to explicit pulldowns. */
    wr(&f, 0, TX_CONF, conf(&standard));
    g_assert_false(level(&f, 4));
    g_assert_false(level(&f, 5));
    g_assert_false(level(&f, 7));
    g_assert_cmpuint(rd(&f, 0, STATE), ==, 1);
    wr(&f, 0, TX_CONF, conf(&standard) | START | UPDATE);
    g_assert_cmphex(rd(&f, 0, TX_CONF) & UPDATE, ==, UPDATE);
    step(&f, 1);
    g_assert_cmphex(rd(&f, 0, TX_CONF) & UPDATE, ==, 0);
    next_rise(&f, 301);
    uint32_t reset = qtest_readl(f.q, SYS_RST);
    qtest_writel(f.q, SYS_RST, reset | GATE(0) | GATE(1));
    wr(&f, 0, TX_CONF, conf(&standard) | START);
    wr(&f, 1, RX_CONF, conf(&standard) | SLAVE | START);
    g_assert_cmphex(rd(&f, 0, TX_CONF) & START, ==, 0);
    g_assert_cmphex(rd(&f, 1, RX_CONF) & START, ==, 0);
    step(&f, 1000);
    g_assert_false(level(&f, 4));
    qtest_writel(f.q, SYS_RST, reset & ~(GATE(0) | GATE(1)));
    g_assert_cmphex(rd(&f, 0, DATE), ==, 0x2102080);
    g_assert_cmphex(rd(&f, 1, DATE), ==, 0x2102080);
    g_assert_cmphex(rd(&f, 0, RAW), ==, 0);
    g_assert_cmphex(rd(&f, 1, RAW), ==, 0);
    g_assert_false(level(&f, 4));
    /* Hardware reset restarts frames, never overwrites recorder sequence. */
    configure(&f, 0, &standard);
    configure(&f, 1, &standard);
    source(&f, 0, &standard, 16);
    receiver(&f, 1, 128);
    launch(&f, &standard, false);
    step(&f, 64 * 200 - 100);
    qtest_quit(f.q);
    GArray *a = events(&f, 1);
    g_assert_cmpuint(a->len, >=, 4);
    for (unsigned i = a->len - 4; i < a->len; ++i) {
        Event e = g_array_index(a, Event, i);
        g_assert_cmphex(e.sample, ==, word(0, i - (a->len - 4), 16));
        g_assert_cmpuint(e.frame, <=, 1);
    }
    g_array_unref(a);
    cleanup(&f);
}

static void test_local_rx_clock_hold(void)
{
    Fixture f = start();
    g_assert_cmphex(qtest_readl(f.q, SYS_CLOCK) &
                    R_SYSTEM_SYSCLK_CONF_SOC_CLK_SEL_MASK, ==, 0);
    for (unsigned c = 0; c < 2; ++c) {
        configure(&f, c, &standard);
        source(&f, c, &standard, 64);
        receiver(&f, c, 256);
        wr(&f, c, RX_CONF, conf(&standard) | SLAVE | START);
    }
    wr(&f, 1, TX_CONF, conf(&standard) | SLAVE | START);
    /* RX0 uses the hardware shared-clock path, RX1 physical slave clocks.
     * Both still consume only the cross-controller resolved data nets. */
    wr(&f, 0, TX_CONF, conf(&standard) | BIT(27) | START);
    step(&f, 4 * 16 * 200 - 100);
    for (unsigned hold = 0; hold < 2; ++hold) {
        uint8_t before[2][256], after[256];
        uint32_t current[2], next[2];
        if (hold) {
            pll_power(&f, false);
        }
        for (unsigned c = 0; c < 2; ++c) {
            qtest_memread(f.q, DST(c), before[c], sizeof(before[c]));
            current[c] = qtest_readl(f.q, DMA(c, 0, 0x34));
            next[c] = qtest_readl(f.q, DMA(c, 0, 0x30));
            wr(&f, c, RX_CLK, hold ? CLOCK_ON | BIT(28) | 8 :
               BIT(29) | 8);
        }
        /* External BCLK is demonstrably live while local RX evolution stops:
         * CLK_ACTIVE=0 first; genuine RTC BBPLL force-power-down second. */
        uint64_t rise = next_rise(&f, 201);
        g_assert_cmpuint(next_rise(&f, 201) - rise, ==, 200);
        step(&f, 12800 - 400);
        for (unsigned c = 0; c < 2; ++c) {
            qtest_memread(f.q, DST(c), after, sizeof(after));
            g_assert_cmpmem(after, sizeof(after), before[c], sizeof(before[c]));
            g_assert_cmphex(qtest_readl(f.q, DMA(c, 0, 0x34)), ==, current[c]);
            g_assert_cmphex(qtest_readl(f.q, DMA(c, 0, 0x30)), ==, next[c]);
        }
        if (hold) {
            pll_power(&f, true);
        }
        for (unsigned c = 0; c < 2; ++c) {
            /* Powered PLL160 RX resumes while CPU and TX remain on XTAL. */
            wr(&f, c, RX_CLK, CLOCK_ON | (hold ? BIT(28) : 0) | 8);
        }
        g_assert_cmphex(qtest_readl(f.q, SYS_CLOCK) &
                        R_SYSTEM_SYSCLK_CONF_SOC_CLK_SEL_MASK, ==, 0);
        step(&f, 4 * 16 * 200);
    }
    for (unsigned c = 0; c < 2; ++c) {
        uint8_t actual[24];
        qtest_memread(f.q, DST(c), actual, sizeof(actual));
        for (unsigned i = 0; i < 12; ++i) {
            unsigned source_index = (i / 4) * 8 + i % 4;
            g_assert_cmphex(lduw_le_p(actual + 2 * i), ==,
                            word(!c, source_index, 16));
        }
    }
    qtest_quit(f.q);
    for (unsigned c = 0; c < 2; ++c) {
        GArray *a = events(&f, c);
        g_assert_cmpuint(a->len, ==, 12);
        for (unsigned i = 0; i < a->len; ++i) {
            Event e = g_array_index(a, Event, i);
            unsigned source_index = (i / 4) * 8 + i % 4;
            g_assert_cmphex(e.sample, ==, word(!c, source_index, 16));
            g_assert_cmpuint(e.flags, ==, 3);
            g_assert_cmpuint(e.ns, ==, source_index * 3200ULL + 3100);
        }
        g_array_unref(a);
    }
    cleanup(&f);
}


/* Converter silicon scope must not be silently widened. I2S0 arithmetic is
 * explicitly unqualified until the missing hardware filter reference arrives;
 * I2S1 must reject a converter it does not contain. Neither path may record
 * fabricated zero audio. Raw PDM tests below use actual serial words. */
static void test_converter_boundary(gconstpointer opaque)
{
    unsigned c = GPOINTER_TO_UINT(opaque);
    Fixture f = start();
    configure(&f, c, &standard);
    source(&f, c, &standard, 16);
    wr(&f, c, PDM_CONF, BIT(25));
    wr(&f, c, TX_CONF, BYPASS | PDM | START);
    step(&f, 10000);
    assert_running(&f, false);
    qtest_quit(f.q);
    g_autofree char *log = NULL;
    gsize size;
    GError *error = NULL;
    g_assert_true(g_file_get_contents(f.log, &log, &size, &error));
    g_assert_no_error(error);
    g_assert_nonnull(strstr(log, c ? "unsupported on this controller" :
                           "exact filter arithmetic/reference unavailable"));
    for (unsigned port = 0; port < 2; ++port) {
        GArray *a = events(&f, port);
        g_assert_cmpuint(a->len, ==, 0);
        g_array_unref(a);
    }
    cleanup(&f);
}

static void test_raw_pdm_physical(gconstpointer opaque)
{
    Fixture f = start();
    bool paired_slave = GPOINTER_TO_UINT(opaque);
    configure(&f, 0, &standard);
    configure(&f, 1, &standard);
    source(&f, 0, &standard, 16);
    receiver(&f, 1, 64);
    if (paired_slave) {
        source(&f, 1, &standard, 16);
        /* SDK duplex slave shape: TX observes RX_WS through SIG_LOOPBACK.
         * None of TX_WS/TX_BCLK/RX_BCLK has a physical clock input route. */
        qtest_writel(f.q, IN(28), GPIO_FUNC_IN_LOW);
        qtest_writel(f.q, IN(29), GPIO_FUNC_IN_LOW);
        qtest_writel(f.q, IN(31), GPIO_FUNC_IN_LOW);
        wr(&f, 1, TX_CONF, BYPASS | PDM | SLAVE | BIT(27) | START);
    }
    wr(&f, 0, TX_CONF, BYPASS | PDM);
    wr(&f, 1, RX_CONF, BYPASS | PDM | SLAVE);
    wr(&f, 1, RX_CONF, BYPASS | PDM | SLAVE | START);
    wr(&f, 0, TX_CONF, BYPASS | PDM | START);
    /* Raw stereo packing emits one bit on each real WS transition. Each
     * physical bit is observed at GPIO7; no PCM/PDM conversion is inferred. */
    for (unsigned bit = 0; bit < 128; ++bit) {
        step(&f, bit ? 200 : 100);
        unsigned channel = bit % 2;
        unsigned sample = (bit / 32) * 2 + channel;
        unsigned shift = 15 - (bit % 32) / 2;
        g_assert_cmpint(level(&f, 7), ==, !!(word(0, sample, 16) & BIT(shift)));
        if (paired_slave) {
            g_assert_cmpint(level(&f, 21), ==, !!(word(1, sample, 16) & BIT(shift)));
            g_assert_cmphex(qtest_readl(f.q, IN(29)), ==, GPIO_FUNC_IN_LOW);
        }
        g_assert_cmpint(level(&f, 5), ==, channel);
    }
    qtest_quit(f.q);
    GArray *a = events(&f, 1);
    g_assert_cmpuint(a->len, ==, 8);
    for (unsigned i = 0; i < a->len; ++i) {
        Event e = g_array_index(a, Event, i);
        g_assert_cmphex(e.sample, ==, word(0, i, 16));
        g_assert_cmpuint(e.slot, ==, i % 2);
        g_assert_cmpuint(e.width, ==, 16);
        g_assert_cmpuint(e.flags, ==, 3);
    }
    g_array_unref(a);
    cleanup(&f);
}

static void external_routes(Fixture *f, bool source_pull, bool source_high,
                            bool source_input_enabled)
{
    apply_project(f, true, source_pull, source_high);
    for (unsigned pad = 8; pad <= 11; ++pad) {
        /* Enabling a buffer on an unknown pad is itself genuine GPIO
         * consumption. A clock source with no consumer leaves those buffers
         * off; active clock-consumer fixtures enable the real input route. */
        uint32_t mux = INPUT;
        if (pad <= 9 && !source_input_enabled) {
            mux &= ~R_IO_MUX_GPIOn_FUN_IE_MASK;
        }
        qtest_writel(f->q, PAD(pad), mux);
    }
    qtest_writel(f->q, IN(23), 9 | BIT(7));
    /* Leave GPIO8 disabled until a real source controller is routed. */
    qtest_writel(f->q, GPIO(A_GPIO_ENABLE_W1TS), BIT(10));
}

static void external_edges(Fixture *f, unsigned count, unsigned origin,
                           bool bclk, unsigned bclk_origin)
{
    for (unsigned i = 1; i <= count; ++i) {
        step(f, 99);
        g_assert_cmpint(level(f, 9), ==, (origin + i - 1) % 2);
        step(f, 1);
        unsigned edge = origin + i;
        g_assert_cmpint(level(f, 8), ==, edge % 2);
        g_assert_cmpint(level(f, 9), ==, edge % 2);
        g_assert_cmpuint(f->solved_ns, ==, f->now);
        /* Real input half-edges / 2.5: alternating 300ns/200ns output
         * half-periods, not a rounded frequency or synthetic clock. */
        g_assert_cmpint(level(f, 11), ==, (edge * 2 / 5) % 2);
        g_assert_cmpint(level(f, 4), ==,
                        bclk ? ((edge - bclk_origin) / 5) % 2 : 0);
    }
}

static void test_external_mclk_metrology(void)
{
    Fixture f = start();
    configure(&f, 0, &standard);
    configure(&f, 1, &standard);
    external_routes(&f, true, false, true);
    qtest_writel(f.q, OUT(8), 21);  /* I2S1 MCLK, native XTAL / 8. */
    qtest_writel(f.q, GPIO(A_GPIO_ENABLE_W1TS), BIT(8));
    qtest_writel(f.q, OUT(10), 23); /* Independent I2S0 MCLK output net. */
    wr(&f, 1, RX_CLK, 0);
    wr(&f, 1, TX_CLK, CLOCK_ON | 8);
    wr(&f, 0, RX_CLK, 0);
    wr(&f, 0, TX_DIV, 1 | (1U << 9)); /* n=2 + 1/2. */
    wr(&f, 0, TX_CLK, CLOCK_ON | (3U << 27) | 2);
    wr(&f, 0, TX_CONF1, rd(&f, 0, TX_CONF1) | BIT(7)); /* BCLK / 2. */
    external_edges(&f, 20, 0, false, 0); /* MCLK active before START. */
    g_assert_cmphex(rd(&f, 0, TX_CONF) & START, ==, 0);
    source(&f, 0, &standard, 64);
    receiver(&f, 1, 256);
    wr(&f, 1, RX_CLK, CLOCK_ON | 8);
    /* RX_CLK selects controller1 MCLK output; keep its identical XTAL /8. */
    wr(&f, 1, RX_CONF, conf(&standard) | SLAVE | START);
    wr(&f, 0, TX_CONF, conf(&standard) | START);
    external_edges(&f, 320, 20, true, 20);
    gate(&f, 1, false); /* Source power gate: all derived edges stop. */
    for (unsigned i = 0; i < 8; ++i) {
        step(&f, 1000);
        g_assert_false(level(&f, 9));
        g_assert_false(level(&f, 11));
        g_assert_false(level(&f, 4));
    }
    gate(&f, 1, true);
    external_edges(&f, 10, 340, true, 20);
    wr(&f, 0, TX_CLK, BIT(29) | (3U << 27) | 2);
    for (unsigned i = 0; i < 10; ++i) {
        step(&f, 100);
        g_assert_cmpint(level(&f, 9), ==, (i + 1) % 2);
        g_assert_false(level(&f, 11));
        g_assert_false(level(&f, 4));
    }
    wr(&f, 0, TX_CLK, CLOCK_ON | (3U << 27) | 2);
    /* Ten ignored source edges do not advance either divider accumulator. */
    external_edges(&f, 10, 350, true, 20);
    uint32_t reset = qtest_readl(f.q, SYS_RST);
    qtest_writel(f.q, SYS_RST, reset | GATE(0));
    for (unsigned i = 0; i < 10; ++i) {
        step(&f, 100);
        g_assert_cmpint(level(&f, 9), ==, (i + 1) % 2);
        g_assert_false(level(&f, 11));
        g_assert_false(level(&f, 4));
    }
    qtest_writel(f.q, SYS_RST, reset & ~GATE(0));
    wr(&f, 0, RX_CLK, 0);
    wr(&f, 0, TX_DIV, 1 | (1U << 9));
    wr(&f, 0, TX_CLK, CLOCK_ON | (3U << 27) | 2);
    external_edges(&f, 10, 0, false, 0); /* Reset MCLK phase; START stays clear. */
    qtest_quit(f.q);
    GArray *a = events(&f, 1);
    g_assert_cmpuint(a->len, ==, 2);
    for (unsigned i = 0; i < a->len; ++i) {
        Event e = g_array_index(a, Event, i);
        g_assert_cmphex(e.sample, ==, word(0, i, 16));
        g_assert_cmpuint(e.ns, ==, 17500 + i * 16000ULL);
        g_assert_cmpuint(e.flags, ==, 3);
    }
    g_array_unref(a);
    cleanup(&f);
}

static void test_external_mclk_unknown(gconstpointer opaque)
{
    unsigned consumer = GPOINTER_TO_UINT(opaque);
    Fixture f = start();
    configure(&f, 0, &standard);
    external_routes(&f, false, false, consumer != 0); /* Actual unknown source. */
    wr(&f, 0, RX_CLK, 0);
    wr(&f, 0, TX_CLK, CLOCK_ON | (3U << 27) | 2);
    if (consumer == 1) {
        qtest_writel(f.q, OUT(10), 23); /* Real active MCLK output consumer. */
    } else if (consumer == 2) {
        source(&f, 0, &standard, 16);
        wr(&f, 0, TX_CONF, conf(&standard) | START);
    }
    step(&f, 10000);
    assert_running(&f, !consumer);
    qtest_quit(f.q);
    g_autofree char *log = NULL;
    GError *error = NULL;
    g_assert_true(g_file_get_contents(f.log, &log, NULL, &error));
    g_assert_no_error(error);
    if (consumer) {
        g_assert_nonnull(strstr(log, "consumed input floating, contended or unknown"));
    } else {
        g_assert_null(strstr(log, "I2S0 TX dependency:"));
    }
    for (unsigned c = 0; c < 2; ++c) {
        GArray *a = events(&f, c);
        g_assert_cmpuint(a->len, ==, 0);
        g_array_unref(a);
    }
    cleanup(&f);
}

static void test_external_slave_quiet(gconstpointer opaque)
{
    unsigned mode = GPOINTER_TO_UINT(opaque);
    bool held_high = mode & 1;
    unsigned receiver_controller = (mode >> 1) == 1 ? 1 : 0;
    bool shared = (mode >> 1) == 2;
    Fixture f = start();
    configure(&f, 0, &standard);
    configure(&f, 1, &standard);
    external_routes(&f, true, held_high, true);
    qtest_writel(f.q, OUT(8), 21);
    qtest_writel(f.q, GPIO(A_GPIO_ENABLE_W1TS), BIT(8));
    qtest_writel(f.q, IN(21), 9 | BIT(7));
    /* Source1's TX clock unit independently supplies its MCLK output.
     * RX_CLK.EN=0 selects TX for that output, but RX_CLK.ACTIVE=1 still
     * consumes the genuine external pin for the slave module clock. */
    wr(&f, 1, RX_CLK, 0);
    wr(&f, 1, TX_CLK, CLOCK_ON | 2); /* XTAL40/N2, 25ns physical half-edge. */
    wr(&f, 0, RX_CLK, 0);
    wr(&f, 0, TX_CLK, CLOCK_ON | 42); /* 525ns BCLK half-edge, <1MHz. */
    wr(&f, receiver_controller, RX_CLK, BIT(26) | (3U << 27) | 2);
    source(&f, 0, &standard, 64);
    if (!receiver_controller) {
        source(&f, 1, &standard, 64);
        wr(&f, 1, TX_CONF, conf(&standard) | SLAVE | START);
    }
    receiver(&f, receiver_controller, 256);
    wr(&f, receiver_controller, RX_CONF, conf(&standard) | SLAVE | START);
    wr(&f, 0, TX_CONF, conf(&standard) | START | (shared ? BIT(27) : 0));
    /* Source module pulses every second actual MCLK edge, at multiples of
     * 50ns. BCLK rises at 525+1050*k, hence every accepted bit is sampled
     * exactly 25ns after the resolved BCLK transition. */
    step(&f, 33600 + (held_high ? 25 : 0));
    g_assert_cmpint(level(&f, 9), ==, held_high);
    uint8_t before[256], after[256];
    qtest_memread(f.q, DST(receiver_controller), before, sizeof(before));
    uint32_t cursor = qtest_readl(f.q, DMA(receiver_controller, 0, 0x34));
    uint32_t next = qtest_readl(f.q, DMA(receiver_controller, 0, 0x30));
    uint64_t quiet_start = f.now;
    wr(&f, 1, TX_CLK, BIT(29) | 2); /* Only source clock unit ACTIVE off. */
    g_assert_cmpint(level(&f, 9), ==, held_high);
    uint64_t rise = next_rise(&f, 1101);
    g_assert_cmpuint(next_rise(&f, 1101) - rise, ==, 1050);
    step(&f, quiet_start + 67200 - f.now);
    g_assert_cmpint(level(&f, 9), ==, held_high);
    qtest_memread(f.q, DST(receiver_controller), after, sizeof(after));
    g_assert_cmpmem(after, sizeof(after), before, sizeof(before));
    g_assert_cmphex(qtest_readl(f.q, DMA(receiver_controller, 0, 0x34)), ==, cursor);
    g_assert_cmphex(qtest_readl(f.q, DMA(receiver_controller, 0, 0x30)), ==, next);
    wr(&f, 1, TX_CLK, CLOCK_ON | 2);
    step(&f, 24);
    g_assert_cmpint(level(&f, 9), ==, held_high);
    step(&f, 1);
    g_assert_cmpint(level(&f, 9), ==, !held_high);
    step(&f, 33600 - 25);
    qtest_quit(f.q);
    GArray *a = events(&f, receiver_controller);
    g_assert_cmpuint(a->len, ==, 4);
    static const uint64_t times[] = {16300, 33100, 117100, 133900};
    for (unsigned i = 0; i < a->len; ++i) {
        Event e = g_array_index(a, Event, i);
        unsigned source_index = receiver_controller && i >= 2 ? i + 4 : i;
        g_assert_cmphex(e.sample, ==, word(!receiver_controller, source_index, 16));
        g_assert_cmpuint(e.ns, ==, times[i]);
        g_assert_cmpuint(e.flags, ==, 3);
        g_assert_false(e.ns > quiet_start && e.ns <= quiet_start + 67200);
    }
    g_array_unref(a);
    GArray *unused = events(&f, !receiver_controller);
    g_assert_cmpuint(unused->len, ==, 0);
    g_array_unref(unused);
    cleanup(&f);
}

#define PEER_SERVICE "/machine/soc/i2s-sample-peers"

static QDict *service_reply(Fixture *f, const char *property)
{
    return qtest_qmp(f->q, "{'execute':'qom-get','arguments':"
                     "{'path':%s,'property':%s}}", PEER_SERVICE, property);
}

static QDict *service_object(Fixture *f, const char *property)
{
    QDict *reply = service_reply(f, property);
    g_assert_false(qdict_haskey(reply, "error"));
    QDict *result = object(qdict_get_str(reply, "return"));
    qobject_unref(reply);
    return result;
}

static QDict *list_dict(QList *list, unsigned index)
{
    const QListEntry *e;
    QLIST_FOREACH_ENTRY(list, e) {
        if (!index--) {
            QDict *d = qobject_to(QDict, qlist_entry_obj(e));
            g_assert_nonnull(d);
            return d;
        }
    }
    g_assert_not_reached();
}

static QDict *capture_request(Fixture *f, const char *id, unsigned controller,
                              uint64_t peer_offset, uint64_t controller_offset,
                              unsigned count)
{
    g_autofree char *request = g_strdup_printf(
        "{\"componentId\":\"%s\",\"offset\":0,\"count\":1024,"
        "\"transitions\":{\"controllerId\":%u,\"peerOffset\":%" PRIu64
        ",\"controllerOffset\":%" PRIu64 ",\"count\":%u}}",
        id, controller, peer_offset, controller_offset, count);
    return qtest_qmp(f->q, "{'execute':'qom-set','arguments':"
        "{'path':%s,'property':'capture-request-json','value':%s}}",
        PEER_SERVICE, request);
}

static QDict *metadata_capture(Fixture *f, unsigned controller,
                               uint64_t peer_offset, uint64_t controller_offset,
                               unsigned count)
{
    QDict *reply = capture_request(f, "P", controller, peer_offset, controller_offset, count);
    g_assert_false(qdict_haskey(reply, "error"));
    qobject_unref(reply);
    QDict *capture = service_object(f, "capture-json");
    QDict *transitions = qdict_get_qdict(capture, "transitions");
    g_assert_cmpint(qdict_get_int(transitions, "version"), ==, 1);
    QDict *core = qdict_get_qdict(transitions, "controller");
    QDict *peer = qdict_get_qdict(transitions, "peer");
    g_assert_cmpint(qdict_get_int(core, "controller"), ==, controller);
    g_assert_cmpint(qdict_get_int(core, "capacity"), ==, 16384);
    g_assert_cmpint(qdict_get_int(core, "version"), ==, 1);
    g_assert_cmpint(qdict_get_int(peer, "version"), ==, 1);
    g_assert_cmpuint(qlist_size(qdict_get_qlist(core, "records")), <=, count);
    g_assert_cmpuint(qlist_size(qdict_get_qlist(peer, "records")), <=, count);
    const QListEntry *e;
    uint64_t sequence = controller_offset;
    QLIST_FOREACH_ENTRY(qdict_get_qlist(core, "records"), e) {
        QDict *record = qobject_to(QDict, qlist_entry_obj(e));
        g_assert_cmpuint(qdict_get_int(record, "sequence"), ==, sequence++);
        /* Attribution is explicitly payload-free; DIN is a separate oracle. */
        g_assert_false(qdict_haskey(record, "sample"));
    }
    sequence = peer_offset;
    QLIST_FOREACH_ENTRY(qdict_get_qlist(peer, "records"), e) {
        QDict *record = qobject_to(QDict, qlist_entry_obj(e));
        g_assert_cmpuint(qdict_get_int(record, "sequence"), ==, sequence++);
        g_assert_false(qdict_haskey(record, "sample"));
    }
    return capture;
}

static QDict *core_window(QDict *capture)
{
    return qdict_get_qdict(qdict_get_qdict(capture, "transitions"), "controller");
}

static unsigned window_kind_count(QDict *window, unsigned kind)
{
    unsigned count = 0;
    const QListEntry *e;
    QLIST_FOREACH_ENTRY(qdict_get_qlist(window, "records"), e) {
        count += qdict_get_int(qobject_to(QDict, qlist_entry_obj(e)), "kind") == kind;
    }
    return count;
}

static unsigned kind_count(QDict *capture, unsigned kind)
{
    return window_kind_count(core_window(capture), kind);
}

static void metadata_peer_geometry(Fixture *f, unsigned controller,
                                   const Format *v, bool raw, unsigned capacity)
{
    f->metadata_peer = true;
    f->metadata_controller = controller;
    f->metadata_width = v->width;
    f->metadata_slots = v->slots;
    f->metadata_mask = v->mask;
    f->metadata_ws_width = v->ws_width;
    f->metadata_tdm = v->slots > 2;
    f->metadata_raw = raw;
    f->metadata_capacity = capacity;
    apply_project(f, false, false, false);
    qtest_writel(f->q, PAD(17), INPUT);
    QDict *status = service_object(f, "status-json");
    g_assert_cmpint(qdict_get_int(status, "version"), ==, 1);
    g_assert_cmpuint(qlist_size(qdict_get_qlist(status, "peers")), ==, 1);
    QDict *peer = list_dict(qdict_get_qlist(status, "peers"), 0);
    g_assert_cmpstr(qdict_get_str(peer, "component_id"), ==, "P");
    g_assert_true(qdict_get_bool(peer, "power_known"));
    g_assert_true(qdict_get_bool(peer, "registered_powered"));
    g_assert_true(qdict_get_bool(peer, "powered"));
    g_assert_false(qdict_get_bool(peer, "paused"));
    g_assert_false(qdict_get_bool(peer, "capture_overflow"));
    g_assert_cmpint(qdict_get_int(qdict_get_qdict(peer, "transitions"), "capacity"),
                    ==, 2 * capacity + 16);
    for (unsigned c = 0; c < 2; ++c) {
        QDict *core = list_dict(qdict_get_qlist(status, "controllers"), c);
        g_assert_cmpint(qdict_get_int(core, "controller"), ==, c);
        g_assert_true(qdict_get_bool(core, "capture_enabled"));
        g_assert_cmpint(qdict_get_int(core, "capacity"), ==, 16384);
    }
    qobject_unref(status);
}

static void metadata_peer(Fixture *f, unsigned controller, unsigned width,
                          bool raw, unsigned capacity)
{
    Format v = standard;
    v.width = v.slot = v.ws_width = width;
    metadata_peer_geometry(f, controller, &v, raw, capacity);
}

static void capture_purity(Fixture *f, QDict *before)
{
    GString *a = qobject_to_json(QOBJECT(before));
    for (unsigned i = 0; i < 3; ++i) {
        QDict *after = service_object(f, "capture-json");
        GString *b = qobject_to_json(QOBJECT(after));
        g_assert_cmpstr(a->str, ==, b->str);
        g_string_free(b, true);
        qobject_unref(after);
    }
    g_string_free(a, true);
}

static void compare_din_rx(Fixture *f, QDict *capture, unsigned rx,
                           const uint32_t *expected, unsigned count)
{
    unsigned din_count = f->metadata_raw ? count * 16 : count;
    g_assert_cmpuint(qdict_get_int(capture, "total"), ==, din_count);
    QList *din = qdict_get_qlist(capture, "events");
    g_assert_cmpuint(qlist_size(din), ==, din_count);
    uint32_t assembled[16] = {0};
    g_assert_cmpuint(count, <=, G_N_ELEMENTS(assembled));
    const QListEntry *entry;
    unsigned i = 0;
    QLIST_FOREACH_ENTRY(din, entry) {
        QDict *d = qobject_to(QDict, qlist_entry_obj(entry));
        g_assert_cmpuint(qdict_get_int(d, "sequence"), ==, i);
        g_assert_cmpint(qdict_get_bool(d, "raw"), ==, f->metadata_raw);
        if (f->metadata_raw) {
            unsigned sample = (i / 32) * 2 + i % 2;
            unsigned bit = 15 - (i % 32) / 2;
            unsigned value = qdict_get_int(d, "sample");
            g_assert_cmpuint(value, <=, 1);
            g_assert_cmpuint(value, ==, !!(expected[sample] & BIT(bit)));
            g_assert_cmpuint(qdict_get_int(d, "slot"), ==, i % 2);
            g_assert_cmpuint(qdict_get_int(d, "ns"), ==, 100 + i * 200ULL);
            assembled[sample] |= value << bit;
        } else {
            g_assert_cmphex(qdict_get_int(d, "sample"), ==, expected[i]);
            assembled[i] = qdict_get_int(d, "sample");
        }
        ++i;
    }
    qtest_quit(f->q);
    GArray *actual = events(f, rx);
    g_assert_cmpuint(actual->len, ==, count);
    for (i = 0; i < count; ++i) {
        Event r = g_array_index(actual, Event, i);
        g_assert_cmphex(r.sample, ==, assembled[i]);
        g_assert_cmphex(r.sample, ==, expected[i]);
        if (f->metadata_raw) {
            /* Independent peer DIN stores edge bits; the native RX packs
             * both completed channels at the actual final stereo halfphase. */
            g_assert_cmpuint(r.slot, ==, i % 2);
            g_assert_cmpuint(r.ns, ==, 6300 + (i / 2) * 6400ULL);
        } else {
            QDict *d = list_dict(din, i);
            g_assert_cmpuint(qdict_get_int(d, "slot"), ==, r.slot);
            g_assert_cmpuint(qdict_get_int(d, "ns"), ==, r.ns);
        }
    }
    g_array_unref(actual);
}

static void test_metadata_raw_slave_master_clock(void)
{
    Fixture f = start();
    configure(&f, 0, &standard);
    configure(&f, 1, &standard);
    source(&f, 1, &standard, 32);
    receiver(&f, 1, 512);
    f.metadata_master = true;
    metadata_peer(&f, 1, 16, true, 256);
    /* The registered peer is the only physical clock driver. MCU1 emits
     * raw data after its RX_WS edge; the master peer samples that DIN on
     * the following BCLK rise, with a genuine100ns setup halfperiod.
     * MCU1 simultaneously receives the peer's independent declared DOUT. */
    g_assert_cmphex(rd(&f, 0, TX_CONF) & START, ==, 0);
    g_assert_cmphex(rd(&f, 0, RX_CONF) & START, ==, 0);
    qtest_writel(f.q, IN(30), 17 | BIT(7));
    qtest_writel(f.q, IN(28), GPIO_FUNC_IN_LOW);
    qtest_writel(f.q, IN(29), GPIO_FUNC_IN_LOW);
    qtest_writel(f.q, IN(31), GPIO_FUNC_IN_LOW);
    wr(&f, 1, RX_CONF, BYPASS | PDM | SLAVE | START);
    wr(&f, 1, TX_CONF, BYPASS | PDM | SLAVE | BIT(27) | START);
    QDict *capture = metadata_capture(&f, 1, 0, 0, 1024);
    g_assert_cmpuint(kind_count(capture, S3_I2S_TX_LOAD), ==, 0);
    g_assert_cmpuint(kind_count(capture, S3_I2S_TX_COMPLETE), ==, 0);
    g_assert_cmpuint(window_kind_count(qdict_get_qdict(
        qdict_get_qdict(capture, "transitions"), "peer"),
        S3_I2S_PEER_TRANSITION_COMPLETE), ==, 0);
    capture_purity(&f, capture);
    qobject_unref(capture);
    step(&f, 199); /* Initial inactive BCLK rise is not a physical raw phase. */
    capture = metadata_capture(&f, 1, 0, 0, 1024);
    g_assert_cmpuint(kind_count(capture, S3_I2S_TX_LOAD), ==, 0);
    g_assert_cmpuint(qdict_get_int(capture, "total"), ==, 0);
    qobject_unref(capture);
    step(&f, 1);
    capture = metadata_capture(&f, 1, 0, 0, 1024);
    g_assert_cmpuint(kind_count(capture, S3_I2S_TX_LOAD), ==, 2);
    g_assert_cmpuint(kind_count(capture, S3_I2S_TX_COMPLETE), ==, 0);
    g_assert_cmpuint(qdict_get_int(capture, "total"), ==, 0);
    qobject_unref(capture);
    for (unsigned phase = 0; phase < 128; ++phase) {
        unsigned channel = phase % 2;
        unsigned sample = (phase / 32) * 2 + channel;
        unsigned shift = 15 - (phase % 32) / 2;
        bool peer_bit = !!(master_raw_words[channel] & BIT(shift));
        bool mcu_bit = !!(word(1, sample, 16) & BIT(shift));
        g_assert_false(level(&f, 4));
        g_assert_cmpint(level(&f, 5), ==, channel);
        g_assert_cmpint(level(&f, 17), ==, peer_bit);
        g_assert_cmpint(level(&f, 21), ==, mcu_bit);
        step(&f, 99);
        g_assert_false(level(&f, 4));
        step(&f, 1);
        g_assert_true(level(&f, 4));
        g_assert_cmpint(level(&f, 21), ==, mcu_bit);
        if (phase + 1 < 128) {
            step(&f, 100);
        }
    }
    capture = metadata_capture(&f, 1, 0, 0, 1024);
    g_assert_cmpuint(kind_count(capture, S3_I2S_TX_LOAD), ==, 8);
    g_assert_cmpuint(kind_count(capture, S3_I2S_TX_COMPLETE), ==, 8);
    g_assert_cmpuint(kind_count(capture, S3_I2S_TX_ABORT), ==, 0);
    unsigned loads = 0, completes = 0;
    uint64_t epoch = qdict_get_int(core_window(capture), "epoch");
    const QListEntry *entry;
    QLIST_FOREACH_ENTRY(qdict_get_qlist(core_window(capture), "records"), entry) {
        QDict *r = qobject_to(QDict, qlist_entry_obj(entry));
        unsigned kind = qdict_get_int(r, "kind");
        if (kind == S3_I2S_TX_LOAD) {
            g_assert_cmpuint(qdict_get_int(r, "source_word_id"), ==, ++loads);
            g_assert_cmpuint(qdict_get_int(r, "ns"), ==, 200 + ((loads - 1) / 2) * 6400ULL);
        } else if (kind == S3_I2S_TX_COMPLETE) {
            unsigned i = completes++;
            uint64_t first = 200 + (i / 2) * 6400ULL + (i % 2) * 200;
            g_assert_cmpuint(qdict_get_int(r, "source_word_id"), ==, i + 1);
            g_assert_cmpuint(qdict_get_int(r, "epoch"), ==, epoch);
            g_assert_cmpuint(qdict_get_int(r, "word_ordinal"), ==, i + 1);
            g_assert_cmpuint(qdict_get_int(r, "source"), ==, S3_I2S_TX_PAYLOAD);
            g_assert_cmpuint(qdict_get_int(r, "raw_channel"), ==, i % 2);
            g_assert_cmpuint(qdict_get_int(r, "shifted_bits"), ==, 16);
            g_assert_cmpuint(qdict_get_int(r, "valid_bits"), ==, 16);
            g_assert_cmpuint(qdict_get_int(r, "slot_bits"), ==, 16);
            g_assert_cmpuint(qdict_get_int(r, "first_ns"), ==, first);
            g_assert_cmpuint(qdict_get_int(r, "last_ns"), ==, first + 6000);
            g_assert_cmpuint(qdict_get_int(r, "ns"), ==, first + 6000);
            g_assert_cmpint(qdict_get_int(r, "first_boundary_ns"), ==, -1);
            g_assert_cmpint(qdict_get_int(r, "last_boundary_ns"), ==, -1);
            g_assert_cmpuint(qdict_get_int(r, "last_halfphase") -
                            qdict_get_int(r, "first_halfphase"), ==, 30);
        }
    }
    QDict *peer_window = qdict_get_qdict(qdict_get_qdict(capture, "transitions"), "peer");
    g_assert_cmpuint(window_kind_count(peer_window, S3_I2S_PEER_TRANSITION_BEGIN), ==, 128);
    g_assert_cmpuint(window_kind_count(peer_window, S3_I2S_PEER_TRANSITION_COMPLETE), ==, 128);
    capture_purity(&f, capture);
    QList *din = qdict_get_qlist(capture, "events");
    g_assert_cmpuint(qdict_get_int(capture, "total"), ==, 128);
    g_assert_cmpuint(qlist_size(din), ==, 128);
    unsigned phase = 0;
    QLIST_FOREACH_ENTRY(din, entry) {
        QDict *r = qobject_to(QDict, qlist_entry_obj(entry));
        unsigned sample = (phase / 32) * 2 + phase % 2;
        unsigned shift = 15 - (phase % 32) / 2;
        g_assert_true(qdict_get_bool(r, "raw"));
        g_assert_cmpuint(qdict_get_int(r, "sequence"), ==, phase);
        g_assert_cmpuint(qdict_get_int(r, "sample"), ==, !!(word(1, sample, 16) & BIT(shift)));
        g_assert_cmpuint(qdict_get_int(r, "slot"), ==, phase % 2);
        g_assert_cmpuint(qdict_get_int(r, "ns"), ==, 300 + phase * 200ULL);
        ++phase;
    }
    /* RX is the opposite real direction, not an echo of MCU DIN. */
    uint8_t rx_data[16];
    qtest_memread(f.q, DST(1), rx_data, sizeof(rx_data));
    for (unsigned i = 0; i < 8; ++i) {
        g_assert_cmphex(lduw_le_p(rx_data + 2 * i), ==, master_raw_words[i % 2]);
    }
    qtest_quit(f.q);
    GArray *rx = events(&f, 1);
    g_assert_cmpuint(rx->len, ==, 8);
    for (unsigned i = 0; i < 8; ++i) {
        Event r = g_array_index(rx, Event, i);
        g_assert_cmphex(r.sample, ==, master_raw_words[i % 2]);
        g_assert_cmpuint(r.slot, ==, i % 2);
        g_assert_cmpuint(r.ns, ==, 6400 + (i / 2) * 6400ULL);
        g_assert_cmpuint(r.width, ==, 16);
        g_assert_cmpuint(r.flags, ==, 3);
    }
    g_array_unref(rx);
    GArray *idle = events(&f, 0);
    g_assert_cmpuint(idle->len, ==, 0);
    g_array_unref(idle);
    qobject_unref(capture);
    cleanup(&f);
}

static void test_metadata_completion(gconstpointer opaque)
{
    unsigned mode = GPOINTER_TO_UINT(opaque);
    if (mode == 4) {
        test_metadata_raw_slave_master_clock();
        return;
    }
    bool raw = mode >= 3;
    unsigned controller = mode == 1;
    Format v = standard;
    v.slave_loop = true;
    v.rx_master = mode == 2;
    Fixture f = start();
    for (unsigned c = 0; c < 2; ++c) {
        configure(&f, c, &v);
        source(&f, c, &v, 32);
        receiver(&f, c, 512);
    }
    metadata_peer(&f, controller, 16, raw, raw ? 256 : 64);
    if (v.rx_master) {
        qtest_writel(f.q, OUT(18), 26);
        qtest_writel(f.q, OUT(19), 27);
    }
    QDict *cold = metadata_capture(&f, controller, 0, 0, 1024);
    g_assert_cmpuint(kind_count(cold, S3_I2S_TX_LOAD), ==, 0);
    g_assert_cmpuint(kind_count(cold, S3_I2S_TX_COMPLETE), ==, 0);
    g_assert_cmpuint(window_kind_count(qdict_get_qdict(
        qdict_get_qdict(cold, "transitions"), "peer"),
        S3_I2S_PEER_TRANSITION_COMPLETE), ==, 0);
    capture_purity(&f, cold);
    qobject_unref(cold);
    if (raw) {
        wr(&f, !controller, RX_CONF, BYPASS | PDM | SLAVE | START);
        wr(&f, 0, TX_CONF, BYPASS | PDM | START);
        step(&f, 99);
        cold = metadata_capture(&f, controller, 0, 0, 1024);
        g_assert_cmpuint(kind_count(cold, S3_I2S_TX_LOAD), ==, 0);
        qobject_unref(cold);
        step(&f, 1);
    } else {
        launch(&f, &v, true);
    }
    cold = metadata_capture(&f, controller, 0, 0, 1024);
    g_assert_cmpuint(kind_count(cold, S3_I2S_TX_LOAD), ==, raw ? 2 : 1);
    g_assert_cmpuint(kind_count(cold, S3_I2S_TX_COMPLETE), ==, 0);
    capture_purity(&f, cold);
    qobject_unref(cold);
    /* Probe the peer's isolated physical DOUT independently of either
     * metadata source identity or MCU DIN payload. Every first-word bit
     * (raw: first sixteen actual halfphases) must really be published. */
    for (unsigned bit = 0; bit < 16; ++bit) {
        bool expected = raw ? bit % 2 : !!(0x5a & BIT(15 - bit));
        g_assert_cmpint(level(&f, 17), ==, expected);
        if (bit + 1 < 16) {
            step(&f, 200);
        }
    }
    uint64_t first_complete = raw ? 6200 : 3100;
    step(&f, first_complete - f.now - 1);
    cold = metadata_capture(&f, controller, 0, 0, 1024);
    g_assert_cmpuint(kind_count(cold, S3_I2S_TX_COMPLETE), ==, 0);
    if (!raw) {
        g_assert_cmpuint(window_kind_count(qdict_get_qdict(
            qdict_get_qdict(cold, "transitions"), "peer"),
            S3_I2S_PEER_TRANSITION_COMPLETE), ==, 0);
    }
    qobject_unref(cold);
    step(&f, 1);
    cold = metadata_capture(&f, controller, 0, 0, 1024);
    g_assert_cmpuint(kind_count(cold, S3_I2S_TX_COMPLETE), ==, 1);
    qobject_unref(cold);
    step(&f, (raw ? 25600 : 25500) - f.now);
    QDict *capture = metadata_capture(&f, controller, 0, 0, 1024);
    g_assert_cmpuint(kind_count(capture, S3_I2S_TX_LOAD), ==, 8);
    g_assert_cmpuint(kind_count(capture, S3_I2S_TX_COMPLETE), ==, 8);
    g_assert_cmpuint(kind_count(capture, S3_I2S_TX_ABORT), ==, 0);
    uint64_t ids[8], epochs[8];
    unsigned loads = 0, completes = 0;
    const QListEntry *e;
    QLIST_FOREACH_ENTRY(qdict_get_qlist(core_window(capture), "records"), e) {
        QDict *r = qobject_to(QDict, qlist_entry_obj(e));
        unsigned kind = qdict_get_int(r, "kind");
        if (kind == S3_I2S_TX_LOAD) {
            g_assert_cmpuint(loads, <, 8);
            ids[loads] = qdict_get_int(r, "source_word_id");
            epochs[loads] = qdict_get_int(r, "epoch");
            g_assert_cmpuint(ids[loads], ==, loads + 1);
            g_assert_cmpuint(qdict_get_int(r, "shifted_bits"), ==, 0);
            ++loads;
        } else if (kind == S3_I2S_TX_COMPLETE) {
            unsigned i = completes++;
            g_assert_cmpuint(i, <, loads);
            g_assert_cmpuint(qdict_get_int(r, "source_word_id"), ==, ids[i]);
            g_assert_cmpuint(qdict_get_int(r, "epoch"), ==, epochs[i]);
            g_assert_cmpuint(qdict_get_int(r, "word_ordinal"), ==, i + 1);
            g_assert_cmpuint(qdict_get_int(r, "source"), ==, S3_I2S_TX_PAYLOAD);
            g_assert_cmpuint(qdict_get_int(r, "shifted_bits"), ==, 16);
            g_assert_cmpuint(qdict_get_int(r, "slot_bits"), ==, 16);
            g_assert_cmpuint(qdict_get_int(r, "valid_bits"), ==, 16);
            uint64_t first = raw ? 100 + (i / 2) * 6400 + (i % 2) * 200 : i * 3200;
            uint64_t last = first + (raw ? 6000 : 3000);
            g_assert_cmpuint(qdict_get_int(r, "first_ns"), ==, first);
            g_assert_cmpuint(qdict_get_int(r, "last_ns"), ==, last);
            g_assert_cmpuint(qdict_get_int(r, "ns"), ==, last + 100);
            if (raw) {
                g_assert_cmpuint(qdict_get_int(r, "raw_channel"), ==, i % 2);
                g_assert_cmpuint(qdict_get_int(r, "last_halfphase") -
                                qdict_get_int(r, "first_halfphase"), ==, 30);
            }
            g_assert_cmpint(qdict_get_int(r, "first_boundary_ns"), ==,
                            first + 100);
            g_assert_cmpint(qdict_get_int(r, "last_boundary_ns"), ==,
                            last + 100);
        }
    }
    QDict *peer_window = qdict_get_qdict(qdict_get_qdict(capture, "transitions"), "peer");
    unsigned peer_begins = 0, peer_completes = 0;
    QLIST_FOREACH_ENTRY(qdict_get_qlist(peer_window, "records"), e) {
        QDict *r = qobject_to(QDict, qlist_entry_obj(e));
        unsigned kind = qdict_get_int(r, "kind");
        if (kind != S3_I2S_PEER_TRANSITION_BEGIN &&
            kind != S3_I2S_PEER_TRANSITION_COMPLETE) {
            continue;
        }
        unsigned i = kind == S3_I2S_PEER_TRANSITION_BEGIN ?
                     peer_begins++ : peer_completes++;
        g_assert_cmpuint(qdict_get_int(r, "source_id"), ==, i + 1);
        g_assert_cmpuint(qdict_get_int(r, "source_cursor"), ==, i % (raw ? 32 : 2));
        g_assert_cmpuint(qdict_get_int(r, "source"), ==, S3_I2S_PEER_SOURCE_PAYLOAD);
        g_assert_true(qdict_get_bool(r, "power_known"));
        g_assert_true(qdict_get_bool(r, "powered"));
        g_assert_cmpuint(qdict_get_int(r, "epoch"), ==, qdict_get_int(peer_window, "epoch"));
        g_assert_cmpuint(qdict_get_int(r, "power_epoch"), >, 0);
        uint64_t first = raw ? (i ? 100 + i * 200ULL : 0) : i * 3200ULL;
        g_assert_cmpuint(qdict_get_int(r, "first_ns"), ==, first);
        if (kind == S3_I2S_PEER_TRANSITION_COMPLETE) {
            uint64_t last = raw ? first : first + 3000;
            uint64_t boundary = raw ? 100 + i * 200ULL : first + 3100;
            g_assert_cmpuint(qdict_get_int(r, "published_bits"), ==, raw ? 1 : 16);
            g_assert_cmpuint(qdict_get_int(r, "slot_bits"), ==, raw ? 1 : 16);
            g_assert_cmpuint(qdict_get_int(r, "last_ns"), ==, last);
            g_assert_cmpuint(qdict_get_int(r, "ns"), ==, boundary);
            g_assert_cmpuint(qdict_get_int(r, "last_boundary_ns"), ==, boundary);
        } else {
            g_assert_cmpuint(qdict_get_int(r, "published_bits"), ==, 1);
            g_assert_cmpint(qdict_get_int(r, "first_boundary_ns"), ==, -1);
        }
    }
    g_assert_cmpuint(peer_begins, ==, raw ? 128 : 8);
    g_assert_cmpuint(peer_completes, ==, raw ? 128 : 8);
    capture_purity(&f, capture);
    uint32_t expected[8];
    for (unsigned i = 0; i < 8; ++i) {
        expected[i] = word(controller, i, 16);
    }
    compare_din_rx(&f, capture, !controller, expected, 8);
    qobject_unref(capture);
    cleanup(&f);
}

static void test_metadata_abort(gconstpointer opaque)
{
    unsigned action = GPOINTER_TO_UINT(opaque);
    Fixture f = start();
    configure(&f, 0, &standard);
    configure(&f, 1, &standard);
    source(&f, 0, &standard, 32);
    receiver(&f, 1, 512);
    metadata_peer(&f, 0, 16, false, 64);
    launch(&f, &standard, false);
    step(&f, 900); /* Exactly five actual sampling boundaries, not a word. */
    QDict *before = metadata_capture(&f, 0, 0, 0, 1024);
    g_assert_cmpuint(kind_count(before, S3_I2S_TX_LOAD), ==, 1);
    g_assert_cmpuint(kind_count(before, S3_I2S_TX_COMPLETE), ==, 0);
    g_assert_cmpuint(qdict_get_int(before, "total"), ==, 0);
    uint64_t epoch = qdict_get_int(core_window(before), "epoch");
    qobject_unref(before);
    static const unsigned reasons[] = {S3_I2S_TX_REASON_REGISTER_STOP,
        S3_I2S_TX_REASON_GATE, S3_I2S_TX_REASON_STREAM_RESET,
        S3_I2S_TX_REASON_DEVICE_RESET};
    if (!action) {
        wr(&f, 0, TX_CONF, conf(&standard));
    } else if (action == 1) {
        gate(&f, 0, false);
    } else if (action == 2) {
        wr(&f, 0, TX_CONF, BIT(0));
    } else {
        uint32_t reset = qtest_readl(f.q, SYS_RST);
        qtest_writel(f.q, SYS_RST, reset | GATE(0));
        qtest_writel(f.q, SYS_RST, reset & ~GATE(0));
    }
    QDict *capture = metadata_capture(&f, 0, 0, 0, 1024);
    g_assert_cmpuint(kind_count(capture, S3_I2S_TX_ABORT), ==, 1);
    g_assert_cmpuint(kind_count(capture, S3_I2S_TX_COMPLETE), ==, 0);
    const QListEntry *e;
    QLIST_FOREACH_ENTRY(qdict_get_qlist(core_window(capture), "records"), e) {
        QDict *r = qobject_to(QDict, qlist_entry_obj(e));
        if (qdict_get_int(r, "kind") != S3_I2S_TX_ABORT) {
            continue;
        }
        g_assert_cmpuint(qdict_get_int(r, "source_word_id"), ==, 1);
        g_assert_cmpuint(qdict_get_int(r, "epoch"), ==, epoch);
        g_assert_cmpuint(qdict_get_int(r, "reason"), ==, reasons[action]);
        g_assert_cmpuint(qdict_get_int(r, "shifted_bits"), ==, 5);
        g_assert_cmpuint(qdict_get_int(r, "ns"), ==, 900);
        g_assert_cmpuint(qdict_get_int(r, "last_boundary_ns"), ==, 900);
    }
    if (action >= 2) {
        g_assert_cmpuint(qdict_get_int(core_window(capture), "epoch"), >, epoch);
    }
    capture_purity(&f, capture);
    qobject_unref(capture);
    if (action >= 2) {
        configure(&f, 0, &standard);
        source(&f, 0, &standard, 16);
        wr(&f, 0, TX_CONF, conf(&standard) | START);
        capture = metadata_capture(&f, 0, 0, 0, 1024);
        g_assert_cmpuint(kind_count(capture, S3_I2S_TX_LOAD), ==, 2);
        g_assert_cmpuint(kind_count(capture, S3_I2S_TX_COMPLETE), ==, 0);
        QLIST_FOREACH_ENTRY(qdict_get_qlist(core_window(capture), "records"), e) {
            QDict *r = qobject_to(QDict, qlist_entry_obj(e));
            if (qdict_get_int(r, "kind") == S3_I2S_TX_LOAD &&
                qdict_get_int(r, "source_word_id") > 1) {
                g_assert_cmpuint(qdict_get_int(r, "source_word_id"), ==, 2);
                g_assert_cmpuint(qdict_get_int(r, "epoch"), >, epoch);
            }
        }
        qobject_unref(capture);
    }
    qtest_quit(f.q);
    GArray *rx = events(&f, 1);
    g_assert_cmpuint(rx->len, ==, 0);
    g_array_unref(rx);
    cleanup(&f);
}

static void test_metadata_sources(gconstpointer opaque)
{
    unsigned mode = GPOINTER_TO_UINT(opaque);
    bool slave_idle = mode == 2;
    unsigned controller = slave_idle;
    Fixture f = start();
    Format v = standard;
    v.mask = 1;
    configure(&f, 0, slave_idle ? &standard : &v);
    configure(&f, 1, &standard);
    source(&f, controller, &standard, 2);
    if (slave_idle) {
        source(&f, 0, &standard, 64);
    }
    receiver(&f, !controller, 512);
    metadata_peer(&f, controller, 16, false, 64);
    wr(&f, !controller, RX_CONF, conf(&standard) | SLAVE | START);
    if (slave_idle) {
        wr(&f, 1, TX_CONF, conf(&standard) | SLAVE | BIT(13) | START);
        wr(&f, 0, TX_CONF, conf(&standard) | START);
    } else {
        wr(&f, 0, TX_CONF, conf(&v) | START | (mode ? BIT(6) : 0));
    }
    step(&f, 10 * 16 * 200 - 100);
    QDict *capture = metadata_capture(&f, controller, 0, 0, 1024);
    g_assert_cmpuint(kind_count(capture, S3_I2S_TX_LOAD), ==, 2);
    g_assert_cmpuint(kind_count(capture, S3_I2S_TX_COMPLETE), ==, 10);
    uint32_t expected[10];
    unsigned index = 0;
    const QListEntry *e;
    QLIST_FOREACH_ENTRY(qdict_get_qlist(core_window(capture), "records"), e) {
        QDict *r = qobject_to(QDict, qlist_entry_obj(e));
        if (qdict_get_int(r, "kind") != S3_I2S_TX_COMPLETE) {
            continue;
        }
        unsigned i = index++, source, id;
        if (slave_idle) {
            source = i < 2 ? S3_I2S_TX_PAYLOAD : S3_I2S_TX_IDLE_ZERO;
            id = i < 2 ? i + 1 : 0;
            expected[i] = i < 2 ? word(1, i, 16) : 0;
        } else {
            unsigned frame = i / 2;
            unsigned payload = MIN(frame, 1);
            expected[i] = i % 2 && !mode ? 0x5a : word(0, payload, 16);
            source = i % 2 ? (mode ? S3_I2S_TX_MONO_COPY : S3_I2S_TX_SINGLE) :
                     frame < 2 ? S3_I2S_TX_PAYLOAD : S3_I2S_TX_UNDERRUN_REPEAT;
            id = i % 2 && !mode ? 0 : payload + 1;
        }
        g_assert_cmpuint(qdict_get_int(r, "source"), ==, source);
        g_assert_cmpuint(qdict_get_int(r, "source_word_id"), ==, id);
        g_assert_cmpuint(qdict_get_int(r, "word_ordinal"), ==, i + 1);
    }
    g_assert_cmpuint(index, ==, 10);
    capture_purity(&f, capture);
    compare_din_rx(&f, capture, !controller, expected, 10);
    qobject_unref(capture);
    cleanup(&f);
}

static void reject_capture_preserving(Fixture *f, QDict *before,
                                      const char *id, unsigned controller,
                                      uint64_t peer_offset, uint64_t controller_offset,
                                      unsigned count)
{
    QDict *reply = capture_request(f, id, controller, peer_offset, controller_offset, count);
    g_assert_true(qdict_haskey(reply, "error"));
    qobject_unref(reply);
    capture_purity(f, before);
}

static void test_metadata_retention(void)
{
    enum { PAYLOAD_WORDS = 8192, CAPTURE_WORDS = 8300, PEER_CAPACITY = 16384 };
    Fixture f = start();
    Format v = {8, 8, 2, 3, 8};
    configure(&f, 0, &v);
    configure(&f, 1, &v);
    /* A real LOAD and COMPLETE per payload word fills the unchanged16384
     * record ring with fewer physical bits than a mostly-replayed stream.
     * Four physical descriptors remain within the S3 size/owner contract. */
    uint8_t data[PAYLOAD_WORDS];
    for (unsigned i = 0; i < PAYLOAD_WORDS; ++i) {
        data[i] = word(0, i, 8);
    }
    qtest_memwrite(f.q, SRC(0), data, sizeof(data));
    for (unsigned i = 0; i < 4; ++i) {
        descriptor(&f, TD(0) + 12 * i, 2048, 2048, i == 3,
                   SRC(0) + i * 2048, i == 3 ? 0 : TD(0) + 12 * (i + 1));
    }
    dma_start(&f, 0, true, TD(0));
    metadata_peer(&f, 0, 8, false, PEER_CAPACITY);
    QDict *cold = metadata_capture(&f, 0, 0, 0, 1);
    qobject_unref(cold);
    launch(&f, &v, false); /* Actual RX1 FIFO eventually drops, never fabricates. */
    step(&f, CAPTURE_WORDS * 8 * 200 - 100);
    for (unsigned i = 0; i < 4; ++i) {
        g_assert_cmphex(qtest_readl(f.q, TD(0) + 12 * i) & BIT(31), ==, 0);
    }
    g_assert_cmphex(qtest_readl(f.q, DMA(0, 1, 0x08)) & BIT(2), ==, 0);
    QDict *stale = service_reply(&f, "capture-json");
    g_assert_true(qdict_haskey(stale, "error")); /* Lost old offset is explicit. */
    qobject_unref(stale);
    QDict *status = service_object(&f, "status-json");
    QDict *summary = list_dict(qdict_get_qlist(status, "controllers"), 0);
    uint64_t first = qdict_get_int(summary, "first_sequence");
    uint64_t total = qdict_get_int(summary, "total");
    g_assert_cmpuint(first, >, 0);
    g_assert_cmpuint(qdict_get_int(summary, "lost"), ==, first);
    g_assert_cmpuint(qdict_get_int(summary, "count"), ==, 16384);
    g_assert_cmpuint(total - first, ==, 16384);
    QDict *peer = list_dict(qdict_get_qlist(status, "peers"), 0);
    QDict *ps = qdict_get_qdict(peer, "transitions");
    uint64_t peer_first = qdict_get_int(ps, "first_sequence");
    uint64_t peer_total = qdict_get_int(ps, "total");
    g_assert_cmpuint(qdict_get_int(peer, "capture_count"), ==, CAPTURE_WORDS);
    g_assert_false(qdict_get_bool(peer, "capture_overflow"));
    g_assert_cmpuint(qdict_get_int(ps, "capacity"), ==, 2 * PEER_CAPACITY + 16);
    qobject_unref(status);
    QDict *capture = metadata_capture(&f, 0, peer_first, first, 1024);
    g_assert_cmpuint(qlist_size(qdict_get_qlist(core_window(capture), "records")), ==, 1024);
    const QListEntry *entry;
    QLIST_FOREACH_ENTRY(qdict_get_qlist(core_window(capture), "records"), entry) {
        QDict *r = qobject_to(QDict, qlist_entry_obj(entry));
        unsigned kind = qdict_get_int(r, "kind");
        uint64_t id = qdict_get_int(r, "source_word_id");
        if (kind == S3_I2S_TX_LOAD) {
            g_assert_cmpuint(id, <=, PAYLOAD_WORDS);
            g_assert_cmpuint(qdict_get_int(r, "source"), ==, S3_I2S_TX_PAYLOAD);
            g_assert_cmpuint(qdict_get_int(r, "shifted_bits"), ==, 0);
            g_assert_cmpuint(qdict_get_int(r, "ns"), ==, (id - 1) * 1600ULL);
        } else {
            g_assert_cmpuint(kind, ==, S3_I2S_TX_COMPLETE);
            uint64_t ordinal = qdict_get_int(r, "word_ordinal");
            bool payload = ordinal <= PAYLOAD_WORDS;
            g_assert_cmpuint(qdict_get_int(r, "source"), ==,
                            payload ? S3_I2S_TX_PAYLOAD : S3_I2S_TX_UNDERRUN_REPEAT);
            g_assert_cmpuint(id, ==, payload ? ordinal :
                            PAYLOAD_WORDS - 1 + qdict_get_int(r, "slot"));
            g_assert_cmpuint(qdict_get_int(r, "shifted_bits"), ==, 8);
            g_assert_cmpuint(qdict_get_int(r, "ns"), ==, (ordinal - 1) * 1600ULL + 1500);
        }
    }
    capture_purity(&f, capture);
    reject_capture_preserving(&f, capture, "P", 0, peer_first, first - 1, 1);
    reject_capture_preserving(&f, capture, "P", 0, peer_first, total + 1, 1);
    reject_capture_preserving(&f, capture, "P", 0, peer_total + 1, first, 1);
    reject_capture_preserving(&f, capture, "P", 0, peer_first, first, 0);
    reject_capture_preserving(&f, capture, "P", 0, peer_first, first, 1025);
    reject_capture_preserving(&f, capture, "P", 2, peer_first, first, 1);
    reject_capture_preserving(&f, capture, "missing-peer", 0, peer_first, first, 1);
    for (unsigned budget_case = 0; budget_case < 2; ++budget_case) {
        g_autofree char *request = g_strdup_printf(
            "{\"componentId\":\"P\",\"offset\":0,\"count\":%u}",
            budget_case ? 1025 : 0);
        QDict *reply = qtest_qmp(f.q, "{'execute':'qom-set','arguments':"
            "{'path':%s,'property':'capture-request-json','value':%s}}",
            PEER_SERVICE, request);
        g_assert_true(qdict_haskey(reply, "error"));
        qobject_unref(reply);
        capture_purity(&f, capture);
    }
    qobject_unref(capture);
    capture = metadata_capture(&f, 1, peer_first, 0, 1024);
    g_assert_cmpuint(kind_count(capture, S3_I2S_TX_LOAD), ==, 0);
    g_assert_cmpuint(kind_count(capture, S3_I2S_TX_COMPLETE), ==, 0);
    qobject_unref(capture);
    capture = metadata_capture(&f, 0, peer_total, total - 1, 1);
    QList *newest = qdict_get_qlist(core_window(capture), "records");
    g_assert_cmpuint(qlist_size(newest), ==, 1);
    QDict *last = list_dict(newest, 0);
    g_assert_cmpuint(qdict_get_int(last, "sequence"), ==, total - 1);
    g_assert_cmpuint(qdict_get_int(last, "kind"), ==, S3_I2S_TX_COMPLETE);
    g_assert_cmpuint(qdict_get_int(last, "source"), ==, S3_I2S_TX_UNDERRUN_REPEAT);
    g_assert_cmpuint(qdict_get_int(last, "source_word_id"), ==, PAYLOAD_WORDS);
    g_assert_cmpuint(qdict_get_int(last, "word_ordinal"), ==, CAPTURE_WORDS);
    g_assert_cmpuint(qdict_get_int(last, "ns"), ==, CAPTURE_WORDS * 1600ULL - 100);
    capture_purity(&f, capture);
    qobject_unref(capture);
    capture = metadata_capture(&f, 0, peer_total, total, 1024);
    g_assert_cmpuint(qlist_size(qdict_get_qlist(core_window(capture), "records")), ==, 0);
    g_assert_cmpuint(qlist_size(qdict_get_qlist(qdict_get_qdict(
        qdict_get_qdict(capture, "transitions"), "peer"), "records")), ==, 0);
    capture_purity(&f, capture);
    qobject_unref(capture);
    /* Check every independent real DIN event in bounded public windows. */
    for (unsigned offset = 0; offset < CAPTURE_WORDS; offset += 1024) {
        g_autofree char *request = g_strdup_printf(
            "{\"componentId\":\"P\",\"offset\":%u,\"count\":1024}", offset);
        QDict *reply = qtest_qmp(f.q, "{'execute':'qom-set','arguments':"
            "{'path':%s,'property':'capture-request-json','value':%s}}",
            PEER_SERVICE, request);
        g_assert_false(qdict_haskey(reply, "error"));
        qobject_unref(reply);
        QDict *din = service_object(&f, "capture-json");
        QList *records = qdict_get_qlist(din, "events");
        g_assert_cmpuint(qdict_get_int(din, "total"), ==, CAPTURE_WORDS);
        g_assert_cmpuint(qlist_size(records), ==, MIN(1024, CAPTURE_WORDS - offset));
        unsigned index = offset;
        QLIST_FOREACH_ENTRY(records, entry) {
            unsigned source_index = index < PAYLOAD_WORDS ? index :
                                    PAYLOAD_WORDS - 2 + index % 2;
            QDict *r = qobject_to(QDict, qlist_entry_obj(entry));
            g_assert_cmphex(qdict_get_int(r, "sample"), ==, word(0, source_index, 8));
            g_assert_cmpuint(qdict_get_int(r, "ns"), ==, index * 1600ULL + 1500);
            ++index;
        }
        qobject_unref(din);
    }
    qtest_quit(f.q);
    GArray *rx = events(&f, 1);
    g_assert_cmpuint(rx->len, ==, CAPTURE_WORDS);
    for (unsigned i = 0; i < rx->len; ++i) {
        Event r = g_array_index(rx, Event, i);
        unsigned source_index = i < PAYLOAD_WORDS ? i : PAYLOAD_WORDS - 2 + i % 2;
        g_assert_cmphex(r.sample, ==, word(0, source_index, 8));
        g_assert_cmpuint(r.ns, ==, i * 1600ULL + 1500);
    }
    g_array_unref(rx);
    cleanup(&f);
}

static void test_tdm_stride(gconstpointer opaque)
{
    const Format *v = opaque;
    Fixture f = start();
    configure(&f, 0, v);
    configure(&f, 1, v);
    source(&f, 0, v, 32);
    receiver(&f, 1, 512);
    metadata_peer_geometry(&f, 0, v, false, 64);
    launch(&f, v, false);
    unsigned frame_bits = v->slot * v->slots;
    for (unsigned bit = 0; bit < frame_bits * 4; ++bit) {
        unsigned frame = bit / frame_bits;
        unsigned logical = bit % frame_bits;
        unsigned slot = logical / v->slot;
        unsigned lane_bit = logical % v->slot;
        uint32_t serial = serial_word(v, 0, frame, slot);
        g_assert_cmpint(level(&f, 7), ==, !!(serial & BIT(v->slot - lane_bit - 1)));
        g_assert_false(level(&f, 4));
        g_assert_cmpint(level(&f, 5), ==, logical >= v->ws_width);
        step(&f, 100);
        g_assert_true(level(&f, 4));
        if (bit + 1 < frame_bits * 4) {
            step(&f, 100);
        }
    }
    QDict *capture = metadata_capture(&f, 0, 0, 0, 1024);
    unsigned stride = v->skip ? v->slots : ctpop32(v->mask);
    g_assert_cmpuint(kind_count(capture, S3_I2S_TX_LOAD), ==, 4 * stride);
    g_assert_cmpuint(kind_count(capture, S3_I2S_TX_COMPLETE), ==, 4 * v->slots);
    unsigned loaded = 0, completed = 0;
    const QListEntry *entry;
    QLIST_FOREACH_ENTRY(qdict_get_qlist(core_window(capture), "records"), entry) {
        QDict *r = qobject_to(QDict, qlist_entry_obj(entry));
        unsigned kind = qdict_get_int(r, "kind");
        if (kind == S3_I2S_TX_LOAD) {
            unsigned frame = loaded / stride;
            unsigned within = loaded % stride;
            unsigned slot = v->skip ? within : within * 2;
            g_assert_cmpuint(qdict_get_int(r, "source_word_id"), ==, ++loaded);
            g_assert_cmpuint(qdict_get_int(r, "slot"), ==, slot);
            g_assert_cmpuint(qdict_get_int(r, "ns"), ==,
                            (frame * v->slots + slot) * v->slot * 200ULL);
        } else if (kind == S3_I2S_TX_COMPLETE) {
            unsigned frame = completed / v->slots;
            unsigned slot = completed++ % v->slots;
            bool active = v->mask & BIT(slot);
            unsigned id = active ? frame * stride +
                (v->skip ? slot : active_before(v, slot)) + 1 : 0;
            g_assert_cmpuint(qdict_get_int(r, "source_word_id"), ==, id);
            g_assert_cmpuint(qdict_get_int(r, "source"), ==,
                            active ? S3_I2S_TX_PAYLOAD : S3_I2S_TX_SINGLE);
            g_assert_cmpuint(qdict_get_int(r, "slot"), ==, slot);
            g_assert_cmpuint(qdict_get_int(r, "shifted_bits"), ==, v->slot);
            g_assert_cmpuint(qdict_get_int(r, "valid_bits"), ==, v->width);
            g_assert_cmpuint(qdict_get_int(r, "ns"), ==,
                            ((frame * v->slots + slot + 1) * v->slot) * 200ULL - 100);
        }
    }
    uint32_t expected[8];
    for (unsigned i = 0; i < 8; ++i) {
        expected[i] = serial_word(v, 0, i / 2, (i % 2) * 2);
    }
    /* Both RX and peer DIN pack enabled slots only, irrespective of whether
     * TX consumed the disabled-slot buffer entries. Verify real guest RAM. */
    unsigned bytes = bytes_per_sample(v);
    uint8_t actual[32];
    qtest_memread(f.q, DST(1), actual, 8 * bytes);
    for (unsigned i = 0; i < 8; ++i) {
        for (unsigned b = 0; b < bytes; ++b) {
            g_assert_cmphex(actual[i * bytes + b], ==, (expected[i] >> (8 * b)) & 0xff);
        }
    }
    g_assert_cmpuint(qtest_readb(f.q, DST(1) + 8 * bytes), ==, 0xcd);
    capture_purity(&f, capture);
    compare_din_rx(&f, capture, 1, expected, 8);
    qobject_unref(capture);
    cleanup(&f);
}

static void test_metadata_fifo_reset_preserves_word(void)
{
    Fixture f = start();
    configure(&f, 0, &standard);
    configure(&f, 1, &standard);
    source(&f, 0, &standard, 2);
    receiver(&f, 1, 512);
    metadata_peer(&f, 0, 16, false, 64);
    launch(&f, &standard, false);
    step(&f, 900);
    QDict *before = metadata_capture(&f, 0, 0, 0, 1024);
    uint64_t epoch = qdict_get_int(core_window(before), "epoch");
    g_assert_cmpuint(kind_count(before, S3_I2S_TX_LOAD), ==, 1);
    g_assert_cmpuint(kind_count(before, S3_I2S_TX_COMPLETE), ==, 0);
    qobject_unref(before);
    wr(&f, 0, TX_CONF, conf(&standard) | START | BIT(1));
    wr(&f, 0, TX_CONF, conf(&standard) | START);
    QDict *partial = metadata_capture(&f, 0, 0, 0, 1024);
    g_assert_cmpuint(kind_count(partial, S3_I2S_TX_FIFO_RESET), ==, 1);
    g_assert_cmpuint(kind_count(partial, S3_I2S_TX_ABORT), ==, 0);
    g_assert_cmpuint(kind_count(partial, S3_I2S_TX_COMPLETE), ==, 0);
    g_assert_cmpuint(qdict_get_int(core_window(partial), "epoch"), ==, epoch);
    qobject_unref(partial);
    step(&f, 2199);
    partial = metadata_capture(&f, 0, 0, 0, 1024);
    g_assert_cmpuint(kind_count(partial, S3_I2S_TX_COMPLETE), ==, 0);
    qobject_unref(partial);
    step(&f, 1); /* Sixteen actual boundaries now complete the preserved word. */
    wr(&f, 0, TX_CONF, conf(&standard));
    QDict *capture = metadata_capture(&f, 0, 0, 0, 1024);
    g_assert_cmpuint(kind_count(capture, S3_I2S_TX_LOAD), ==, 1);
    g_assert_cmpuint(kind_count(capture, S3_I2S_TX_COMPLETE), ==, 1);
    g_assert_cmpuint(kind_count(capture, S3_I2S_TX_ABORT), ==, 0);
    const QListEntry *entry;
    QLIST_FOREACH_ENTRY(qdict_get_qlist(core_window(capture), "records"), entry) {
        QDict *r = qobject_to(QDict, qlist_entry_obj(entry));
        if (qdict_get_int(r, "kind") == S3_I2S_TX_FIFO_RESET) {
            unsigned flags = qdict_get_int(r, "flags");
            g_assert_cmpuint(flags & S3_I2S_TX_FLAG_CLEAR_FIFO, !=, 0);
            g_assert_cmpuint(flags & (S3_I2S_TX_FLAG_CLEAR_SHIFTER |
                S3_I2S_TX_FLAG_CLEAR_FRAME_CACHE | S3_I2S_TX_FLAG_CLEAR_POSITION), ==, 0);
            g_assert_cmpuint(qdict_get_int(r, "ns"), ==, 900);
        } else if (qdict_get_int(r, "kind") == S3_I2S_TX_COMPLETE) {
            g_assert_cmpuint(qdict_get_int(r, "epoch"), ==, epoch);
            g_assert_cmpuint(qdict_get_int(r, "source_word_id"), ==, 1);
            g_assert_cmpuint(qdict_get_int(r, "shifted_bits"), ==, 16);
            g_assert_cmpuint(qdict_get_int(r, "first_ns"), ==, 0);
            g_assert_cmpuint(qdict_get_int(r, "last_ns"), ==, 3000);
            g_assert_cmpuint(qdict_get_int(r, "first_boundary_ns"), ==, 100);
            g_assert_cmpuint(qdict_get_int(r, "last_boundary_ns"), ==, 3100);
            g_assert_cmpuint(qdict_get_int(r, "ns"), ==, 3100);
        }
    }
    uint32_t expected = word(0, 0, 16);
    capture_purity(&f, capture);
    compare_din_rx(&f, capture, 1, &expected, 1);
    qobject_unref(capture);
    cleanup(&f);
}





int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    static Format formats[32];
    unsigned count = 0;
    static const unsigned widths[] = {8, 16, 24, 32};
    static const char *names[] = {"philips", "msb", "pcm"};
    for (unsigned mode = 0; mode < 3; ++mode) {
        for (unsigned i = 0; i < G_N_ELEMENTS(widths); ++i) {
            unsigned width = widths[i];
            Format *v = &formats[count++];
            *v = (Format){width, width, 2, 3, mode == 2 ? 1 : width,
                          mode == 0};
            g_autofree char *name = g_strdup_printf("/esp32s3/i2s/loopback-only/%s/%u", names[mode], width);
            qtest_add_data_func(name, v, test_vectors);
        }
    }
    for (unsigned mode = 0; mode < 3; ++mode) {
        for (unsigned symmetric = 0; symmetric < 2; ++symmetric) {
            Format *v = &formats[count++];
            *v = (Format){.width = 16, .slot = 16, .slots = 2,
                .mask = 3, .ws_width = mode == 2 ? 1 : 16,
                .philips = mode == 0, .slave_loop = true,
                .rx_master = symmetric};
            g_autofree char *name = g_strdup_printf(
                "/esp32s3/i2s/sdk-duplex/%s/%s", names[mode],
                symmetric ? "rx-master-tx-slave" : "both-slaves-sig-loop");
            qtest_add_data_func(name, v, test_vectors);
        }
    }
    formats[count] = (Format){16, 16, 2, 3, 16, false, true};
    qtest_add_data_func("/esp32s3/i2s/loopback-only/mono-first", &formats[count++], test_vectors);
    formats[count] = (Format){24, 32, 2, 3, 32, false, false, true};
    qtest_add_data_func("/esp32s3/i2s/loopback-only/24-in-32", &formats[count++], test_vectors);
    /* 24_FILL_EN transfers all four bytes, including a nonzero upper byte.
     * LEFT_ALIGN must not shift the effective 32-bit sample by eight. */
    formats[count] = (Format){24, 32, 2, 3, 32, false, false, true, true};
    qtest_add_data_func("/esp32s3/i2s/loopback-only/24-in-32-left-aligned", &formats[count++], test_vectors);
    formats[count] = (Format){16, 32, 2, 3, 32, false, false, false, true};
    qtest_add_data_func("/esp32s3/i2s/loopback-only/left-aligned", &formats[count++], test_vectors);
    formats[count] = (Format){16, 16, 8, 0xa5, 1};
    qtest_add_data_func("/esp32s3/i2s/loopback-only/tdm-mask-128-bit", &formats[count++], test_vectors);
    formats[count] = (Format){16, 16, 2, 3, 16, false, false, false, false, true};
    qtest_add_data_func("/esp32s3/i2s/loopback-only/big-endian", &formats[count++], test_vectors);
    formats[count] = (Format){16, 16, 2, 3, 16, false, false, false, false, false, true};
    qtest_add_data_func("/esp32s3/i2s/loopback-only/lsb-first", &formats[count++], test_vectors);
    qtest_add_func("/esp32s3/i2s/gdma/partial-eof-next-cursor-ring", test_descriptor_eof_continue);
    qtest_add_func("/esp32s3/i2s/underflow/full-frame-repeat", test_underflow_full_frame);
    qtest_add_func("/esp32s3/i2s/overflow/real-fifo", test_real_fifo_overflow);
    qtest_add_func("/esp32s3/i2s/clock/gate-divider-stop-reset", test_clock_gate_divider_lifecycle);
    qtest_add_func("/esp32s3/i2s/clock/local-rx-and-inactive-pll-hold", test_local_rx_clock_hold);
    qtest_add_func("/esp32s3/i2s/clock/external-mclk-physical-divider", test_external_mclk_metrology);
    qtest_add_data_func("/esp32s3/i2s/clock/external-unknown-unconsumed", GUINT_TO_POINTER(0), test_external_mclk_unknown);
    qtest_add_data_func("/esp32s3/i2s/clock/external-unknown-output-consumed", GUINT_TO_POINTER(1), test_external_mclk_unknown);
    qtest_add_data_func("/esp32s3/i2s/clock/external-unknown-stream-consumed", GUINT_TO_POINTER(2), test_external_mclk_unknown);
    for (unsigned mode = 0; mode < 6; ++mode) {
        static const char *receivers[] = {"rx0-slave", "rx1-slave", "rx0-shared"};
        g_autofree char *name = g_strdup_printf(
            "/esp32s3/i2s/clock/ext-module-%s-held-%u",
            receivers[mode >> 1], mode & 1);
        qtest_add_data_func(name, GUINT_TO_POINTER(mode), test_external_slave_quiet);
    }
    qtest_add_data_func("/esp32s3/i2s/converter/i2s0-explicit-block", GUINT_TO_POINTER(0), test_converter_boundary);
    qtest_add_data_func("/esp32s3/i2s/converter/i2s1-not-present", GUINT_TO_POINTER(1), test_converter_boundary);
    qtest_add_data_func("/esp32s3/i2s/raw-pdm/physical-stereo", GUINT_TO_POINTER(0), test_raw_pdm_physical);
    qtest_add_data_func("/esp32s3/i2s/raw-pdm/paired-slave-rx-ws-only", GUINT_TO_POINTER(1), test_raw_pdm_physical);
    for (unsigned mode = 0; mode < 5; ++mode) {
        static const char *names[] = {"tx-master", "tx-slave", "rx-master-tx-slave",
                                      "raw-master-channels", "raw-slave-channels"};
        g_autofree char *name = g_strdup_printf("/esp32s3/i2s/metadata/complete/%s", names[mode]);
        qtest_add_data_func(name, GUINT_TO_POINTER(mode), test_metadata_completion);
    }
    for (unsigned action = 0; action < 4; ++action) {
        static const char *names[] = {"stop", "gate", "stream-reset", "device-reset"};
        g_autofree char *name = g_strdup_printf("/esp32s3/i2s/metadata/partial-abort/%s", names[action]);
        qtest_add_data_func(name, GUINT_TO_POINTER(action), test_metadata_abort);
    }
    for (unsigned mode = 0; mode < 3; ++mode) {
        static const char *names[] = {"single-repeat", "mono-copy-repeat", "slave-idle-zero"};
        g_autofree char *name = g_strdup_printf("/esp32s3/i2s/metadata/source/%s", names[mode]);
        qtest_add_data_func(name, GUINT_TO_POINTER(mode), test_metadata_sources);
    }
    qtest_add_func("/esp32s3/i2s/metadata/retention-scoped-pure-bounded-windows", test_metadata_retention);
    static Format strides[8];
    for (unsigned i = 0; i < G_N_ELEMENTS(widths); ++i) {
        for (unsigned skip = 0; skip < 2; ++skip) {
            Format *v = &strides[2 * i + skip];
            *v = (Format){.width = widths[i], .slot = widths[i],
                .slots = 4, .mask = 5, .ws_width = 1, .skip = skip};
            g_autofree char *name = g_strdup_printf(
                "/esp32s3/i2s/tdm/stride/%s-%u",
                skip ? "all-slots" : "packed-enabled", widths[i]);
            qtest_add_data_func(name, v, test_tdm_stride);
        }
    }
    qtest_add_func("/esp32s3/i2s/metadata/fifo-reset-preserves-loaded-word",
                   test_metadata_fifo_reset_preserves_word);
    return g_test_run();
}
