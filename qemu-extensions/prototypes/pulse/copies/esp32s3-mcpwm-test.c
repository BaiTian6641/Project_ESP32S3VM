/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Real group MMIO and electrical ProjectDocument v3 nets, not GPIO injection.
 * Run after the pulse machine overlay is ready; this worker did not run checks.
 * Reference: pinned S3 mcpwm_ll.h, mcpwm_struct.h, gpio_sig_map.h;
 * https://documentation.espressif.com/esp32-s3_technical_reference_manual_en.pdf ch36. */
#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qapi/error.h"
#include "qapi/qmp/qdict.h"
#include "qapi/qmp/qjson.h"
#include "qapi/qmp/qlist.h"
#include "libqtest.h"
#include "hw/gpio/esp32s3_gpio.h"
#include "hw/gpio/esp32s3_iomux.h"
#define SYS(a) (0x600c0000ULL + (a))
#define GPIO(a) (0x60004000ULL + (a))
#define PAD(n) (0x60009000ULL + IO_MUX_GPIOn_REG_OFFSET(n))
#define INPUT (R_IO_MUX_GPIOn_FUN_IE_MASK | (ESP32S3_IOMUX_MCU_SEL_GPIO << R_IO_MUX_GPIOn_MCU_SEL_SHIFT))
#define T(t,a) (4 + 16 * (t) + (a))
#define O(o,a) (0x3c + 0x38 * (o) + (a))
static QTestState *q;
static unsigned group;
static uint64_t base(void) { return group ? 0x6002c000ULL : 0x6001e000ULL; }
static uint32_t rd(unsigned a) { return qtest_readl(q, base() + a); }
static void wr(unsigned a, uint32_t v) { qtest_writel(q, base() + a, v); }
static void step(uint64_t ns) { qtest_clock_step(q, ns); }
static unsigned out(unsigned o, unsigned g) { return 4 + 6 * group + 2 * o + g; }
static void level(unsigned pad, bool high)
{
    uint32_t v = qtest_readl(q, GPIO(A_GPIO_IN));
    g_assert_cmphex(v & BIT(pad), ==, high ? BIT(pad) : 0);
}
static void matrix(unsigned signal, unsigned pad)
{
    qtest_writel(q, GPIO(GPIO_FUNC_IN_SEL_CFG_OFFSET(signal)), BIT(7) | pad);
}
static void source(bool high)
{
    qtest_writel(q, GPIO(high ? A_GPIO_OUT_W1TS : A_GPIO_OUT_W1TC), BIT(16));
    step(1); /* bounded publication delta, not a fabricated injected edge */
}
static void graph(void)
{
    GString *j = g_string_new(
        "{\"version\":3,\"id\":\"mcpwm-native\",\"name\":\"MCPWM native\","
        "\"profile\":{\"chip\":\"esp32s3\",\"board\":\"esp32-s3-devkitc-1\",\"module\":\"esp32-s3-wroom-1\"},"
        "\"firmware\":{},\"runtime\":{\"electrical\":{\"driver_profile\":\"s3-explicit-finite-v1\",\"mode\":\"dc\"}},"
        "\"geometry\":{\"components\":{},\"nets\":{}},\"components\":["
        "{\"id\":\"U\",\"name\":\"MCU\",\"kind\":\"mcu\",\"type\":\"mcu\",\"parameters\":{},\"terminals\":["
        "{\"id\":\"U.vdd\",\"name\":\"vdd\",\"role\":\"vdd\",\"domain\":\"power\",\"direction\":\"input\"},"
        "{\"id\":\"U.gnd\",\"name\":\"gnd\",\"role\":\"gnd\",\"domain\":\"ground\",\"direction\":\"input\"}");
    for (unsigned p = 4; p <= 17; p++) {
        g_string_append_printf(j, ",{\"id\":\"U.io%u\",\"name\":\"io%u\",\"role\":\"io%u\",\"domain\":\"digital\",\"direction\":\"inout\",\"gpio\":%u}", p, p, p, p);
    }
    g_string_append(j,
        "]},{\"id\":\"G\",\"name\":\"Ground\",\"kind\":\"ground\",\"type\":\"ground\",\"parameters\":{},\"terminals\":["
        "{\"id\":\"G.ref\",\"name\":\"ref\",\"role\":\"ref\",\"domain\":\"ground\",\"direction\":\"unspecified\"}]},"
        "{\"id\":\"V\",\"name\":\"Supply\",\"kind\":\"voltage-source\",\"type\":\"voltage-source\",\"parameters\":{\"voltage\":{\"value\":3.3,\"unit\":\"V\"}},\"terminals\":["
        "{\"id\":\"V.p\",\"name\":\"p\",\"role\":\"p\",\"domain\":\"power\",\"direction\":\"unspecified\"},"
        "{\"id\":\"V.n\",\"name\":\"n\",\"role\":\"n\",\"domain\":\"ground\",\"direction\":\"unspecified\"}]}");
    for (unsigned p = 4; p <= 16; p++) {
        g_string_append_printf(j,
            ",{\"id\":\"R%u\",\"name\":\"R%u\",\"kind\":\"resistor\",\"type\":\"resistor\",\"parameters\":{\"resistance\":{\"value\":10000,\"unit\":\"ohm\"}},\"terminals\":["
            "{\"id\":\"R%u.a\",\"name\":\"a\",\"role\":\"a\",\"domain\":\"passive\",\"direction\":\"passive\"},"
            "{\"id\":\"R%u.b\",\"name\":\"b\",\"role\":\"b\",\"domain\":\"passive\",\"direction\":\"passive\"}]}", p, p, p, p);
    }
    g_string_append(j, "],\"nets\":[{\"id\":\"gnd\",\"name\":\"gnd\",\"endpoints\":[\"G.ref\",\"V.n\",\"U.gnd\"");
    for (unsigned p = 4; p <= 16; p++) {
        g_string_append_printf(j, ",\"R%u.b\"", p);
    }
    g_string_append(j, "]},{\"id\":\"vdd\",\"name\":\"vdd\",\"endpoints\":[\"V.p\",\"U.vdd\"]}");
    for (unsigned p = 4; p <= 16; p++) {
        g_string_append_printf(j, ",{\"id\":\"p%u\",\"name\":\"p%u\",\"endpoints\":[\"U.io%u\",\"R%u.a\"]}", p, p, p, p);
    }
    /* Pad17 deliberately floating: negative capture/fault dependency net. */
    g_string_append(j, ",{\"id\":\"floating\",\"name\":\"floating\",\"endpoints\":[\"U.io17\"]}]}");
    QDict *r = qtest_qmp(q, "{'execute':'qom-set','arguments':{'path':'/machine/soc/electrical','property':'project-json','value':%s}}", j->str);
    g_assert_false(qdict_haskey(r, "error"));
    qobject_unref(r);
    g_string_free(j, true);
}
static void setup(unsigned g)
{
    group = g;
    q = qtest_init("-machine esp32s3 -S -L pc-bios -global driver=esp32s3.gpio,property=strap_mode,value=0x00");
    qtest_irq_intercept_out_named(q,
        group ? "/machine/soc/mcpwm1" : "/machine/soc/mcpwm0", "sysbus-irq");
    qtest_writel(q, SYS(0x60), 0x00028401);
    qtest_writel(q, SYS(0x18), qtest_readl(q, SYS(0x18)) | BIT(17) | BIT(20));
    for (unsigned p = 4; p <= 17; p++) {
        qtest_writel(q, PAD(p), INPUT);
        if (p < 16) {
            qtest_writel(q, GPIO(GPIO_FUNC_OUT_SEL_CFG_OFFSET(p)), 156 + p);
        }
    }
    qtest_writel(q, GPIO(A_GPIO_ENABLE_W1TS), BIT(16));
    graph();
    wr(0, 15); /* group 10MHz: 100ns per tick */
    wr(0x38, 0 | (1 << 2) | (2 << 4));
}
static void configure(unsigned t, unsigned period, unsigned a, unsigned b, unsigned mode)
{
    wr(T(t, 0), period << 8);
    wr(O(t, 4), a);
    wr(O(t, 8), b);
    wr(O(t, 0x14), 2 | (1 << 4)); /* A high at zero, low at compare A */
    wr(O(t, 0x18), 1 | (2 << 6)); /* B low at zero, high at compare B */
    wr(T(t, 4), 2 | (mode << 3));
}
static void test_both_groups(void)
{
    for (unsigned g = 0; g < 2; g++) {
        setup(g);
        configure(0, 9, 3, 6, 1);
        configure(1, 19, 7, 12, 1);
        configure(2, 29, 11, 18, 1);
        for (unsigned o = 0; o < 3; o++) {
            level(out(o, 0), true); level(out(o, 1), false);
        }
        step(301);
        level(out(0, 0), false); level(out(1, 0), true); level(out(2, 0), true);
        step(300); level(out(0, 1), true);
        step(100); level(out(1, 0), false);
        step(300); level(out(0, 0), true); level(out(0, 1), false);
        step(100); level(out(2, 0), false);
        wr(0x110, BIT(15) | BIT(16) | BIT(17));
        g_assert_true(qtest_get_irq(q, 0));
        uint32_t raw = rd(0x114);
        wr(0x114, 0); /* RAW is W1C, not an ordinary writable register. */
        g_assert_cmphex(rd(0x114), ==, raw);
        wr(0x114, BIT(15));
        g_assert_cmphex(rd(0x114), ==, raw & ~BIT(15));
        g_assert_true(qtest_get_irq(q, 0)); /* other compare events retained */
        wr(0x11c, rd(0x114));
        g_assert_cmphex(rd(0x118), ==, 0);
        g_assert_false(qtest_get_irq(q, 0));
        qtest_quit(q);
    }
}
static void test_shadows_and_stop(void)
{
    setup(0); configure(0, 9, 3, 6, 1);
    wr(O(0, 0), 1); /* compare A shadow at TEZ */
    wr(O(0, 4), 7);
    g_assert_cmphex(rd(O(0, 0)) & BIT(8), ==, BIT(8));
    step(301); level(4, false);
    step(700); level(4, true);
    g_assert_cmphex(rd(O(0, 0)) & BIT(8), ==, 0);
    step(600); level(4, true);
    step(100); level(4, false);
    wr(T(0, 4), 8); /* stop at next TEZ */
    step(300);
    g_assert_cmphex(rd(0x114) & BIT(0), ==, BIT(0));
    uint32_t stopped = rd(T(0, 12));
    step(5000); g_assert_cmphex(rd(T(0, 12)), ==, stopped);
    wr(T(0, 4), 3 | 8); /* run exactly until NEXT empty, not immediate stop */
    wr(0x11c, UINT32_MAX); step(999);
    g_assert_cmphex(rd(0x114) & BIT(0), ==, 0);
    step(1); g_assert_cmphex(rd(0x114) & BIT(0), ==, BIT(0));
    qtest_quit(q);
}
static void test_count_sync_capture(void)
{
    setup(1); configure(0, 10, 3, 7, 3);
    wr(O(0, 0x14), 2 | (1 << 4) | (2 << 16));
    step(301); level(10, false);
    step(1400); level(10, true); /* down-compare A */
    matrix(169, 16);
    wr(0x34, 4);
    wr(T(0, 8), 1 | (5 << 4));
    source(true);
    unsigned status = rd(T(0, 12));
    g_assert_cmpuint(status & 0xffff, ==, 6); /* status is next-count */
    matrix(175, 16);
    wr(0xe8, 1);
    wr(0xf0, 1 | BIT(1) | BIT(2));
    wr(0x110, BIT(27));
    source(false);
    uint32_t v0 = rd(0xfc);
    step(1000); source(true);
    g_assert_cmpuint(rd(0xfc) - v0, >=, 80);
    g_assert_cmpuint(rd(0xfc) - v0, <=, 81);
    g_assert_cmphex(rd(0x108) & 1, ==, 0);
    g_assert_true(qtest_get_irq(q, 0));
    wr(0xec, 123456);
    wr(0xe8, 1 | 2 | BIT(5));
    wr(0xf0, 1 | BIT(12));
    g_assert_cmpuint(rd(0xfc), ==, 123456);
    g_assert_cmphex(rd(0xe8) & BIT(5), ==, 0);
    g_assert_cmphex(rd(0xf0) & BIT(12), ==, 0);
    qtest_quit(q);
}
static void test_deadtime_force_carrier(void)
{
    setup(0);
    /* Classic active-high complementary from A; RED=200ns, FED=300ns. */
    wr(O(0, 0x24), 1); wr(O(0, 0x20), 2);
    wr(O(0, 0x1c), BIT(14));
    configure(0, 19, 10, 15, 1);
    step(199); level(4, false);
    step(2); level(4, true); level(5, false);
    step(800); level(4, false); level(5, false);
    step(299); level(5, true);
    wr(O(0, 0x1c), BIT(15) | BIT(16));
    wr(O(0, 0x10), 2 << 6); /* continuous force A high */
    level(4, true); step(5000); level(4, true);
    wr(O(0, 0x28), 1 | (4 << 5)); /* carrier 1st 800ns, then 50% */
    level(4, true); step(800); level(4, false);
    step(400); level(4, true); step(400); level(4, false);
    wr(O(0, 0x28), 0);
    wr(O(0, 0x10), BIT(10) | (1 << 11)); /* NCI force low, release CNTU */
    level(4, false);
    qtest_quit(q);
}
static void test_fault_brake(void)
{
    setup(0); configure(0, 9, 8, 6, 1);
    matrix(163, 16);
    wr(0xe4, 1 | BIT(3)); /* active high F0 */
    wr(O(0, 0x2c), BIT(3) | BIT(7) | (1 << 10) | (2 << 14));
    wr(O(0, 0x30), BIT(1)); /* CBC recovers at TEZ */
    source(true);
    step(100); /* fault detector samples on PWM_clk, not observer arrival */
    level(4, true); /* OST wins over CBC low */
    g_assert_cmphex(rd(O(0, 0x34)), ==, 3);
    g_assert_cmphex(rd(0x114) & (BIT(9) | BIT(21) | BIT(24)), ==,
                    BIT(9) | BIT(21) | BIT(24));
    source(false);
    step(100);
    wr(O(0, 0x30), BIT(1) | 1); /* clear OST, retained CBC forces low */
    level(4, false);
    step(1000); level(4, true);
    g_assert_cmphex(rd(O(0, 0x34)), ==, 0);
    /* Software brake toggle does not depend on an external route. */
    wr(O(0, 0x2c), BIT(4) | (1 << 14));
    wr(O(0, 0x30), BIT(4)); level(4, false);
    qtest_quit(q);
}
static void test_gate_reset_phase(void)
{
    setup(0); configure(0, 9, 3, 6, 1);
    step(150);
    uint32_t before = rd(T(0, 12));
    wr(0x110, BIT(3)); /* pending TEZ level survives a function-clock gate */
    g_assert_true(qtest_get_irq(q, 0));
    qtest_writel(q, SYS(0x18), qtest_readl(q, SYS(0x18)) & ~BIT(17));
    step(10000); g_assert_cmphex(rd(T(0, 12)), ==, before);
    level(4, true); /* generator and physical source hold their high state */
    g_assert_true(qtest_get_irq(q, 0));
    wr(0x114, 0);
    g_assert_true(qtest_get_irq(q, 0));
    wr(0x114, BIT(3));
    g_assert_false(qtest_get_irq(q, 0));
    qtest_writel(q, SYS(0x18), qtest_readl(q, SYS(0x18)) | BIT(17));
    step(149); level(4, true); step(1); level(4, false);
    qtest_writel(q, SYS(0x20), qtest_readl(q, SYS(0x20)) | BIT(17));
    step(5000); g_assert_cmphex(rd(0x114), ==, 0);
    qtest_writel(q, SYS(0x20), qtest_readl(q, SYS(0x20)) & ~BIT(17));
    g_assert_cmphex(rd(T(0, 0)), ==, 255 << 8);
    g_assert_cmphex(rd(O(0, 0x14)), ==, 0);
    qtest_quit(q);
}

