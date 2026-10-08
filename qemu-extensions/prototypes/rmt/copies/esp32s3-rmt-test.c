/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Native esp32s3 register consumer, not a standalone RMT mock.
 * Sources: IDF6.1 S3 rmt_struct.h/rmt_ll.h/esp32s3.peripherals.ld;
 * ESP32-S3 TRM v1.8 ch37; docs/plans/peripherals-electrical.md
 * RMT-01/02, NET-02..06, CORE-02..04.
 * All RX stimulus traverses GPIO matrix, physical pads and ProjectDocument v3.
 * Closed pulse halves are exact; the undocumented idle trailer is not normalized
 * or used as an oracle. DMA qualifies internal-DRAM descriptor chains only;
 * RX demodulation, PSRAM and independent hardware-reference timing are NOT
 * qualified. Named WS functional/NEC envelope peers are real powered graph
 * consumers/sources, not optical/AGC/PWM or measured-silicon models.
 * RMT-176 affects revisions 0.0/0.1/0.2 (no fix): continuous-mode tests use
 * IDF's idle_out_en workaround, not an assumed end-marker idle level.
 * https://docs.espressif.com/projects/esp-chip-errata/en/latest/esp32s3/03-errata-description/index.html#rmt-176-the-idle-state-signal-level-might-run-into-error-in-rmt-continuous-tx-mode
 */
#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qapi/error.h"
#include "qapi/qmp/qjson.h"
#include "qapi/qmp/qlist.h"
#include "qapi/qmp/qnum.h"
#include "libqtest.h"
#include "hw/sysbus.h"
#include "hw/gpio/esp32s3_gpio.h"
#include "hw/gpio/esp32s3_iomux.h"

#define BASE 0x60016000ULL
/* Linker PROVIDE(RMTMEM=0x60016800), not the obsolete +0x400 candidate. */
#define MEM(ch, i) (BASE + 0x800 + 4 * (48 * (ch) + (i)))
#define TX_CONF(ch) (0x20 + 4 * (ch))
#define RX_CONF0(ch) (0x30 + 8 * (ch))
#define RX_CONF1(ch) (0x34 + 8 * (ch))
#define TX_STATUS(ch) (0x50 + 4 * (ch))
#define RX_STATUS(ch) (0x60 + 4 * (ch))
#define RAW 0x70
#define ST 0x74
#define ENA 0x78
#define CLR 0x7c
#define CARRIER(ch) (0x80 + 4 * (ch))
#define TX_LIM(ch) (0xa0 + 4 * (ch))
#define RX_LIM(ch) (0xb0 + 4 * (ch))
#define SYS_CONF 0xc0
#define SYNC 0xc4
#define REF_RST 0xc8
#define TX_DONE(ch) BIT(ch)
#define TX_ERR(ch) BIT(4 + (ch))
#define TX_THR(ch) BIT(8 + (ch))
#define TX_LOOP(ch) BIT(12 + (ch))
#define RX_DONE(ch) BIT(16 + (ch))
#define RX_ERR(ch) BIT(20 + (ch))
#define RX_THR(ch) BIT(24 + (ch))
#define TX_START BIT(0)
#define TX_STOP BIT(7)
#define TX_UPDATE BIT(24)
#define RX_UPDATE BIT(15)
#define RX_OWNER BIT(3)
#define FSM_MASK (7U << 22)
#define GPIO(a) (0x60004000ULL + (a))
#define SYS(a) (0x600c0000ULL + (a))
#define PAD(n) (0x60009000ULL + IO_MUX_GPIOn_REG_OFFSET(n))
#define INPUT (R_IO_MUX_GPIOn_FUN_IE_MASK | (ESP32S3_IOMUX_MCU_SEL_GPIO << R_IO_MUX_GPIOn_MCU_SEL_SHIFT))
static QTestState *s;
static int64_t now;
static uint32_t tx_config[4];

static uint32_t rd(unsigned reg) { return qtest_readl(s, BASE + reg); }
static void wr(unsigned reg, uint32_t value) { qtest_writel(s, BASE + reg, value); }
static void step(int64_t ns) { now = qtest_clock_step(s, ns); }
static uint32_t symbol(unsigned d0, bool l0, unsigned d1, bool l1)
{
    return d0 | (l0 ? BIT(15) : 0) | (d1 << 16) | (l1 ? BIT(31) : 0);
}
static void memory(unsigned ch, unsigned i, uint32_t value)
{
    qtest_writel(s, MEM(ch, i), value);
}
static uint32_t captured(unsigned ch, unsigned i)
{
    return qtest_readl(s, MEM(ch + 4, i));
}
static void source(unsigned sel, unsigned integral, unsigned a, unsigned b)
{
    wr(SYS_CONF, BIT(0) | BIT(3) | ((integral - 1) << 4) |
       (a << 12) | (b << 18) | (sel << 24) | BIT(26));
}
static void tx_setup(unsigned ch, unsigned divider, uint32_t flags)
{
    tx_config[ch] = BIT(6) | ((divider & 255) << 8) | BIT(16) | flags;
    wr(TX_CONF(ch), tx_config[ch] | BIT(1) | BIT(2) | TX_UPDATE);
    wr(TX_CONF(ch), tx_config[ch] | TX_UPDATE);
}
static void launch(unsigned ch)
{
    wr(TX_CONF(ch), tx_config[ch] | TX_UPDATE);
    wr(TX_CONF(ch), tx_config[ch] | TX_START);
}
static void stop(unsigned ch)
{
    wr(TX_CONF(ch), tx_config[ch] | TX_STOP | TX_UPDATE);
}
static void rx_setup(unsigned ch, unsigned divider, unsigned idle,
                     uint32_t flags)
{
    wr(RX_CONF0(ch), (divider & 255) | (idle << 8) | BIT(24));
    wr(RX_CONF1(ch), flags | BIT(1) | BIT(2) | RX_UPDATE);
    wr(RX_CONF1(ch), flags | BIT(0) | RX_UPDATE);
}
static void rx_disable(unsigned ch)
{
    wr(RX_CONF1(ch), RX_OWNER | RX_UPDATE);
}
static void rx_to_apb(unsigned ch)
{
    wr(RX_CONF1(ch), RX_UPDATE);
}
static void irq(uint32_t raw, uint32_t enabled)
{
    g_assert_cmphex(rd(RAW), ==, raw);
    g_assert_cmphex(rd(ST), ==, raw & enabled);
    g_assert_cmpint(qtest_get_irq(s, 0), ==, !!(raw & enabled));
}

/* Same public v3 schema as the permanent electrical QTest. Two separate nets
 * allow TX0/1 phase observation without shorting their push-pull outputs. */
