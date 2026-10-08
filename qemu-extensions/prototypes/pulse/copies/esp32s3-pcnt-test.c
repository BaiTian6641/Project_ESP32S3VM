/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Real machine PCNT register consumer. All stimulus traverses two distinct
 * MCU pads on a ProjectDocument v3 electrical net; no count-injection API.
 * Sources: pinned S3 pcnt_reg.h, pcnt_ll.h; ESP32-S3 TRM v1.8 chapter 38.
 */
#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qapi/error.h"
#include "qapi/qmp/qjson.h"
#include "qapi/qmp/qlist.h"
#include "qapi/qmp/qnum.h"
#include "libqtest.h"
#include "hw/gpio/esp32s3_gpio.h"
#include "hw/gpio/esp32s3_iomux.h"

#define BASE 0x60017000ULL
#define GPIO 0x60004000ULL
#define SYSTEM 0x600c0000ULL
#define IOMUX 0x60009000ULL
#define INPUT (R_IO_MUX_GPIOn_FUN_IE_MASK | (ESP32S3_IOMUX_MCU_SEL_GPIO << R_IO_MUX_GPIOn_MCU_SEL_SHIFT))
#define CONF(n, r) (12 * (n) + 4 * (r))
#define CNT(n) (0x30 + 4 * (n))
#define STATUS(n) (0x50 + 4 * (n))
#define RAW 0x40
#define ST 0x44
#define ENA 0x48
#define CLR 0x4c
#define CTRL 0x60
static QTestState *s;
static uint32_t ctrl;

static uint32_t rd(unsigned reg) { return qtest_readl(s, BASE + reg); }
static void wr(unsigned reg, uint32_t value) { qtest_writel(s, BASE + reg, value); }
static int count(unsigned unit) { return (int16_t)rd(CNT(unit)); }
static void step(unsigned ns) { qtest_clock_step(s, ns); }
static void pad(unsigned n, bool input)
{
    qtest_writel(s, IOMUX + IO_MUX_GPIOn_REG_OFFSET(n),
                 input ? INPUT : ESP32S3_IOMUX_MCU_SEL_GPIO << R_IO_MUX_GPIOn_MCU_SEL_SHIFT);
}
static void level(unsigned output, bool high)
{
    qtest_writel(s, GPIO + (high ? A_GPIO_OUT_W1TS : A_GPIO_OUT_W1TC), BIT(output));
}
static void sampled_pad(unsigned pad_number, bool high)
{
    QDict *r = qtest_qmp(s, "{'execute':'qom-get','arguments':{'path':'/machine/soc/electrical','property':'snapshot-json'}}");
    g_assert_false(qdict_haskey(r, "error"));
    QObject *o = qobject_from_json(qdict_get_str(r, "return"), &error_abort);
    QDict *snapshot = qobject_to(QDict, o);
    const QListEntry *entry;
    bool found = false;
    g_assert_nonnull(snapshot);
    QLIST_FOREACH_ENTRY(qdict_get_qlist(snapshot, "pads"), entry) {
        QDict *p = qobject_to(QDict, qlist_entry_obj(entry));
        if (qdict_get_int(p, "gpio") == pad_number) {
            QNum *voltage = qobject_to(QNum, qdict_get(p, "voltage_v"));
            g_assert_true(qdict_get_bool(p, "valid"));
            g_assert_nonnull(voltage);
            g_assert_cmpint(qnum_get_double(voltage) > 1.65, ==, high);
            found = true;
        }
    }
    g_assert_true(found);
    qobject_unref(o);
    qobject_unref(r);
}

static void pulse(unsigned width)
{
    level(4, true); sampled_pad(5, true); step(width);
    level(4, false); sampled_pad(5, false); step(width);
}
static void route(unsigned unit, unsigned channel, uint32_t pulse_route,
                  uint32_t control_route)
{
    qtest_writel(s, GPIO + GPIO_FUNC_IN_SEL_CFG_OFFSET(33 + 4 * unit + channel), pulse_route);
    qtest_writel(s, GPIO + GPIO_FUNC_IN_SEL_CFG_OFFSET(35 + 4 * unit + channel), control_route);
}
static void clear(unsigned unit)
{
    wr(CTRL, ctrl | BIT(2 * unit));
    wr(CTRL, ctrl);
    wr(CLR, BIT(unit));
}
static void configure(unsigned unit, uint32_t actions, int high, int low,
                      int threshold0, int threshold1)
{
    wr(CONF(unit, 0), actions);
    wr(CONF(unit, 1), (uint16_t)threshold0 | ((uint32_t)(uint16_t)threshold1 << 16));
    wr(CONF(unit, 2), (uint16_t)high | ((uint32_t)(uint16_t)low << 16));
    clear(unit);
}
static void irq(unsigned raw, unsigned enabled)
{
    g_assert_cmphex(rd(RAW), ==, raw);
    g_assert_cmphex(rd(ST), ==, raw & enabled);
    g_assert_cmpint(qtest_get_irq(s, 0), ==, !!(raw & enabled));
}