static void test_ost_clear_active_fault(void)
{
    setup(0); configure(0, 9, 8, 6, 1);
    matrix(163, 16);
    wr(0xe4, 1 | BIT(3));
    wr(O(0, 0x2c), BIT(7) | (1 << 14)); /* F0 OST forces A low */
    source(true); step(100);
    level(4, false);
    g_assert_cmphex(rd(O(0, 0x34)), ==, 2);
    wr(O(0, 0x30), 1); /* documented rising edge clears ongoing OST */
    level(4, true);
    g_assert_cmphex(rd(O(0, 0x34)), ==, 0);
    step(100);
    g_assert_cmphex(rd(O(0, 0x34)), ==, 0); /* no new fault activation */
    source(false); step(100);
    wr(0x11c, UINT32_MAX);
    source(true); step(100);
    level(4, false);
    g_assert_cmphex(rd(O(0, 0x34)), ==, 2);
    g_assert_cmphex(rd(0x114) & (BIT(9) | BIT(24)), ==, BIT(9) | BIT(24));
    qtest_quit(q);
}

static void test_nci_next_event_toggle(void)
{
    setup(0); configure(0, 9, 3, 6, 1);
    wr(O(0, 0x14), 2 | (3 << 4)); /* TEZ high, compare-A toggles output */
    step(100); level(4, true);
    wr(O(0, 0x10), BIT(10) | (1 << 11)); /* immediate NCI low */
    level(4, false);
    step(199); level(4, false);
    step(1); level(4, true); /* next active timing event toggles forced output */
    step(700); level(4, true); /* NCI is not a continuously held override */
    qtest_quit(q);
}