static QDict *graph_document(bool connected)
{
    g_autofree char *json = g_strdup_printf(
        "{\"version\":3,\"id\":\"rmt-native\",\"name\":\"RMT native\","
        "\"profile\":{\"chip\":\"esp32s3\",\"board\":\"esp32-s3-devkitc-1\",\"module\":\"esp32-s3-wroom-1\"},"
        "\"firmware\":{},\"runtime\":{\"electrical\":{\"driver_profile\":\"s3-explicit-finite-v1\",\"mode\":\"dc\"}},"
        "\"geometry\":{\"components\":{},\"nets\":{}},\"components\":["
        "{\"id\":\"U\",\"name\":\"MCU\",\"kind\":\"mcu\",\"type\":\"mcu\",\"parameters\":{},\"terminals\":["
        "{\"id\":\"U.vdd\",\"name\":\"vdd\",\"role\":\"vdd\",\"domain\":\"power\",\"direction\":\"input\"},"
        "{\"id\":\"U.gnd\",\"name\":\"gnd\",\"role\":\"gnd\",\"domain\":\"ground\",\"direction\":\"input\"},"
        "{\"id\":\"U.io4\",\"name\":\"io4\",\"role\":\"io4\",\"domain\":\"digital\",\"direction\":\"inout\",\"gpio\":4},"
        "{\"id\":\"U.io5\",\"name\":\"io5\",\"role\":\"io5\",\"domain\":\"digital\",\"direction\":\"inout\",\"gpio\":5},"
        "{\"id\":\"U.io6\",\"name\":\"io6\",\"role\":\"io6\",\"domain\":\"digital\",\"direction\":\"inout\",\"gpio\":6},"
        "{\"id\":\"U.io7\",\"name\":\"io7\",\"role\":\"io7\",\"domain\":\"digital\",\"direction\":\"inout\",\"gpio\":7}]},"
        "{\"id\":\"G\",\"name\":\"Ground\",\"kind\":\"ground\",\"type\":\"ground\",\"parameters\":{},\"terminals\":["
        "{\"id\":\"G.ref\",\"name\":\"ref\",\"role\":\"ref\",\"domain\":\"ground\",\"direction\":\"unspecified\"}]},"
        "{\"id\":\"V\",\"name\":\"Supply\",\"kind\":\"voltage-source\",\"type\":\"voltage-source\",\"parameters\":{\"voltage\":{\"value\":3.3,\"unit\":\"V\"}},\"terminals\":["
        "{\"id\":\"V.p\",\"name\":\"p\",\"role\":\"p\",\"domain\":\"power\",\"direction\":\"unspecified\"},"
        "{\"id\":\"V.n\",\"name\":\"n\",\"role\":\"n\",\"domain\":\"ground\",\"direction\":\"unspecified\"}]},"
        "{\"id\":\"R0\",\"name\":\"R0\",\"kind\":\"resistor\",\"type\":\"resistor\",\"parameters\":{\"resistance\":{\"value\":10000,\"unit\":\"ohm\"}},\"terminals\":["
        "{\"id\":\"R0.a\",\"name\":\"a\",\"role\":\"a\",\"domain\":\"passive\",\"direction\":\"passive\"},"
        "{\"id\":\"R0.b\",\"name\":\"b\",\"role\":\"b\",\"domain\":\"passive\",\"direction\":\"passive\"}]},"
        "{\"id\":\"R1\",\"name\":\"R1\",\"kind\":\"resistor\",\"type\":\"resistor\",\"parameters\":{\"resistance\":{\"value\":10000,\"unit\":\"ohm\"}},\"terminals\":["
        "{\"id\":\"R1.a\",\"name\":\"a\",\"role\":\"a\",\"domain\":\"passive\",\"direction\":\"passive\"},"
        "{\"id\":\"R1.b\",\"name\":\"b\",\"role\":\"b\",\"domain\":\"passive\",\"direction\":\"passive\"}]}],"
        "\"nets\":[{\"id\":\"gnd\",\"name\":\"gnd\",\"endpoints\":[\"G.ref\",\"V.n\",\"U.gnd\",\"R0.b\",\"R1.b\"]},"
        "{\"id\":\"vdd\",\"name\":\"vdd\",\"endpoints\":[\"V.p\",\"U.vdd\"]},"
        "{\"id\":\"pulse0\",\"name\":\"pulse0\",\"endpoints\":[\"U.io5\",\"R0.a\"%s]},"
        "{\"id\":\"pulse1\",\"name\":\"pulse1\",\"endpoints\":[\"U.io6\",\"U.io7\",\"R1.a\"]}%s]}",
        connected ? ",\"U.io4\"" : "",
        connected ? "" : ",{\"id\":\"tx0\",\"name\":\"tx0\",\"endpoints\":[\"U.io4\"]}");
    QObject *o = qobject_from_json(json, &error_abort);
    g_assert_nonnull(qobject_to(QDict, o));
    return qobject_to(QDict, o);
}
static void apply_graph(QDict *p)
{
    /* Apply is paused-only. QTest has dummy CPUs: cont enables the virtual
     * clock without executing firmware, and stop makes later Apply quiescent. */
    QDict *r = qtest_qmp(s, "{'execute':'stop'}");
    g_assert_false(qdict_haskey(r, "error"));
    qobject_unref(r);
    GString *json = qobject_to_json(QOBJECT(p));
    r = qtest_qmp(s, "{'execute':'qom-set','arguments':{'path':'/machine/soc/electrical','property':'project-json','value':%s}}", json->str);
    g_assert_false(qdict_haskey(r, "error"));
    qobject_unref(r);
    g_string_free(json, true);
    r = qtest_qmp(s, "{'execute':'cont'}");
    g_assert_false(qdict_haskey(r, "error"));
    qobject_unref(r);
}
static void graph(bool connected)
{
    QDict *p = graph_document(connected);
    apply_graph(p);
    qobject_unref(p);
}
static double sample(unsigned pad, bool high)
{
    uint32_t value = qtest_readl(s, GPIO(A_GPIO_IN));
    g_assert_cmphex(value & BIT(pad), ==, high ? BIT(pad) : 0);
    QDict *r = qtest_qmp(s, "{'execute':'qom-get','arguments':{'path':'/machine/soc/electrical','property':'snapshot-json'}}");
    g_assert_false(qdict_haskey(r, "error"));
    QObject *o = qobject_from_json(qdict_get_str(r, "return"), &error_abort);
    QDict *snap = qobject_to(QDict, o);
    g_assert_nonnull(snap);
    const QListEntry *e;
    bool found = false;
    double measured = 0;
    QLIST_FOREACH_ENTRY(qdict_get_qlist(snap, "pads"), e) {
        QDict *p = qobject_to(QDict, qlist_entry_obj(e));
        if (qdict_get_int(p, "gpio") == pad) {
            QNum *voltage = qobject_to(QNum, qdict_get(p, "voltage_v"));
            g_assert_true(qdict_get_bool(p, "valid"));
            g_assert_nonnull(voltage);
            double v = qnum_get_double(voltage);
            g_assert_true(high ? v > 2.4 : v < 0.8);
            measured = v;
            found = true;
        }
    }
    g_assert_true(found);
    g_test_message("sample ns=%" PRId64 " GPIO%u=%u raw=0x%08x tx0=0x%08x rx0=0x%08x",
                   now, pad, high, rd(RAW), rd(TX_STATUS(0)), rd(RX_STATUS(0)));
    qobject_unref(o);
    qobject_unref(r);
    return measured;
}
static void setup(void)
{
    s = qtest_initf("-machine esp32s3 -S -L pc-bios -global driver=esp32s3.gpio,property=strap_mode,value=0x00");
    now = qtest_clock_step(s, 0);
    qtest_irq_intercept_out_named(s, "/machine/soc/rmt", SYSBUS_DEVICE_GPIO_IRQ);
    qtest_writel(s, SYS(0x60), 0x00028401); /* CORE-02 PLL/APB 80MHz */
    qtest_writel(s, SYS(0x18), qtest_readl(s, SYS(0x18)) | BIT(9));
    source(1, 1, 0, 0);
    for (unsigned pad = 4; pad <= 7; pad++) {
        qtest_writel(s, PAD(pad), INPUT);
    }
    for (unsigned ch = 0; ch < 2; ch++) {
        unsigned out = 4 + 2 * ch, in = out + 1;
        qtest_writel(s, GPIO(GPIO_FUNC_OUT_SEL_CFG_OFFSET(out)), 81 + ch);
        qtest_writel(s, GPIO(GPIO_FUNC_IN_SEL_CFG_OFFSET(81 + ch)), BIT(7) | in);
        tx_setup(ch, 80, 0);
    }
    graph(true);
    wr(ENA, 0);
    wr(CLR, UINT32_MAX);
}
static void software_drive(bool high)
{
    qtest_writel(s, GPIO(high ? A_GPIO_OUT_W1TS : A_GPIO_OUT_W1TC), BIT(4));
}
static void software_output(void)
{
    qtest_writel(s, GPIO(GPIO_FUNC_OUT_SEL_CFG_OFFSET(4)), GPIO_FUNC_OUT_SEL_NONE);
    qtest_writel(s, GPIO(A_GPIO_ENABLE_W1TS), BIT(4));
    software_drive(false);
}

static void test_symbols_irq(void)
{
    setup();
    memory(0, 0, symbol(7, true, 11, false));
    memory(0, 1, symbol(5, true, 0, false));
    memory(0, 2, 0);
    wr(TX_LIM(0), 1);
    rx_setup(0, 80, 40, RX_OWNER);
    launch(0);
    irq(0, 0); sample(5, true);
    g_assert_cmphex(rd(TX_STATUS(0)) & FSM_MASK, !=, 0);
    step(6999); sample(5, true); irq(0, 0);
    step(1); sample(5, false); irq(0, 0);
    step(10999); sample(5, false); irq(0, 0);
    step(1); sample(5, true); irq(TX_THR(0), 0);
    wr(ENA, TX_THR(0)); irq(TX_THR(0), TX_THR(0));
    wr(CLR, TX_DONE(0)); irq(TX_THR(0), TX_THR(0));
    step(4999); sample(5, true);
    step(1); sample(5, false); irq(TX_THR(0) | TX_DONE(0), TX_THR(0));
    g_assert_cmphex(rd(TX_STATUS(0)) & FSM_MASK, ==, 0);
    wr(ENA, TX_DONE(0)); irq(TX_THR(0) | TX_DONE(0), TX_DONE(0));
    wr(CLR, TX_DONE(0)); irq(TX_THR(0), TX_DONE(0));
    wr(CLR, TX_THR(0)); irq(0, TX_DONE(0));
    step(40000); g_assert_cmphex(rd(RAW) & RX_DONE(0), ==, 0);
    step(1000); g_assert_cmphex(rd(RAW) & RX_DONE(0), ==, RX_DONE(0));
    rx_to_apb(0);
    g_assert_cmphex(captured(0, 0), ==, symbol(7, true, 11, false));
    g_assert_cmphex(captured(0, 1) & 0xffff, ==, symbol(5, true, 0, false));
    g_test_message("RX closed halves: 0x%08x 0x%04x", captured(0, 0), captured(0, 1) & 0xffff);
    qtest_quit(s);
}

