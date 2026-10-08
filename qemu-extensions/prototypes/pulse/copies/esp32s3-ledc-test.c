/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SOC01 register/comparator qualification over powered ProjectDocument nets.
 * No GPIO/IRQ/sample injection. Timing oracle: TRM ch35, XTAL 40 MHz.
 * Firmware qualification of IDF fade callbacks/LEDC->PCNT is separately owned
 * by tests/firmware/pulse_native. This test does not claim silicon validation. */
#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qapi/error.h"
#include "qapi/qmp/qjson.h"
#include "qapi/qmp/qlist.h"
#include "qapi/qmp/qnum.h"
#include "libqtest.h"
#include "hw/gpio/esp32s3_gpio.h"
#include "hw/gpio/esp32s3_iomux.h"
#include "hw/misc/esp32s3_ledc.h"

#define BASE 0x60019000ULL
#define GPIO(a) (0x60004000ULL + (a))
#define SYS(a) (0x600c0000ULL + (a))
#define CH(c, r) ((c) * 20 + (r))
#define TCONF(t) (0xa0 + (t) * 8)
#define TVALUE(t) (0xa4 + (t) * 8)
#define UPDATE BIT(25)
#define RESET BIT(23)
#define PAUSE BIT(22)
#define DIV (40U * 256U << 4)
static QTestState *s;

static uint32_t rd(unsigned r) { return qtest_readl(s, BASE + r); }
static void wr(unsigned r, uint32_t v) { qtest_writel(s, BASE + r, v); }
static void step(unsigned ns) { qtest_clock_step(s, ns); }

static void graph(void)
{
    const char *json =
        "{\"version\":3,\"id\":\"ledc-native\",\"name\":\"LEDC physical net\","
        "\"profile\":{\"chip\":\"esp32s3\",\"board\":\"esp32-s3-devkitc-1\",\"module\":\"esp32-s3-wroom-1\"},"
        "\"firmware\":{},\"runtime\":{\"electrical\":{\"driver_profile\":\"s3-explicit-finite-v1\",\"mode\":\"dc\"}},"
        "\"geometry\":{\"components\":{},\"nets\":{}},\"components\":["
        "{\"id\":\"U\",\"name\":\"MCU\",\"kind\":\"mcu\",\"type\":\"mcu\",\"parameters\":{},\"terminals\":["
        "{\"id\":\"U.vdd\",\"name\":\"vdd\",\"role\":\"vdd\",\"domain\":\"power\",\"direction\":\"input\"},"
        "{\"id\":\"U.gnd\",\"name\":\"gnd\",\"role\":\"gnd\",\"domain\":\"ground\",\"direction\":\"input\"},"
        "{\"id\":\"U.io4\",\"name\":\"io4\",\"role\":\"io4\",\"domain\":\"digital\",\"direction\":\"inout\",\"gpio\":4},"
        "{\"id\":\"U.io5\",\"name\":\"io5\",\"role\":\"io5\",\"domain\":\"digital\",\"direction\":\"inout\",\"gpio\":5}]},"
        "{\"id\":\"G\",\"name\":\"Ground\",\"kind\":\"ground\",\"type\":\"ground\",\"parameters\":{},\"terminals\":["
        "{\"id\":\"G.ref\",\"name\":\"ref\",\"role\":\"ref\",\"domain\":\"ground\",\"direction\":\"unspecified\"}]},"
        "{\"id\":\"V\",\"name\":\"Supply\",\"kind\":\"voltage-source\",\"type\":\"voltage-source\",\"parameters\":{\"voltage\":{\"value\":3.3,\"unit\":\"V\"}},\"terminals\":["
        "{\"id\":\"V.p\",\"name\":\"p\",\"role\":\"p\",\"domain\":\"power\",\"direction\":\"unspecified\"},"
        "{\"id\":\"V.n\",\"name\":\"n\",\"role\":\"n\",\"domain\":\"ground\",\"direction\":\"unspecified\"}]}],"
        "\"nets\":[{\"id\":\"gnd\",\"name\":\"gnd\",\"endpoints\":[\"G.ref\",\"V.n\",\"U.gnd\"]},"
        "{\"id\":\"vdd\",\"name\":\"vdd\",\"endpoints\":[\"V.p\",\"U.vdd\"]},"
        "{\"id\":\"pulse\",\"name\":\"pulse\",\"endpoints\":[\"U.io4\",\"U.io5\"]}]}";
    QDict *r = qtest_qmp(s, "{'execute':'qom-set','arguments':{'path':'/machine/soc/electrical','property':'project-json','value':%s}}", json);
    g_assert_false(qdict_haskey(r, "error"));
    qobject_unref(r);
}