static void test_down_and_fractional_divider(void)
{
    setup(0);
    wr(T(0, 0), 9 << 8);
    wr(O(0, 4), 3);
    wr(O(0, 0x14), (2 << 14) | (1 << 16)); /* DTEP high, DTEA low */
    wr(T(0, 4), 2 | (2 << 3));
    level(4, true);
    g_assert_cmphex(rd(T(0, 12)), ==, BIT(16) | 8);
    step(599); level(4, true);
    step(1); level(4, false);
    wr(T(0, 8), 1 | (8 << 4) | BIT(1)); /* software sync to count 8 */
    g_assert_cmphex(rd(T(0, 12)), ==, BIT(16) | 7);
    step(50);
    wr(0, 31); /* 100ns tick -> 200ns tick, retain half-tick phase */
    step(99);
    g_assert_cmphex(rd(T(0, 12)), ==, BIT(16) | 7);
    step(1);
    g_assert_cmphex(rd(T(0, 12)), ==, BIT(16) | 6);
    qtest_quit(q);
}
static void test_internal_sync_and_capture_prescale(void)
{
    setup(0);
    configure(0, 9, 3, 6, 1);
    configure(1, 99, 30, 60, 1);
    wr(T(0, 8), 1 << 2); /* timer0 TEZ -> sync_out */
    wr(0x34, 1 << 3); /* timer1 input = timer0 sync_out */
    wr(T(1, 8), 1 | (4 << 4));
    step(1000);
    g_assert_cmpuint(rd(T(1, 12)) & 0xffff, ==, 5);
    wr(O(1, 0), 8); /* disable compare shadow update */
    wr(O(1, 4), 45);
    g_assert_cmphex(rd(O(1, 0)) & BIT(8), ==, BIT(8));
    wr(0x10c, rd(0x10c) ^ BIT(5)); /* operator1 force-update toggle */
    g_assert_cmphex(rd(O(1, 0)) & BIT(8), ==, 0);
    matrix(166, 16);
    wr(0xe8, 1);
    wr(0xf0, 1 | BIT(1) | BIT(2) | (1 << 3)); /* positive-edge /2 */
    wr(0x11c, UINT32_MAX);
    source(true); source(false);
    g_assert_cmphex(rd(0x114) & BIT(27), ==, 0);
    step(100);
    source(true);
    g_assert_cmphex(rd(0x114) & BIT(27), ==, BIT(27));
    wr(0x11c, BIT(27));
    source(false);
    g_assert_cmphex(rd(0x108) & 1, ==, 1);
    g_assert_cmphex(rd(0x114) & BIT(27), ==, BIT(27));
    qtest_quit(q);
}
static void test_negative_capture(void)
{
    for (unsigned variant = 0; variant < 3; variant++) {
        setup(0);
        unsigned pad = variant == 0 ? 17 : 16;
        matrix(166, pad);
        if (variant == 1) {
            qtest_writel(q, PAD(16), ESP32S3_IOMUX_MCU_SEL_GPIO << R_IO_MUX_GPIOn_MCU_SEL_SHIFT);
        } else if (variant == 2) {
            qtest_writel(q, GPIO(GPIO_FUNC_IN_SEL_CFG_OFFSET(166)), 16); /* bypass */
        }
        wr(0xe8, 1); wr(0xf0, 1 | BIT(1) | BIT(2));
        source(true); source(false);
        g_assert_cmphex(rd(0x114) & BIT(27), ==, 0);
        g_assert_cmpuint(rd(0xfc), ==, 0);
        qtest_quit(q);
    }
}
int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/esp32s3/mcpwm/both-groups-six-generators", test_both_groups);
    qtest_add_func("/esp32s3/mcpwm/shadow-stop-single-cycle", test_shadows_and_stop);
    qtest_add_func("/esp32s3/mcpwm/updown-sync-capture", test_count_sync_capture);
    qtest_add_func("/esp32s3/mcpwm/deadtime-force-carrier", test_deadtime_force_carrier);
    qtest_add_func("/esp32s3/mcpwm/fault-cbc-ost-priority", test_fault_brake);
    qtest_add_func("/esp32s3/mcpwm/gate-reset-phase", test_gate_reset_phase);
    qtest_add_func("/esp32s3/mcpwm/down-fractional-divider", test_down_and_fractional_divider);
    qtest_add_func("/esp32s3/mcpwm/internal-sync-capture-prescale", test_internal_sync_and_capture_prescale);
    qtest_add_func("/esp32s3/mcpwm/negative-physical-inputs", test_negative_capture);
    qtest_add_func("/esp32s3/mcpwm/ost-clear-active-fault", test_ost_clear_active_fault);
    qtest_add_func("/esp32s3/mcpwm/nci-next-event-toggle", test_nci_next_event_toggle);
    return g_test_run();
}