static void test_dividers_sources(void)
{
    static const struct { unsigned sel, integral, a, b, divider; int64_t tick; } rows[] = {
        {1, 1, 0, 0, 80, 1000}, {1, 1, 0, 0, 40, 500},
        {3, 1, 0, 0, 40, 1000}, {3, 2, 0, 0, 40, 2000},
        {1, 1, 1, 2, 80, 1500}, {3, 1, 0, 0, 256, 6400},
        {2, 1, 0, 0, 70, 4000}, /* CORE-02 nominal RC_FAST=17.5MHz */
    };
    for (unsigned i = 0; i < G_N_ELEMENTS(rows); i++) {
        setup();
        source(rows[i].sel, rows[i].integral, rows[i].a, rows[i].b);
        tx_setup(0, rows[i].divider, 0);
        wr(REF_RST, BIT(0));
        memory(0, 0, symbol(9, true, 13, false)); memory(0, 1, 0);
        launch(0);
        step(9 * rows[i].tick - 1); sample(5, true); irq(0, 0);
        step(1); sample(5, false);
        step(13 * rows[i].tick - 1); irq(0, 0);
        step(1); irq(TX_DONE(0), 0);
        g_test_message("source=%u group=%u+%u/%u divider=%u tick_ns=%" PRId64 " done_ns=%" PRId64,
                       rows[i].sel, rows[i].integral, rows[i].a, rows[i].b,
                       rows[i].divider, rows[i].tick, now);
        qtest_quit(s);
    }
    setup(); source(1, 1, 1, 2); tx_setup(0, 1, 0);
    memory(0, 0, symbol(14, true, 18, false)); memory(0, 1, 0);
    launch(0);
    /* 18.75ns ticks: ceil(262.5)=263, cumulative end=600, not 601.
     * Both widths satisfy TRM equation 37.1; no invalid tiny pulse oracle. */
    step(262); sample(5, true); step(1); sample(5, false);
    step(336); irq(0, 0); step(1); irq(TX_DONE(0), 0);
    g_test_message("fractional tick_ns=18.75 edge_ns=263 done_ns=%" PRId64, now);
    qtest_quit(s);
    setup(); memory(0, 0, symbol(50, true, 50, false)); memory(0, 1, 0);
    launch(0); step(10000);
    source(3, 1, 0, 0); /* live group switch: remaining ticks, not elapsed ns */
    step(79999); sample(5, true); step(1); sample(5, false);
    step(99999); irq(0, 0); step(1); irq(TX_DONE(0), 0);
    g_test_message("live APB->XTAL edge_ns=90000 done_ns=%" PRId64, now);
    qtest_quit(s);
}

static void test_rx_first_edge_idle_filter(void)
{
    setup(); software_output();
    rx_setup(0, 80, 100, RX_OWNER);
    step(37000); /* must not create a low preamble */
    software_drive(true); step(7000); software_drive(false);
    step(11000); software_drive(true); step(5000); software_drive(false);
    step(100000); g_assert_cmphex(rd(RAW) & RX_DONE(0), ==, 0);
    step(1000); g_assert_cmphex(rd(RAW) & RX_DONE(0), ==, RX_DONE(0));
    rx_to_apb(0);
    g_assert_cmphex(captured(0, 0), ==, symbol(7, true, 11, false));
    g_assert_cmphex(captured(0, 1) & 0xffff, ==, 0x8005);
    qtest_quit(s);

    for (unsigned xtal = 0; xtal < 2; xtal++) {
        setup(); software_output();
        if (xtal) { source(3, 1, 0, 0); }
        /* TRM and pinned rmt_rx.c use group rmt_sclk, not APB.
         * Both thresholds mean 2us. The XTAL 1.5us spike distinguishes
         * correct filtering from an incorrect fixed-APB 1us threshold. */
        rx_setup(0, xtal ? 40 : 80, 100,
                 RX_OWNER | BIT(4) | ((xtal ? 80 : 160) << 5));
        software_drive(true); step(xtal ? 1500 : 1000);
        software_drive(false); step(6000);
        software_drive(true); step(7000); software_drive(false);
        step(9000); software_drive(true); step(5000); software_drive(false);
        step(103000);
        g_assert_cmphex(rd(RAW) & (RX_DONE(0) | RX_ERR(0)), ==, RX_DONE(0));
        rx_to_apb(0);
        g_assert_cmphex(captured(0, 0), ==, symbol(7, true, 9, false));
        g_assert_cmphex(captured(0, 1) & 0xffff, ==, 0x8005);
        qtest_quit(s);
    }
}

static void test_rx_owner_overflow_threshold(void)
{
    setup(); software_output();
    rx_setup(0, 80, 100, 0); /* software owns RAM: receiver cannot write */
    software_drive(true); step(5000); software_drive(false); step(5000);
    g_assert_cmphex(rd(RAW) & (RX_ERR(0) | RX_DONE(0)), ==, RX_ERR(0));
    g_assert_cmphex(rd(RX_STATUS(0)) & BIT(25), ==, BIT(25));
    wr(ENA, RX_ERR(0)); g_assert_true(qtest_get_irq(s, 0));
    wr(CLR, RX_DONE(0)); g_assert_true(qtest_get_irq(s, 0));
    wr(CLR, RX_ERR(0)); g_assert_false(qtest_get_irq(s, 0));
    qtest_quit(s);

    setup(); software_output(); wr(RX_LIM(0), 24);
    rx_setup(0, 80, 100, RX_OWNER);
    software_drive(true);
    for (unsigned half = 0; half < 96; half++) {
        step(5000); software_drive(!(half % 2 == 0));
        if (half == 46) { g_assert_cmphex(rd(RAW) & RX_THR(0), ==, 0); }
        if (half == 47) {
            g_assert_cmphex(rd(RAW) & RX_THR(0), ==, RX_THR(0));
            wr(CLR, RX_THR(0));
        }
    }
    step(5000); software_drive(false);
    g_assert_cmphex(rd(RX_STATUS(0)) & BIT(26), ==, BIT(26));
    g_assert_cmphex(rd(RAW) & (RX_ERR(0) | RX_DONE(0)), ==, RX_ERR(0));
    step(200000); g_assert_cmphex(rd(RAW) & RX_DONE(0), ==, 0);
    rx_to_apb(0);
    for (unsigned i = 0; i < 48; i++) {
        g_assert_cmphex(captured(0, i), ==, symbol(5, true, 5, false));
    }
    qtest_quit(s);
}

static void test_wrap_refill_underflow(void)
{
    setup(); tx_setup(0, 80, BIT(4)); wr(TX_LIM(0), 24);
    for (unsigned i = 0; i < 48; i++) { memory(0, i, symbol(5, true, 7, false)); }
    launch(0);
    step(288000 - 1); g_assert_cmphex(rd(RAW), ==, 0);
    step(1); g_assert_cmphex(rd(RAW), ==, TX_THR(0));
    for (unsigned i = 0; i < 24; i++) { memory(0, i, symbol(9, true, 11, false)); }
    wr(CLR, TX_THR(0));
    step(288000); sample(5, true);
    /* The second half is reusable only after the second threshold. */
    memory(0, 24, 0);
    g_assert_cmphex(rd(RAW) & TX_DONE(0), ==, 0);
    step(8999); sample(5, true); step(1); sample(5, false);
    step(470999); g_assert_cmphex(rd(RAW) & TX_DONE(0), ==, 0);
    step(1); g_assert_cmphex(rd(RAW) & TX_DONE(0), ==, TX_DONE(0));
    g_test_message("wrap: 48 original +24 refill symbols; end_ns=%" PRId64, now);
    qtest_quit(s);

    setup();
    for (unsigned i = 0; i < 48; i++) { memory(0, i, symbol(5, true, 7, false)); }
    launch(0); step(576000);
    g_assert_cmphex(rd(RAW) & (TX_DONE(0) | TX_ERR(0)), ==, TX_ERR(0));
    g_assert_cmphex(rd(TX_STATUS(0)) & BIT(25), ==, BIT(25));
    qtest_quit(s);
}

static void test_stop_reset_gate(void)
{
    for (unsigned mode = 0; mode < 4; mode++) {
        setup(); memory(0, 0, symbol(50, true, 50, false)); memory(0, 1, 0);
        wr(ENA, TX_DONE(0)); launch(0); step(10000); sample(5, true);
        if (mode == 0) {
            stop(0); wr(CLR, UINT32_MAX); step(200000);
            irq(0, TX_DONE(0)); sample(5, false);
        } else if (mode == 1) {
            uint32_t rst = qtest_readl(s, SYS(0x20));
            qtest_writel(s, SYS(0x20), rst | BIT(9));
            qtest_writel(s, SYS(0x20), rst & ~BIT(9));
            step(200000); irq(0, 0);
            g_assert_cmphex(rd(TX_STATUS(0)) & FSM_MASK, ==, 0);
        } else {
            uint32_t saved = mode == 2 ? rd(SYS_CONF) : qtest_readl(s, SYS(0x18));
            if (mode == 2) { wr(SYS_CONF, saved & ~BIT(26)); }
            else { qtest_writel(s, SYS(0x18), saved & ~BIT(9)); }
            step(200000); irq(0, TX_DONE(0));
            g_assert_cmphex(rd(TX_STATUS(0)) & FSM_MASK, ==, 0);
            if (mode == 2) { wr(SYS_CONF, saved); }
            else { qtest_writel(s, SYS(0x18), saved); }
            /* Native gate policy cancels the transfer, not a synthetic DONE. */
            step(200000); irq(0, TX_DONE(0));
        }
        /* Restart after cancellation must not inherit the old callback. */
        source(1, 1, 0, 0); tx_setup(0, 80, 0); wr(ENA, TX_DONE(0));
        memory(0, 0, symbol(7, true, 11, false)); memory(0, 1, 0); launch(0);
        step(17999); irq(0, TX_DONE(0)); step(1); irq(TX_DONE(0), TX_DONE(0));
        wr(CLR, TX_DONE(0)); step(200000); irq(0, TX_DONE(0));
        qtest_quit(s);
    }
    setup(); software_output(); rx_setup(0, 80, 100, RX_OWNER);
    software_drive(true); step(7000); rx_disable(0); wr(CLR, UINT32_MAX);
    step(200000); irq(0, 0); qtest_quit(s);
}