static void graph(bool connected)
{
    QDict *r = qtest_qmp(s,
        "{'execute':'qom-set','arguments':{'path':'/machine/soc/electrical',"
        "'property':'project-json','value':{"
        "'version':3,'id':'pcnt-native','name':'PCNT native',"
        "'profile':{'chip':'esp32s3','board':'esp32-s3-devkitc-1','module':'esp32-s3-wroom-1'},"
        "'firmware':{},'runtime':{'electrical':{'driver_profile':'s3-explicit-finite-v1','mode':'dc'}},"
        "'geometry':{'components':{},'nets':{}},'components':["
        "{'id':'U','name':'MCU','kind':'mcu','type':'mcu','parameters':{},'terminals':["
        "{'id':'U.vdd','name':'vdd','role':'vdd','domain':'power','direction':'input'},"
        "{'id':'U.gnd','name':'gnd','role':'gnd','domain':'ground','direction':'input'},"
        "{'id':'U.io4','name':'io4','role':'io4','domain':'digital','direction':'inout','gpio':4},"
        "{'id':'U.io5','name':'io5','role':'io5','domain':'digital','direction':'inout','gpio':5},"
        "{'id':'U.io6','name':'io6','role':'io6','domain':'digital','direction':'inout','gpio':6},"
        "{'id':'U.io7','name':'io7','role':'io7','domain':'digital','direction':'inout','gpio':7}]},"
        "{'id':'G','name':'Ground','kind':'ground','type':'ground','parameters':{},'terminals':["
        "{'id':'G.ref','name':'ref','role':'ref','domain':'ground','direction':'unspecified'}]},"
        "{'id':'V','name':'Supply','kind':'voltage-source','type':'voltage-source','parameters':{'voltage':{'value':3.3,'unit':'V'}},'terminals':["
        "{'id':'V.p','name':'p','role':'p','domain':'power','direction':'unspecified'},"
        "{'id':'V.n','name':'n','role':'n','domain':'ground','direction':'unspecified'}]}],"
        "'nets':[{'id':'gnd','name':'gnd','endpoints':['G.ref','V.n','U.gnd']},"
        "{'id':'vdd','name':'vdd','endpoints':['V.p','U.vdd']},"
        "{'id':'pulse','name':'pulse','endpoints':['U.io4'%s]},"
        "{'id':'control','name':'control','endpoints':['U.io6','U.io7']}]}}}",
        connected ? ",'U.io5'" : "");
    g_assert_false(qdict_haskey(r, "error"));
    qobject_unref(r);
}

static void setup(void)
{
    s = qtest_initf("-machine esp32s3 -S -L pc-bios -global driver=esp32s3.gpio,property=strap_mode,value=0x00");
    qtest_irq_intercept_out_named(s, "/machine/soc/pcnt", "sysbus-irq");
    qtest_writel(s, SYSTEM + 0x60, 0x00028401); /* Published CORE-02 APB 80MHz. */
    qtest_writel(s, SYSTEM + 0x18, qtest_readl(s, SYSTEM + 0x18) | BIT(10));
    for (unsigned n = 4; n <= 7; ++n) {
        pad(n, n == 5 || n == 7);
    }
    level(4, false); level(6, false);
    qtest_writel(s, GPIO + GPIO_FUNC_OUT_SEL_CFG_OFFSET(4), 256 | BIT(10));
    qtest_writel(s, GPIO + GPIO_FUNC_OUT_SEL_CFG_OFFSET(6), 256 | BIT(10));
    qtest_writel(s, GPIO + A_GPIO_ENABLE_W1TS, BIT(4) | BIT(6));
    graph(true);
    sampled_pad(5, false);
    sampled_pad(7, false);
    ctrl = 0;
    wr(CTRL, ctrl);
    for (unsigned n = 0; n < 4; ++n) {
        for (unsigned ch = 0; ch < 2; ++ch) {
            route(n, ch, BIT(7) | 5, BIT(7) | 7);
        }
    }
}

static void test_units_channels_actions(void)
{
    setup();
    for (unsigned n = 0; n < 4; ++n) {
        configure(n, BIT(18) | BIT(26), 10, -10, 2, -2);
    }
    pulse(1000);
    for (unsigned n = 0; n < 4; ++n) {
        g_assert_cmpint(count(n), ==, 2);
        /* Positive decrement, negative increment, high-control inversion. */
        configure(n, (2U << 18) | BIT(16) | BIT(20), 10, -10, 2, -2);
    }
    level(4, true); g_assert_cmpint(count(0), ==, -1);
    level(4, false); g_assert_cmpint(count(0), ==, 0);
    level(6, true);
    level(4, true); g_assert_cmpint(count(0), ==, 1);
    level(4, false); g_assert_cmpint(count(0), ==, 0);
    configure(0, BIT(18) | (2U << 20), 10, -10, 2, -2);
    pulse(1000); g_assert_cmpint(count(0), ==, 0); /* Control hold. */
    route(0, 0, BIT(7) | BIT(6) | 5, BIT(7) | 7);
    configure(0, BIT(16), 10, -10, 2, -2);
    pulse(1000); g_assert_cmpint(count(0), ==, 1); /* Input inversion. */
    qtest_quit(s);
}