static void sample(bool high)
{
    uint32_t input = qtest_readl(s, GPIO(A_GPIO_IN));
    g_assert_cmphex(input & BIT(5), ==, high ? BIT(5) : 0);
    QDict *r = qtest_qmp(s, "{'execute':'qom-get','arguments':{'path':'/machine/soc/electrical','property':'snapshot-json'}}");
    g_assert_false(qdict_haskey(r, "error"));
    QObject *o = qobject_from_json(qdict_get_str(r, "return"), &error_abort);
    QDict *snap = qobject_to(QDict, o);
    bool found = false;
    const QListEntry *e;
    QLIST_FOREACH_ENTRY(qdict_get_qlist(snap, "pads"), e) {
        QDict *p = qobject_to(QDict, qlist_entry_obj(e));
        if (qdict_get_int(p, "gpio") == 5) {
            QNum *v = qobject_to(QNum, qdict_get(p, "voltage_v"));
            g_assert_true(qdict_get_bool(p, "valid"));
            g_assert_nonnull(v);
            g_assert_true(high ? qnum_get_double(v) > 2.4 : qnum_get_double(v) < 0.8);
            found = true;
        }
    }
    g_assert_true(found);
    qobject_unref(o);
    qobject_unref(r);
}

static void setup(unsigned channel, unsigned timer, unsigned hp, unsigned duty)
{
    s = qtest_initf("-machine esp32s3 -S -L pc-bios -global driver=esp32s3.gpio,property=strap_mode,value=0x00");
    qtest_irq_intercept_out_named(s, "/machine/soc/ledc", "sysbus-irq");
    qtest_writel(s, SYS(0x18), qtest_readl(s, SYS(0x18)) | BIT(11));
    wr(0xd0, 3); /* CLK_EN=0 is automatic register gating, not PWM-off. */
    unsigned mux = R_IO_MUX_GPIOn_FUN_IE_MASK |
                   (ESP32S3_IOMUX_MCU_SEL_GPIO << R_IO_MUX_GPIOn_MCU_SEL_SHIFT);
    for (unsigned pad = 4; pad <= 5; ++pad) {
        qtest_writel(s, 0x60009000ULL + IO_MUX_GPIOn_REG_OFFSET(pad), mux);
    }
    qtest_writel(s, GPIO(GPIO_FUNC_OUT_SEL_CFG_OFFSET(4)),
                 ESP32S3_LEDC_MATRIX_BASE + channel);
    graph();
    g_assert_cmphex(rd(0xfc), ==, 0x19040200);
    wr(TCONF(timer), DIV | 4 | RESET | UPDATE);
    wr(CH(channel, 4), hp);
    wr(CH(channel, 8), duty << 4);
    wr(CH(channel, 12), BIT(30));
    wr(CH(channel, 0), timer | BIT(2) | BIT(4));
    wr(TCONF(timer), DIV | 4);
}

static void test_channels(void)
{
    for (unsigned ch = 0; ch < 8; ++ch) {
        unsigned t = ch % 4;
        setup(ch, t, 4, 8);
        sample(false);
        step(3999); sample(false);
        step(1); sample(true);
        g_assert_cmpuint(rd(TVALUE(t)), ==, 4);
        step(7999); sample(true);
        step(1); sample(false);
        step(4000);
        g_assert_cmphex(rd(0xc0) & BIT(t), ==, BIT(t));
        wr(0xc8, BIT(t));
        g_assert_true(qtest_get_irq(s, 0));
        wr(0xcc, BIT(t));
        g_assert_false(qtest_get_irq(s, 0));
        qtest_quit(s);
    }
}