static void test_carrier_sync_loop(void)
{
    setup();
    /* Pinned LL writes carrier ticks directly; zero encodes 65536.
     * Carrier uses rmt_sclk, not the channel's divided symbol clock. */
    wr(CARRIER(0), (80 << 16) | 160);
    tx_setup(0, 80, BIT(20) | BIT(21) | BIT(22));
    memory(0, 0, symbol(6, true, 4, false)); memory(0, 1, 0);
    launch(0); sample(5, true);
    step(999); sample(5, true); step(1); sample(5, false);
    step(1999); sample(5, false); step(1); sample(5, true);
    step(1000); sample(5, false); step(2000); sample(5, false);
    step(4000); irq(TX_DONE(0), 0); sample(5, false); qtest_quit(s);

    setup();
    memory(0, 0, symbol(7, true, 11, false)); memory(0, 1, 0);
    memory(1, 0, symbol(7, true, 11, false)); memory(1, 1, 0);
    wr(SYNC, BIT(0) | BIT(1) | BIT(4));
    launch(0); step(10000); irq(0, 0); sample(5, false); sample(7, false);
    launch(1); sample(5, true); sample(7, true);
    step(6999); sample(5, true); sample(7, true);
    step(1); sample(5, false); sample(7, false);
    step(11000); irq(TX_DONE(0) | TX_DONE(1), 0); qtest_quit(s);

    setup(); tx_setup(0, 80, BIT(3) | BIT(5));
    memory(0, 0, symbol(7, true, 11, false)); memory(0, 1, 0);
    /* TX_LOOP_NUM[18:9], CNT_EN19, STOP_EN21 per pinned struct. */
    wr(TX_LIM(0), (3 << 9) | BIT(19) | BIT(21));
    launch(0); step(53999); g_assert_cmphex(rd(RAW) & TX_LOOP(0), ==, 0);
    step(1); g_assert_cmphex(rd(RAW) & TX_LOOP(0), ==, TX_LOOP(0));
    sample(5, true); /* explicit idle level, RMT-176 workaround */
    stop(0); wr(CLR, UINT32_MAX); step(100000); irq(0, 0);
    qtest_quit(s);
}

static void test_topology_no_hidden_loopback(void)
{
    setup(); graph(false);
    memory(0, 0, symbol(7, true, 11, false));
    memory(0, 1, symbol(5, true, 0, false));
    rx_setup(0, 80, 100, RX_OWNER); launch(0); step(23000);
    g_assert_cmphex(rd(RAW) & TX_DONE(0), ==, TX_DONE(0));
    g_assert_cmphex(rd(RX_STATUS(0)) & 1023, ==, 192);
    graph(true); wr(CLR, UINT32_MAX); tx_setup(0, 80, 0); launch(0);
    step(23000); rx_disable(0); rx_to_apb(0);
    g_assert_cmphex(captured(0, 0), ==, symbol(7, true, 11, false));
    qtest_quit(s);
}

static void test_matrix_inversion_input_enable(void)
{
    setup();
    /* Output inversion is physical; input inversion is after pad sampling. */
    qtest_writel(s, GPIO(GPIO_FUNC_OUT_SEL_CFG_OFFSET(4)), 81 | BIT(9));
    qtest_writel(s, GPIO(GPIO_FUNC_IN_SEL_CFG_OFFSET(81)), BIT(7) | BIT(6) | 5);
    memory(0, 0, symbol(7, true, 11, false));
    memory(0, 1, symbol(5, true, 0, false));
    rx_setup(0, 80, 100, RX_OWNER);
    launch(0); sample(5, false);
    step(7000); sample(5, true);
    step(11000); sample(5, false);
    step(5000); sample(5, true);
    rx_disable(0); rx_to_apb(0);
    g_assert_cmphex(captured(0, 0), ==, symbol(7, true, 11, false));
    qtest_quit(s);

    setup();
    qtest_writel(s, PAD(5), INPUT & ~R_IO_MUX_GPIOn_FUN_IE_MASK);
    memory(0, 0, symbol(7, true, 11, false)); memory(0, 1, 0);
    rx_setup(0, 80, 100, RX_OWNER); launch(0); step(18000);
    g_assert_cmphex(rd(RX_STATUS(0)) & 1023, ==, 192);
    rx_disable(0); wr(CLR, UINT32_MAX); step(200000); irq(0, 0);
    qtest_quit(s);
}

static void test_all_channel_masks(void)
{
    setup();
    for (unsigned ch = 0; ch < 4; ch++) {
        tx_setup(ch, 80, 0);
        memory(ch, 0, symbol(7, true, 11, false));
        memory(ch, 1, 0);
        launch(ch);
    }
    irq(0, 0); step(18000); irq(0xf, 0);
    for (unsigned ch = 0; ch < 4; ch++) {
        wr(ENA, TX_DONE(ch)); irq(0xf & ~((1U << ch) - 1), TX_DONE(ch));
        wr(CLR, RX_DONE(ch));
        irq(0xf & ~((1U << ch) - 1), TX_DONE(ch));
        wr(CLR, TX_DONE(ch)); irq(0xf & ~((1U << (ch + 1)) - 1), TX_DONE(ch));
    }
    qtest_quit(s);

    setup(); software_output();
    for (unsigned ch = 0; ch < 4; ch++) {
        qtest_writel(s, GPIO(GPIO_FUNC_IN_SEL_CFG_OFFSET(81 + ch)), BIT(7) | 5);
        rx_setup(ch, 80, 30, RX_OWNER);
    }
    software_drive(true); step(7000); software_drive(false);
    step(11000); software_drive(true); step(31000);
    irq(0xf << 16, 0);
    for (unsigned ch = 0; ch < 4; ch++) {
        wr(ENA, RX_DONE(ch));
        irq((0xf & ~((1U << ch) - 1)) << 16, RX_DONE(ch));
        wr(CLR, TX_DONE(ch));
        irq((0xf & ~((1U << ch) - 1)) << 16, RX_DONE(ch));
        wr(CLR, RX_DONE(ch));
        irq((0xf & ~((1U << (ch + 1)) - 1)) << 16, RX_DONE(ch));
        rx_to_apb(ch);
        g_assert_cmphex(captured(ch, 0), ==, symbol(7, true, 11, false));
    }
    qtest_quit(s);
}

static void test_rx_wrap(void)
{
    setup(); software_output(); wr(RX_LIM(0), 24);
    rx_setup(0, 80, 100, RX_OWNER | BIT(13));
    software_drive(true);
    for (unsigned i = 0; i < 64; i++) {
        unsigned high = i < 48 ? 5 : 9, low = i < 48 ? 7 : 11;
        step(high * 1000); software_drive(false);
        step(low * 1000); software_drive(true);
        if (i == 23 || i == 47) {
            g_assert_cmphex(rd(RAW) & RX_THR(0), ==, RX_THR(0));
            wr(CLR, RX_THR(0));
        }
    }
    g_assert_cmphex(rd(RAW) & (RX_ERR(0) | RX_DONE(0)), ==, 0);
    g_assert_cmphex(rd(RX_STATUS(0)) & (BIT(25) | BIT(26)), ==, 0);
    rx_disable(0); rx_to_apb(0);
    for (unsigned i = 0; i < 48; i++) {
        g_assert_cmphex(captured(0, i), ==,
                        i < 16 ? symbol(9, true, 11, false) :
                                 symbol(5, true, 7, false));
    }
    g_test_message("RX wrap: 64 exact symbols, 16 replaced, ns=%" PRId64, now);
    qtest_quit(s);
}

/* GDMA offsets/descriptor fields: pinned S3 gdma_reg.h, TRM ch3 and
 * the permanent esp32s3-gdma-test. No service/memory-to-memory shortcut. */