static void test_watchpoints_limits_irq(void)
{
    setup();
    configure(0, BIT(18) | (31U << 11), 3, -3, 2, -2);
    wr(ENA, 0);
    pulse(1000); pulse(1000);
    irq(1, 0); g_assert_cmphex(rd(STATUS(0)) & BIT(3), ==, BIT(3));
    wr(ENA, 1); irq(1, 1);
    wr(CLR, 2); irq(1, 1); /* Other unit W1C must not clear unit0. */
    wr(CLR, 1); irq(0, 1);
    pulse(1000); g_assert_cmpint(count(0), ==, 0);
    irq(1, 1); g_assert_cmphex(rd(STATUS(0)) & BIT(5), ==, BIT(5));
    configure(0, (2U << 18) | (31U << 11), 3, -3, 2, -2);
    pulse(1000); pulse(1000);
    g_assert_cmpint(count(0), ==, -2);
    g_assert_cmphex(rd(STATUS(0)) & BIT(2), ==, BIT(2));
    wr(CLR, 1); pulse(1000);
    g_assert_cmpint(count(0), ==, 0);
    g_assert_cmphex(rd(STATUS(0)) & BIT(4), ==, BIT(4));
    configure(0, BIT(18) | (2U << 16) | BIT(11), 10, -10, 2, -2);
    level(4, true); level(4, false);
    irq(1, 1); g_assert_cmphex(rd(STATUS(0)) & 3, ==, 0);
    configure(0, BIT(18), 2, -2, 0, 0);
    pulse(1000); pulse(1000);
    g_assert_cmpint(count(0), ==, 0); irq(0, 1); /* Limit reset without event enable. */
    qtest_quit(s);
}

static void test_filters_stop_reset_gate(void)
{
    setup();
    configure(0, BIT(18) | (2U << 20) | BIT(10) | 16, 100, -100, 0, 0);
    pulse(100); g_assert_cmpint(count(0), ==, 0); /* 100ns < 16 APB cycles. */
    pulse(250); g_assert_cmpint(count(0), ==, 1);
    level(6, true); step(100); level(6, false); step(250);
    pulse(250); g_assert_cmpint(count(0), ==, 2); /* Control glitch rejected too. */
    level(6, true); step(250); pulse(250);
    g_assert_cmpint(count(0), ==, 2); /* Stable filtered control holds. */
    level(6, false); step(250);
    ctrl = BIT(1); wr(CTRL, ctrl); pulse(250);
    g_assert_cmpint(count(0), ==, 2);
    ctrl = 0; wr(CTRL, ctrl); pulse(250);
    g_assert_cmpint(count(0), ==, 3);
    clear(0); g_assert_cmpint(count(0), ==, 0);
    wr(CNT(0), 123); g_assert_cmpint(count(0), ==, 0); /* RO, no injection. */
    qtest_writel(s, SYSTEM + 0x18, qtest_readl(s, SYSTEM + 0x18) & ~BIT(10));
    pulse(250); g_assert_cmpint(count(0), ==, 0);
    qtest_writel(s, SYSTEM + 0x18, qtest_readl(s, SYSTEM + 0x18) | BIT(10));
    pulse(250); g_assert_cmpint(count(0), ==, 1);
    qtest_writel(s, SYSTEM + 0x20, qtest_readl(s, SYSTEM + 0x20) | BIT(10));
    g_assert_cmphex(rd(CTRL), ==, 0x55);
    g_assert_cmphex(rd(CONF(0, 0)), ==, 0x3c10);
    wr(CONF(0, 0), BIT(18)); g_assert_cmphex(rd(CONF(0, 0)), ==, 0x3c10);
    qtest_writel(s, SYSTEM + 0x20, qtest_readl(s, SYSTEM + 0x20) & ~BIT(10));
    g_assert_cmpint(count(0), ==, 0);
    qtest_quit(s);
}

static void test_route_validity(void)
{
    setup(); configure(0, BIT(18), 100, -100, 0, 0);
    pulse(1000); g_assert_cmpint(count(0), ==, 1);
    pad(5, false); pulse(1000); g_assert_cmpint(count(0), ==, 1);
    pad(5, true); pulse(1000); g_assert_cmpint(count(0), ==, 2);
    route(0, 0, 5, BIT(7) | 7); /* Bypassed input is not valid low. */
    pulse(1000); g_assert_cmpint(count(0), ==, 2);
    route(0, 0, BIT(7) | 5, BIT(7) | 7);
    graph(false);
    level(4, true); step(1000); level(4, false); step(1000);
    g_assert_cmpint(count(0), ==, 2);
    graph(true); pulse(1000); g_assert_cmpint(count(0), ==, 3);
    qtest_quit(s);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/esp32s3/pcnt/units-channels-actions", test_units_channels_actions);
    qtest_add_func("/esp32s3/pcnt/watchpoints-limits-irq", test_watchpoints_limits_irq);
    qtest_add_func("/esp32s3/pcnt/filters-stop-reset-gate", test_filters_stop_reset_gate);
    qtest_add_func("/esp32s3/pcnt/route-validity", test_route_validity);
    return g_test_run();
}