static void test_shadow_pause_gate(void)
{
    setup(0, 0, 0, 4);
    sample(true);
    step(2000);
    wr(CH(0, 8), 8 << 4);
    step(2000); sample(false); /* no PARA_UP: old width */
    wr(CH(0, 0), BIT(2) | BIT(4));
    step(12000); sample(true);
    step(4000); sample(true); /* new duty sampled on overflow */
    wr(TCONF(0), DIV | 4 | PAUSE);
    step(100000); sample(true);
    g_assert_cmpuint(rd(TVALUE(0)), ==, 4);
    wr(TCONF(0), DIV | 4);
    step(3999); sample(true);
    step(1); sample(false);
    qtest_writel(s, SYS(0x18), qtest_readl(s, SYS(0x18)) & ~BIT(11));
    step(100000); sample(false);
    qtest_writel(s, SYS(0x18), qtest_readl(s, SYS(0x18)) | BIT(11));
    step(8000); sample(true);
    wr(CH(0, 0), BIT(3) | BIT(4));
    step(16000); sample(true); /* disabled means driven idle, not high-Z */
    qtest_writel(s, SYS(0x20), qtest_readl(s, SYS(0x20)) | BIT(11));
    sample(false);
    qtest_writel(s, SYS(0x20), qtest_readl(s, SYS(0x20)) & ~BIT(11));
    g_assert_cmphex(rd(TCONF(0)), ==, RESET);
    g_assert_cmphex(rd(CH(0, 12)), ==, BIT(30));
    qtest_quit(s);
}

static void test_fade_overflow_irq(void)
{
    setup(7, 3, 0, 4);
    wr(CH(7, 12), BIT(31) | BIT(30) | (2U << 20) | (1U << 10) | 1);
    wr(CH(7, 0), 3 | BIT(2) | BIT(4) | BIT(15) | (1U << 5));
    wr(0xc8, BIT(11) | BIT(19));
    step(16000);
    g_assert_cmpuint(rd(CH(7, 16)), ==, 5 << 4);
    g_assert_false(qtest_get_irq(s, 0));
    step(16000);
    g_assert_cmpuint(rd(CH(7, 16)), ==, 6 << 4);
    g_assert_cmphex(rd(0xc4), ==, BIT(11) | BIT(19));
    g_assert_true(qtest_get_irq(s, 0));
    wr(0xcc, UINT32_MAX);
    g_assert_false(qtest_get_irq(s, 0));
    step(16000);
    g_assert_cmpuint(rd(CH(7, 16)), ==, 6 << 4);
    wr(CH(7, 12), BIT(31) | (2U << 20) | (1U << 10) | 1);
    wr(CH(7, 8), 6 << 4);
    wr(CH(7, 0), 3 | BIT(2) | BIT(4));
    step(32000);
    g_assert_cmpuint(rd(CH(7, 16)), ==, 4 << 4);
    g_assert_cmphex(rd(0xc0) & BIT(11), ==, BIT(11));
    qtest_quit(s);
}

static void test_fractional_carry(void)
{
    setup(0, 0, 0, 2);
    unsigned div15 = 384U << 4;
    unsigned div25 = 640U << 4;
    wr(TCONF(0), div15 | 4 | RESET | UPDATE);
    wr(CH(0, 8), (2U << 4) | 8); /* 2/3 timer ticks alternating */
    wr(CH(0, 0), BIT(2) | BIT(4));
    wr(TCONF(0), div15 | 4);
    step(74); sample(true);
    step(1); sample(false); /* two ref pulses cost 3 XTAL clocks */
    step(525); sample(true);
    step(99); sample(true);
    step(1); sample(false); /* three ref pulses cost 4 XTAL clocks */
    wr(TCONF(0), div25 | 4 | UPDATE);
    step(500); sample(true); /* shadow divider changes at old wrap */
    g_assert_cmpuint(rd(TVALUE(0)), ==, 0);
    step(124); sample(true);
    step(1); sample(false); /* two new ref pulses cost 5 XTAL clocks */
    step(874); sample(false);
    step(1); sample(true); /* new period = 40 XTAL clocks */
    qtest_quit(s);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/esp32s3/ledc/channels-timers-physical", test_channels);
    qtest_add_func("/esp32s3/ledc/shadow-pause-gate-idle-reset", test_shadow_pause_gate);
    qtest_add_func("/esp32s3/ledc/fade-overflow-irq", test_fade_overflow_irq);
    qtest_add_func("/esp32s3/ledc/fractional-divider-duty-carry", test_fractional_carry);
    return g_test_run();
}