#define DMA_REG(out, reg) (0x6003f000ULL + ((out) ? 0x60 : 0) + (reg))
#define DMA_SRC 0x3fc90000U
#define DMA_TX_DESC 0x3fc91000U
#define DMA_DST 0x3fc92000U
#define DMA_RX_DESC 0x3fc93000U
static uint32_t dma_rd(bool out, unsigned reg)
{
    return qtest_readl(s, DMA_REG(out, reg));
}
static void dma_wr(bool out, unsigned reg, uint32_t value)
{
    qtest_writel(s, DMA_REG(out, reg), value);
}
static void descriptor(uint32_t addr, unsigned size, unsigned length,
                       bool eof, bool owner, uint32_t buffer, uint32_t next)
{
    qtest_writel(s, addr, size | (length << 12) |
                 (eof ? BIT(30) : 0) | (owner ? BIT(31) : 0));
    qtest_writel(s, addr + 4, buffer);
    qtest_writel(s, addr + 8, next);
}
static void dma_setup(void)
{
    setup();
    qtest_writel(s, SYS(0x1c), qtest_readl(s, SYS(0x1c)) | BIT(6));
    qtest_writel(s, SYS(0x24), qtest_readl(s, SYS(0x24)) & ~BIT(6));
    qtest_writel(s, GPIO(GPIO_FUNC_OUT_SEL_CFG_OFFSET(4)), 84);
    qtest_writel(s, GPIO(GPIO_FUNC_IN_SEL_CFG_OFFSET(84)), BIT(7) | 5);
    tx_setup(3, 80, BIT(25) | BIT(4));
    dma_wr(true, 0, BIT(2)); /* AUTO_WRBACK */
    dma_wr(true, 4, BIT(12)); dma_wr(false, 4, BIT(12));
    dma_wr(true, 0x48, 9); dma_wr(false, 0x48, 9); /* RMT trigger */
    dma_wr(true, 0x14, UINT32_MAX); dma_wr(false, 0x14, UINT32_MAX);
}
static void dma_chain(unsigned rx_last_size)
{
    for (unsigned i = 0; i < 128; i++) {
        qtest_writel(s, DMA_SRC + 4 * i, symbol(5, true, 7, false));
    }
    /* Close the 128th low half with a final high pulse. */
    qtest_writel(s, DMA_SRC + 512, symbol(5, true, 0, false));
    qtest_memset(s, DMA_DST, 0xee, 640);
    for (unsigned i = 0; i < 3; i++) {
        unsigned offset = i * 192, tx_size = i == 2 ? 132 : 192;
        descriptor(DMA_TX_DESC + 12 * i, tx_size, tx_size, i == 2, true,
                   DMA_SRC + offset, i == 2 ? 0 : DMA_TX_DESC + 12 * (i + 1));
        descriptor(DMA_RX_DESC + 12 * i, i == 2 ? rx_last_size : 192, 0, false, true,
                   DMA_DST + offset, i == 2 ? 0 : DMA_RX_DESC + 12 * (i + 1));
    }
    dma_wr(false, 0x20, (DMA_RX_DESC & 0xfffff) | BIT(22));
    dma_wr(true, 0x20, (DMA_TX_DESC & 0xfffff) | BIT(21));
    wr(RX_CONF0(3), 80 | (50 << 8) | BIT(23) | BIT(24));
    wr(RX_CONF1(3), RX_OWNER | BIT(13) | BIT(1) | BIT(2) | RX_UPDATE);
    wr(RX_CONF1(3), RX_OWNER | BIT(13) | BIT(0) | RX_UPDATE);
    launch(3);
}
static void test_dma_long_chain(const void *data)
{
    dma_setup(); dma_chain(GPOINTER_TO_UINT(data));
    g_assert_cmphex(rd(RAW), ==, 0);
    g_assert_cmphex(dma_rd(true, 8) & (BIT(1) | BIT(3)), ==, 0);
    step(1540999);
    g_assert_cmphex(rd(RAW) & (TX_DONE(3) | RX_DONE(3)), ==, 0);
    step(1);
    g_assert_cmphex(rd(RAW) & (TX_DONE(3) | TX_ERR(3) | BIT(28)), ==, TX_DONE(3));
    g_assert_cmphex(dma_rd(true, 8) & (BIT(0) | BIT(1) | BIT(2) | BIT(3)), ==,
                    BIT(0) | BIT(1) | BIT(3));
    step(50000); g_assert_cmphex(rd(RAW) & RX_DONE(3), ==, 0);
    step(1000);
    g_assert_cmphex(rd(RAW) & (RX_DONE(3) | RX_ERR(3) | BIT(29)), ==, RX_DONE(3));
    g_assert_cmphex(dma_rd(false, 8) & 0x1f, ==, BIT(0) | BIT(1));
    g_assert_cmphex(dma_rd(false, 0x28), ==, DMA_RX_DESC + 24);
    for (unsigned i = 0; i < 128; i++) {
        g_assert_cmphex(qtest_readl(s, DMA_DST + 4 * i), ==, symbol(5, true, 7, false));
    }
    g_assert_cmphex(qtest_readl(s, DMA_DST + 512) & 0xffff, ==, 0x8005);
    for (unsigned i = 0; i < 3; i++) {
        uint32_t tx = qtest_readl(s, DMA_TX_DESC + 12 * i);
        uint32_t rx = qtest_readl(s, DMA_RX_DESC + 12 * i);
        g_assert_cmphex(tx & BIT(31), ==, 0);
        g_assert_cmphex(rx & (BIT(31) | BIT(30)), ==, i == 2 ? BIT(30) : 0);
        g_assert_cmpuint((rx >> 12) & 4095, ==, i == 2 ? 132 : 192);
    }
    wr(ENA, TX_DONE(3) | RX_DONE(3)); g_assert_true(qtest_get_irq(s, 0));
    wr(CLR, TX_DONE(3)); g_assert_true(qtest_get_irq(s, 0));
    wr(CLR, RX_DONE(3)); g_assert_false(qtest_get_irq(s, 0));
    g_test_message("DMA: 128 exact closed symbols, 3 descriptors, TX ns=1541000 RX ns=%" PRId64
                   " terminal_len=132 terminal_desc=0x%08x", now, DMA_RX_DESC + 24);
    qtest_quit(s);
}
static void test_dma_cancel_faults(void)
{
    for (unsigned reset = 0; reset < 2; reset++) {
        uint8_t before[640], after[640];
        dma_setup(); dma_chain(256); step(60000);
        qtest_memread(s, DMA_DST, before, sizeof(before));
        if (reset) {
            uint32_t saved = qtest_readl(s, SYS(0x20));
            qtest_writel(s, SYS(0x20), saved | BIT(9));
            qtest_writel(s, SYS(0x20), saved & ~BIT(9));
        } else {
            rx_disable(3); stop(3);
        }
        wr(CLR, UINT32_MAX);
        dma_wr(true, 0x14, UINT32_MAX); dma_wr(false, 0x14, UINT32_MAX);
        uint32_t tx_link = dma_rd(true, 0x20), rx_link = dma_rd(false, 0x20);
        step(2000000);
        qtest_memread(s, DMA_DST, after, sizeof(after));
        g_assert_cmpmem(before, sizeof(before), after, sizeof(after));
        g_assert_cmphex(dma_rd(true, 0x20), ==, tx_link);
        g_assert_cmphex(dma_rd(false, 0x20), ==, rx_link);
        g_assert_cmphex(dma_rd(true, 8), ==, 0);
        g_assert_cmphex(dma_rd(false, 8), ==, 0);
        irq(0, 0); qtest_quit(s);
    }
    for (unsigned wrong_periph = 0; wrong_periph < 2; wrong_periph++) {
        dma_setup();
        qtest_writel(s, DMA_SRC, symbol(5, true, 7, false));
        descriptor(DMA_TX_DESC, 4, 4, true, wrong_periph,
                   DMA_SRC, 0);
        if (wrong_periph) { dma_wr(true, 0x48, 0); }
        dma_wr(true, 0x20, (DMA_TX_DESC & 0xfffff) | BIT(21));
        launch(3);
        g_assert_cmphex(rd(RAW), ==, TX_ERR(3) | BIT(28));
        if (!wrong_periph) { g_assert_cmphex(dma_rd(true, 8) & BIT(2), ==, BIT(2)); }
        wr(CLR, UINT32_MAX); step(2000000); irq(0, 0);
        qtest_quit(s);
    }
    for (unsigned wrong_periph = 0; wrong_periph < 2; wrong_periph++) {
        dma_setup(); software_output();
        descriptor(DMA_RX_DESC, 192, 0, false, wrong_periph, DMA_DST, 0);
        if (wrong_periph) { dma_wr(false, 0x48, 0); }
        dma_wr(false, 0x20, (DMA_RX_DESC & 0xfffff) | BIT(22));
        wr(RX_CONF0(3), 80 | (50 << 8) | BIT(23) | BIT(24));
        wr(RX_CONF1(3), RX_OWNER | BIT(13) | BIT(0) | RX_UPDATE);
        software_drive(true); step(5000); software_drive(false);
        step(7000); software_drive(true);
        g_assert_cmphex(rd(RAW), ==, RX_ERR(3) | BIT(29));
        if (!wrong_periph) { g_assert_cmphex(dma_rd(false, 8) & BIT(3), ==, BIT(3)); }
        wr(CLR, UINT32_MAX); step(2000000); irq(0, 0);
        qtest_quit(s);
    }
}

static void test_dma_reassignment(void)
{
    for (unsigned out = 0; out < 2; out++) {
        dma_setup(); dma_chain(256); step(60000);
        uint32_t desc = (out ? DMA_TX_DESC : DMA_RX_DESC) + 0x100;
        uint32_t buf = (out ? DMA_SRC : DMA_DST) + 0x1000;
        qtest_memset(s, buf, 0xee, 64);
        if (out) {
            for (unsigned i = 0; i < 16; i++) {
                qtest_writel(s, buf + 4 * i, symbol(9, true, 11, false));
            }
        }
        descriptor(desc, 64, out ? 64 : 0, out, true, buf, 0);
        qtest_writel(s, DMA_REG(out, 4) + 0xc0, BIT(12));
        qtest_writel(s, DMA_REG(out, 0x48) + 0xc0, 9);
        qtest_writel(s, DMA_REG(out, 0x20) + 0xc0,
                     (desc & 0xfffff) | (out ? BIT(21) : BIT(22)));
        dma_wr(out, 0x48, 0); /* old binding now belongs to SPI2 */
        step(12000);
        uint32_t error = out ? TX_ERR(3) | BIT(28) : RX_ERR(3) | BIT(29);
        g_assert_cmphex(rd(RAW) & error, ==, error);
        /* A second started RMT channel must not steal the existing binding. */
        g_assert_cmphex(qtest_readl(s, desc) & BIT(31), ==, BIT(31));
        g_assert_cmphex(qtest_readl(s, DMA_REG(out, 8) + 0xc0), ==, 0);
        if (!out) {
            for (unsigned i = 0; i < 16; i++) {
                g_assert_cmphex(qtest_readl(s, buf + 4 * i), ==, 0xeeeeeeee);
            }
        }
        rx_disable(3); stop(3); wr(CLR, UINT32_MAX);
        step(2000000); irq(0, 0);
        qtest_quit(s);
    }
}

/* Named peer tests reuse the same actual ProjectDocument, extending terminals
 * and rails. Inspection is the public native QOM capture, never engine hooks. */
static QDict *named_entry(QList *list, const char *key, const char *id)
{
    const QListEntry *e;
    QLIST_FOREACH_ENTRY(list, e) {
        QDict *d = qobject_to(QDict, qlist_entry_obj(e));
        if (!strcmp(qdict_get_str(d, key), id)) { return d; }
    }
    g_assert_not_reached();
}
static QDict *graph_component(QDict *p, const char *id, const char *kind,
                              const char *type)
{
    QDict *c = qdict_new();
    qdict_put_str(c, "id", id); qdict_put_str(c, "name", id);
    qdict_put_str(c, "kind", kind); qdict_put_str(c, "type", type);
    qdict_put(c, "parameters", qdict_new()); qdict_put(c, "terminals", qlist_new());
    qlist_append(qdict_get_qlist(p, "components"), c);
    return c;
}
static void graph_terminal(QDict *c, const char *role, const char *domain,
                           const char *direction, int gpio)
{
    QDict *t = qdict_new();
    g_autofree char *id = g_strdup_printf("%s.%s", qdict_get_str(c, "id"), role);
    qdict_put_str(t, "id", id); qdict_put_str(t, "name", role);
    qdict_put_str(t, "role", role); qdict_put_str(t, "domain", domain);
    qdict_put_str(t, "direction", direction);
    if (gpio >= 0) { qdict_put_int(t, "gpio", gpio); }
    qlist_append(qdict_get_qlist(c, "terminals"), t);
}
static void graph_quantity(QDict *c, const char *name, double value,
                           const char *unit)
{
    QDict *q = qdict_new();
    if (!strcmp(unit, "count")) { qdict_put_int(q, "value", value); }
    else { qdict_put(q, "value", qnum_from_double(value)); }
    qdict_put_str(q, "unit", unit);
    qdict_put(qdict_get_qdict(c, "parameters"), name, q);
}
static void graph_endpoint(QDict *p, const char *net_id, const char *endpoint)
{
    QDict *n = named_entry(qdict_get_qlist(p, "nets"), "id", net_id);
    qlist_append_str(qdict_get_qlist(n, "endpoints"), endpoint);
}
static void graph_net(QDict *p, const char *id, const char *endpoint)
{
    QDict *n = qdict_new();
    qdict_put_str(n, "id", id); qdict_put_str(n, "name", id);
    qdict_put(n, "endpoints", qlist_new());
    qlist_append(qdict_get_qlist(p, "nets"), n);
    graph_endpoint(p, id, endpoint);
}
static void graph_resistor(QDict *p, const char *id, unsigned ohm)
{
    QDict *c = graph_component(p, id, "resistor", "resistor");
    graph_terminal(c, "a", "passive", "passive", -1);
    graph_terminal(c, "b", "passive", "passive", -1);
    graph_quantity(c, "resistance", ohm, "ohm");
}
static QDict *peer_project(bool ws, bool connected, bool pull)
{
    QDict *p = graph_document(true);
    QDict *c = graph_component(p, "VP", "voltage-source", "voltage-source");
    graph_terminal(c, "p", "power", "output", -1);
    graph_terminal(c, "n", "ground", "input", -1);
    graph_quantity(c, "voltage", 3.3, "V");
    graph_endpoint(p, "gnd", "VP.n");
    graph_net(p, "peer-vdd", "VP.p");
    c = graph_component(p, ws ? "WS" : "IR", "device",
                        ws ? "ws2812-functional-3v3" : "nec-envelope-source");
    graph_terminal(c, "vdd", "power", "input", -1);
    graph_terminal(c, "gnd", "ground", "input", -1);
    graph_terminal(c, ws ? "din" : "out", "digital", ws ? "input" : "output", -1);
    graph_endpoint(p, "gnd", ws ? "WS.gnd" : "IR.gnd");
    graph_endpoint(p, "peer-vdd", ws ? "WS.vdd" : "IR.vdd");
    if (ws) {
        graph_quantity(c, "led_count", 1, "count");
        if (connected) {
            graph_endpoint(p, "pulse0", "WS.din");
        } else {
            graph_net(p, "ws-isolated", "WS.din");
            graph_resistor(p, "PullWS", 10000);
            graph_endpoint(p, "ws-isolated", "PullWS.a");
            graph_endpoint(p, "gnd", "PullWS.b");
        }
    } else {
        graph_quantity(c, "address", 0x34, "count");
        graph_quantity(c, "command", 0xa7, "count");
        graph_quantity(c, "period_ms", 1000, "count");
        c = named_entry(qdict_get_qlist(p, "components"), "id", "U");
        graph_terminal(c, "io8", "digital", "inout", 8);
        graph_net(p, "ir-rx", "U.io8");
        if (connected) {
            graph_endpoint(p, "ir-rx", "IR.out");
        } else {
            graph_net(p, "ir-source", "IR.out");
        }
        if (pull) {
            graph_resistor(p, "PullIR", 45000);
            graph_endpoint(p, "ir-rx", "PullIR.a");
            graph_endpoint(p, "vdd", "PullIR.b");
            if (!connected) {
                graph_resistor(p, "PullSource", 45000);
                graph_endpoint(p, "ir-source", "PullSource.a");
                graph_endpoint(p, "vdd", "PullSource.b");
            }
        }
        qtest_writel(s, PAD(8), INPUT);
        qtest_writel(s, GPIO(GPIO_FUNC_IN_SEL_CFG_OFFSET(81)), BIT(7) | 8);
    }
    apply_graph(p);
    return p;
}
static void peer_voltage(QDict *p, double volts)
{
    QDict *c = named_entry(qdict_get_qlist(p, "components"), "id", "VP");
    graph_quantity(c, "voltage", volts, "V");
    apply_graph(p);
}
static uint64_t decimal(QDict *d, const char *key)
{
    const char *text = qdict_get_str(d, key);
    char *end;
    g_assert_true(g_ascii_isdigit(*text));
    errno = 0;
    uint64_t value = g_ascii_strtoull(text, &end, 10);
    g_assert_cmpint(errno, ==, 0); g_assert_cmpstr(end, ==, "");
    return value;
}
static QDict *peer_capture(const char *id)
{
    QDict *reply = qtest_qmp(s, "{'execute':'qom-get','arguments':{'path':'/machine/soc/rmt-peers','property':'capture-json'}}");
    g_assert_false(qdict_haskey(reply, "error"));
    QObject *o = qobject_from_json(qdict_get_str(reply, "return"), &error_abort);
    QDict *root = qobject_to(QDict, o);
    g_assert_nonnull(root); g_assert_cmpint(qdict_get_int(root, "abi"), ==, 1);
    g_assert_true(qdict_get_bool(root, "healthy"));
    g_assert_cmpuint(decimal(root, "sample_ns"), <=, now);
    QDict *peer = named_entry(qdict_get_qlist(root, "peers"), "component_id", id);
    qobject_ref(peer);
    qobject_unref(o); qobject_unref(reply);
    return peer;
}
static void ws_capture(const char *grb, uint64_t frames, uint64_t epoch)
{
    QDict *p = peer_capture("WS");
    g_assert_true(qdict_get_bool(p, "power_known"));
    g_assert_true(qdict_get_bool(p, "powered"));
    g_assert_cmpuint(decimal(p, "power_epoch"), ==, epoch);
    g_assert_cmpuint(decimal(p, "frame_count"), ==, frames);
    g_assert_cmpint(qdict_get_bool(p, "latch_valid"), ==, grb != NULL);
    if (grb) { g_assert_cmpstr(qdict_get_str(p, "grb"), ==, grb); }
    else { g_assert_null(qdict_get_try_str(p, "grb")); }
    qobject_unref(p);
}
static int64_t ws_send(uint32_t grb, unsigned bits, bool malformed)
{
    tx_setup(0, 4, 0); /* APB80MHz /4 =50ns */
    for (unsigned i = 0; i < bits; i++) {
        bool one = (grb >> (23 - i)) & 1;
        unsigned high = malformed && i == 0 ? 4 : one ? 16 : 8;
        memory(0, i, symbol(high, true, 25 - high, false));
    }
    memory(0, bits, 0); wr(CLR, UINT32_MAX);
    launch(0); irq(0, 0);
    int64_t last_fall = now;
    for (unsigned i = 0; i < bits; i++) {
        bool one = (grb >> (23 - i)) & 1;
        unsigned high = malformed && i == 0 ? 4 : one ? 16 : 8;
        step(high * 50); last_fall = now; sample(5, false);
        step((25 - high) * 50);
    }
    irq(TX_DONE(0), 0);
    return last_fall;
}
static void mcu_reset(void)
{
    QDict *r = qtest_qmp(s, "{'execute':'system_reset'}");
    g_assert_false(qdict_haskey(r, "error")); qobject_unref(r);
    step(0);
}
static void test_peer_ws_latch_hold_power_reset(void)
{
    setup(); QDict *project = peer_project(true, true, true);
    QDict *initial = peer_capture("WS");
    uint64_t epoch = decimal(initial, "power_epoch");
    qobject_unref(initial);
    step(50000); ws_capture(NULL, 0, epoch);
    int64_t fall = ws_send(0x123456, 24, false);
    step(fall + 50000 - now - 1); ws_capture(NULL, 0, epoch);
    step(1); ws_capture("123456", 1, epoch);
    QDict *p = peer_capture("WS");
    g_assert_cmpuint(decimal(p, "last_latch_ns"), ==, fall + 50000);
    int64_t first_latch = decimal(p, "last_latch_ns");
    uint64_t errors = decimal(p, "errors");
    qobject_unref(p);
    fall = ws_send(0xabcdef, 7, false);
    step(fall + 50000 - now); ws_capture("123456", 1, epoch);
    p = peer_capture("WS"); g_assert_cmpuint(decimal(p, "errors"), >, errors);
    errors = decimal(p, "errors"); qobject_unref(p);
    fall = ws_send(0xabcdef, 24, true);
    step(fall + 50000 - now); ws_capture("123456", 1, epoch);
    p = peer_capture("WS"); g_assert_cmpuint(decimal(p, "errors"), >, errors);
    uint64_t power_on = decimal(p, "power_on_ns"); qobject_unref(p);
    peer_voltage(project, 2.7); ws_capture("123456", 1, epoch);
    peer_voltage(project, 3.6); ws_capture("123456", 1, epoch);
    mcu_reset(); ws_capture("123456", 1, epoch);
    p = peer_capture("WS"); g_assert_cmpuint(decimal(p, "power_on_ns"), ==, power_on);
    qobject_unref(p);
    peer_voltage(project, 3.61);
    p = peer_capture("WS");
    g_assert_true(qdict_get_bool(p, "power_known"));
    g_assert_false(qdict_get_bool(p, "powered")); qobject_unref(p);
    peer_voltage(project, 2.69);
    p = peer_capture("WS"); g_assert_false(qdict_get_bool(p, "powered")); qobject_unref(p);
    peer_voltage(project, 3.3);
    p = peer_capture("WS"); g_assert_cmpuint(decimal(p, "power_epoch"), >, epoch);
    epoch = decimal(p, "power_epoch"); qobject_unref(p);
    ws_capture(NULL, 0, epoch); /* Only a real power cycle clears held pixels. */
    qtest_writel(s, SYS(0x60), 0x00028401);
    qtest_writel(s, SYS(0x18), qtest_readl(s, SYS(0x18)) | BIT(9));
    source(1, 1, 0, 0);
    qtest_writel(s, PAD(4), INPUT); qtest_writel(s, PAD(5), INPUT);
    qtest_writel(s, GPIO(GPIO_FUNC_OUT_SEL_CFG_OFFSET(4)), 81);
    tx_setup(0, 4, 0); step(50000);
    fall = ws_send(0xabcdef, 24, false);
    step(fall + 50000 - now); ws_capture("abcdef", 1, epoch);
    g_test_message("WS actual GRB=123456 first latch_ns=%" PRId64 " MCU reset retained external power epoch", first_latch);
    qobject_unref(project); qtest_quit(s);
}
static void test_peer_ws_disconnected(void)
{
    setup(); QDict *project = peer_project(true, false, true);
    step(50000); int64_t fall = ws_send(0xff007f, 24, false);
    step(fall + 50000 - now);
    QDict *p = peer_capture("WS");
    g_assert_false(qdict_get_bool(p, "latch_valid"));
    g_assert_cmpuint(decimal(p, "frame_count"), ==, 0);
    g_assert_null(qdict_get_try_str(p, "grb"));
    qobject_unref(p); qobject_unref(project); qtest_quit(s);
}
static void nec_physical_voltage(QDict *p, bool valid, double expected)
{
    g_assert_cmpint(qdict_get_bool(p, "out_voltage_valid"), ==, valid);
    QObject *o = qdict_get(p, "out_voltage_v");
    if (valid) {
        QNum *voltage = qobject_to(QNum, o);
        g_assert_nonnull(voltage);
        g_assert_cmpfloat_with_epsilon(qnum_get_double(voltage), expected, 1e-7);
    } else {
        g_assert_cmpint(qobject_type(o), ==, QTYPE_QNULL);
    }
}
static void nec_off_with_pull(QDict *p)
{
    g_assert_true(qdict_get_bool(p, "power_known"));
    g_assert_false(qdict_get_bool(p, "powered"));
    g_assert_false(qdict_get_bool(p, "drive_oe"));
    g_assert_false(qdict_get_bool(p, "out_valid"));
    g_assert_cmpint(qobject_type(qdict_get(p, "out_level")), ==, QTYPE_QNULL);
    /* The independent MCU-rail pull remains physical 3.3V. An off NEC
     * device has no operational logic sample or invented 0V-rail threshold. */
    nec_physical_voltage(p, true, 3.3);
}

static void nec_observe(bool low)
{
    QDict *p = peer_capture("IR");
    g_assert_true(qdict_get_bool(p, "power_known"));
    g_assert_true(qdict_get_bool(p, "powered"));
    g_assert_cmpint(qdict_get_bool(p, "drive_oe"), ==, low);
    g_assert_false(qdict_get_bool(p, "drive_level"));
    g_assert_true(qdict_get_bool(p, "out_valid"));
    g_assert_cmpint(qdict_get_bool(p, "out_level"), ==, !low);
    nec_physical_voltage(p, true, low ? 3.3 * 40 / 45040 : 3.3);
    qobject_unref(p);
    double voltage = sample(8, !low);
    /* Actual 45kohm rail pull and 40ohm OD sink, not a rail/bit shortcut. */
    g_assert_cmpfloat_with_epsilon(voltage, low ? 3.3 * 40 / 45040 : 3.3, 1e-7);
}
static void nec_frame(void)
{
    /* Called at the physical falling leader edge, not a generated RX vector. */
    int64_t start = now;
    nec_observe(true); step(9000000); nec_observe(false);
    step(4500000); nec_observe(true);
    const uint8_t bytes[] = {0x34, 0xcb, 0xa7, 0x58};
    for (unsigned i = 0; i < 32; i++) {
        step(560000); nec_observe(false);
        step((bytes[i / 8] & BIT(i % 8)) ? 1690000 : 560000);
        nec_observe(true);
    }
    step(560000); nec_observe(false);
    g_assert_cmpint(now - start, ==, 67980000);
}
static void nec_capture_words(void)
{
    g_assert_cmphex(rd(RAW) & (RX_DONE(0) | RX_ERR(0)), ==, RX_DONE(0));
    rx_to_apb(0);
    g_assert_cmphex(captured(0, 0), ==, symbol(9000, false, 4500, true));
    const uint8_t bytes[] = {0x34, 0xcb, 0xa7, 0x58};
    for (unsigned i = 0; i < 32; i++) {
        unsigned space = (bytes[i / 8] & BIT(i % 8)) ? 1690 : 560;
        g_assert_cmphex(captured(0, i + 1), ==, symbol(560, false, space, true));
    }
    g_assert_cmphex(captured(0, 33) & 0xffff, ==, 560);
    g_test_message("NEC actual source bytes=34cba758 closed RMT words=33 final mark=560us done_ns=%" PRId64, now);
}
static void test_peer_nec_exact_reset(void)
{
    setup(); QDict *project = peer_project(false, true, true);
    QDict *p = peer_capture("IR");
    uint64_t epoch = decimal(p, "power_epoch"), power_on = decimal(p, "power_on_ns");
    qobject_unref(p);
    rx_setup(0, 80, 10000, RX_OWNER);
    step(power_on + 100000000 - now - 1); nec_observe(false);
    g_assert_cmphex(rd(RAW), ==, 0);
    g_assert_cmphex(rd(RX_STATUS(0)) & 1023, ==, 192);
    step(1); nec_frame();
    p = peer_capture("IR");
    g_assert_cmpuint(decimal(p, "frame_count"), ==, 1);
    g_assert_cmpuint(decimal(p, "frame_abort_count"), ==, 0); qobject_unref(p);
    step(10000000); g_assert_cmphex(rd(RAW) & RX_DONE(0), ==, 0);
    step(1000); nec_capture_words();
    mcu_reset();
    p = peer_capture("IR");
    g_assert_cmpuint(decimal(p, "power_epoch"), ==, epoch);
    g_assert_cmpuint(decimal(p, "power_on_ns"), ==, power_on);
    g_assert_cmpuint(decimal(p, "frame_count"), ==, 1); qobject_unref(p);
    qtest_writel(s, SYS(0x60), 0x00028401);
    qtest_writel(s, SYS(0x18), qtest_readl(s, SYS(0x18)) | BIT(9));
    source(1, 1, 0, 0); qtest_writel(s, PAD(8), INPUT);
    qtest_writel(s, GPIO(GPIO_FUNC_IN_SEL_CFG_OFFSET(81)), BIT(7) | 8);
    rx_setup(0, 80, 10000, RX_OWNER);
    step(100000000); nec_observe(false); /* reset must not reanchor first frame */
    step(power_on + 1100000000 - now - 1); nec_observe(false);
    step(1); nec_frame(); step(10001000); nec_capture_words();
    p = peer_capture("IR");
    g_assert_cmpuint(decimal(p, "frame_count"), ==, 2);
    g_assert_cmpuint(decimal(p, "power_epoch"), ==, epoch);
    g_assert_cmpuint(decimal(p, "frame_abort_count"), ==, 0);
    qobject_unref(p); qobject_unref(project); qtest_quit(s);
}
static void test_peer_nec_power_cancel(void)
{
    setup(); QDict *project = peer_project(false, true, true);
    QDict *p = peer_capture("IR");
    uint64_t epoch = decimal(p, "power_epoch"), power_on = decimal(p, "power_on_ns");
    qobject_unref(p);
    step(power_on + 100000000 - now); nec_observe(true);
    step(2000000); peer_voltage(project, 0);
    p = peer_capture("IR");
    nec_off_with_pull(p); qobject_unref(p);
    sample(8, true); wr(CLR, UINT32_MAX);
    step(7000000); sample(8, true); irq(0, 0);
    step(100000000);
    p = peer_capture("IR");
    g_assert_cmpuint(decimal(p, "frame_count"), ==, 0);
    nec_off_with_pull(p); qobject_unref(p);
    peer_voltage(project, 3.3);
    p = peer_capture("IR");
    g_assert_cmpuint(decimal(p, "power_epoch"), >, epoch);
    power_on = decimal(p, "power_on_ns"); qobject_unref(p);
    step(power_on + 100000000 - now - 1); nec_observe(false);
    step(1); nec_frame();
    p = peer_capture("IR"); g_assert_cmpuint(decimal(p, "frame_count"), ==, 1);
    qobject_unref(p); qobject_unref(project); qtest_quit(s);
}
static void test_peer_nec_missing_pull(void)
{
    setup(); QDict *project = peer_project(false, true, false);
    QDict *p = peer_capture("IR");
    uint64_t power_on = decimal(p, "power_on_ns");
    g_assert_false(qdict_get_bool(p, "drive_oe"));
    g_assert_false(qdict_get_bool(p, "out_valid"));
    g_assert_cmpint(qobject_type(qdict_get(p, "out_level")), ==, QTYPE_QNULL);
    nec_physical_voltage(p, false, 0); qobject_unref(p);
    step(power_on + 100000000 - now);
    p = peer_capture("IR");
    g_assert_true(qdict_get_bool(p, "drive_oe"));
    g_assert_true(qdict_get_bool(p, "out_valid"));
    g_assert_false(qdict_get_bool(p, "out_level"));
    nec_physical_voltage(p, true, 0); qobject_unref(p);
    sample(8, false);
    step(9000000);
    p = peer_capture("IR");
    g_assert_false(qdict_get_bool(p, "drive_oe"));
    g_assert_false(qdict_get_bool(p, "out_valid"));
    g_assert_cmpint(qobject_type(qdict_get(p, "out_level")), ==, QTYPE_QNULL);
    nec_physical_voltage(p, false, 0); qobject_unref(p);
    peer_voltage(project, 0); step(100000000);
    p = peer_capture("IR");
    g_assert_true(qdict_get_bool(p, "power_known"));
    g_assert_false(qdict_get_bool(p, "powered"));
    g_assert_false(qdict_get_bool(p, "drive_oe"));
    g_assert_false(qdict_get_bool(p, "out_valid"));
    g_assert_cmpint(qobject_type(qdict_get(p, "out_level")), ==, QTYPE_QNULL);
    nec_physical_voltage(p, false, 0);
    g_assert_cmpuint(decimal(p, "frame_count"), ==, 0);
    qobject_unref(p); qobject_unref(project); qtest_quit(s);
}
static void test_peer_nec_disconnected(void)
{
    setup(); QDict *project = peer_project(false, false, true);
    QDict *p = peer_capture("IR");
    uint64_t power_on = decimal(p, "power_on_ns"); qobject_unref(p);
    rx_setup(0, 80, 10000, RX_OWNER);
    step(power_on + 100000000 - now);
    /* The isolated source still works, but its edge cannot reach GPIO8. */
    p = peer_capture("IR");
    g_assert_true(qdict_get_bool(p, "drive_oe"));
    g_assert_true(qdict_get_bool(p, "out_valid"));
    g_assert_false(qdict_get_bool(p, "out_level")); qobject_unref(p);
    sample(8, true); step(9000000); sample(8, true);
    peer_voltage(project, 0); step(100000000);
    g_assert_cmphex(rd(RX_STATUS(0)) & 1023, ==, 192);
    irq(0, 0); qobject_unref(project); qtest_quit(s);
}

static void test_rx_live_clock_ref_reset(void)
{
    setup(); software_output(); rx_setup(0, 80, 1000, RX_OWNER);
    step(1000); software_drive(true);
    step(1000000);
    /* Preserve 1000 elapsed ticks. Rescaling a synthetic past edge by200
     * would place it before ns0 and overflow an unsigned idle deadline. */
    source(1, 200, 0, 0); wr(REF_RST, BIT(4));
    step(199999); irq(0, 0);
    step(1); irq(RX_DONE(0), 0);
    g_assert_cmphex(rd(RX_STATUS(0)) & FSM_MASK, ==, 0);
    g_test_message("RX live groupdivider200 REF_RST: elapsed1000 ticks retained, idle_done_ns=%" PRId64, now);
    qtest_quit(s);

    setup(); software_output(); rx_setup(0, 80, 1000, RX_OWNER);
    step(1000); software_drive(true); step(7000);
    source(3, 1, 0, 0);
    wr(RX_CONF0(0), 200 | (1000 << 8) | BIT(24));
    wr(RX_CONF1(0), RX_OWNER | BIT(0) | RX_UPDATE);
    wr(REF_RST, BIT(4)); /* Divider phase reset must not erase captured ticks. */
    step(10000); software_drive(false); /* 7 old +2 new ticks =9 */
    step(5000); software_drive(true);
    rx_disable(0); rx_to_apb(0);
    g_assert_cmphex(captured(0, 0), ==, symbol(9, true, 1, false));
    irq(0, 0);
    g_test_message("RX APB->XTAL divider80->200 REF_RST: exact closed word=0x%08x ns=%" PRId64,
                   captured(0, 0), now);
    qtest_quit(s);
}

static void test_rx_pending_filter_clock(void)
{
    const uint32_t filter = RX_OWNER | BIT(4) | (160 << 5);
    setup(); software_output(); rx_setup(0, 80, 4, filter);
    step(1000); software_drive(true); step(1000);
    source(3, 1, 0, 0);
    wr(RX_CONF0(0), 40 | (4 << 8) | BIT(24));
    wr(RX_CONF1(0), filter | BIT(0) | RX_UPDATE);
    wr(REF_RST, BIT(4));
    /* Pending filter retains80 oldgroup ticks; remaining80 at40MHz need
     * 2us. Qualified edge=4us; RXtick1us then idle>4 completes at9us. */
    step(1999); irq(0, 0); step(1); irq(0, 0);
    step(4999); irq(0, 0); step(1); irq(RX_DONE(0), 0);
    g_test_message("RX pendingfilter qualified_edge_ns=4000 idle_done_ns=%" PRId64, now);
    qtest_quit(s);

    setup(); software_output(); rx_setup(0, 80, 100, filter);
    step(1000); software_drive(true); step(1000);
    source(3, 1, 0, 0);
    wr(RX_CONF0(0), 40 | (100 << 8) | BIT(24));
    wr(RX_CONF1(0), filter | BIT(0) | RX_UPDATE);
    wr(REF_RST, BIT(4));
    step(10000); software_drive(false); /* raw falling edge=12us */
    step(6000); software_drive(true);   /* raw rising edge=18us */
    step(4000); /* accepted low16us/high22us, not backdated raw input */
    rx_disable(0); rx_to_apb(0);
    g_assert_cmphex(captured(0, 0), ==, symbol(12, true, 6, false));
    irq(0, 0);
    g_test_message("RX mixedfilter qualified edges4/16/22us exact word=0x%08x", captured(0, 0));
    qtest_quit(s);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/esp32s3/rmt/symbols-irq-w1c", test_symbols_irq);
    qtest_add_func("/esp32s3/rmt/dividers-sources", test_dividers_sources);
    qtest_add_func("/esp32s3/rmt/rx-first-edge-idle-filter", test_rx_first_edge_idle_filter);
    qtest_add_func("/esp32s3/rmt/rx-owner-overflow-threshold", test_rx_owner_overflow_threshold);
    qtest_add_func("/esp32s3/rmt/wrap-refill-underflow", test_wrap_refill_underflow);
    qtest_add_func("/esp32s3/rmt/stop-reset-gate", test_stop_reset_gate);
    qtest_add_func("/esp32s3/rmt/carrier-sync-loop", test_carrier_sync_loop);
    qtest_add_func("/esp32s3/rmt/topology-no-hidden-loopback", test_topology_no_hidden_loopback);
    qtest_add_func("/esp32s3/rmt/all-channel-masks", test_all_channel_masks);
    qtest_add_func("/esp32s3/rmt/rx-wrap", test_rx_wrap);
    qtest_add_func("/esp32s3/rmt/matrix-inversion-input-enable", test_matrix_inversion_input_enable);
    qtest_add_data_func("/esp32s3/rmt/dma-long-chain", GUINT_TO_POINTER(256), test_dma_long_chain);
    qtest_add_data_func("/esp32s3/rmt/dma-exact-capacity-eof", GUINT_TO_POINTER(132), test_dma_long_chain);
    qtest_add_func("/esp32s3/rmt/dma-cancel-faults", test_dma_cancel_faults);
    qtest_add_func("/esp32s3/rmt/dma-reassignment", test_dma_reassignment);
    qtest_add_func("/esp32s3/rmt/peer/ws-latch-hold-power-reset", test_peer_ws_latch_hold_power_reset);
    qtest_add_func("/esp32s3/rmt/peer/ws-disconnected", test_peer_ws_disconnected);
    qtest_add_func("/esp32s3/rmt/peer/nec-exact-reset", test_peer_nec_exact_reset);
    qtest_add_func("/esp32s3/rmt/peer/nec-power-cancel", test_peer_nec_power_cancel);
    qtest_add_func("/esp32s3/rmt/peer/nec-missing-pull", test_peer_nec_missing_pull);
    qtest_add_func("/esp32s3/rmt/peer/nec-disconnected", test_peer_nec_disconnected);
    qtest_add_func("/esp32s3/rmt/rx-live-clock-ref-reset", test_rx_live_clock_ref_reset);
    qtest_add_func("/esp32s3/rmt/rx-pending-filter-clock", test_rx_pending_filter_clock);
    return g_test_run();
}
