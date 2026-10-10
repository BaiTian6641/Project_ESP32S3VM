/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SOC16: named functional profile qualification, NOT silicon qualification.
 * Observe powered ProjectDocument nets only. No GPIO/count/ADC injection.
 * Independent oracle integrates the transfer residual twice; it does not
 * reproduce the device's delayed-error implementation. IDF allocation and
 * enable/disable software FSM tests belong to the ordinary firmware fixture.
 */
#include "qemu/osdep.h"
#include <math.h>
#include "qemu/bitops.h"
#include "qapi/error.h"
#include "qapi/qmp/qjson.h"
#include "qapi/qmp/qlist.h"
#include "qapi/qmp/qnum.h"
#include "libqtest.h"
#include "hw/gpio/esp32s3_gpio.h"
#include "hw/gpio/esp32s3_iomux.h"

#define PROFILE "s3-sdm-second-order-transfer-v1"
#define BASE 0x60004f00ULL
#define GPIO(r) (0x60004000ULL + (r))
#define SYSCLK 0x600c0060ULL
#define FUNCTION BIT(30)
#define DIVIDER 39U /* XTAL/APB 40 MHz / 40 = one pulse per microsecond */
static QTestState *s;

typedef struct Reference {
    int density;
    int64_t discrepancy, integral;
    bool level;
} Reference;

/* With e[-1]=e[-2]=0, the public transfer equation gives
 * D[n]=sum(y-x)=e[n]-e[n-1], A[n]=sum D=e[n].
 * The tie-high quantizer is therefore high iff A[n-1]+D[n-1]<=x.
 * Arbitrary precision is used by the separate Python reference; these small
 * finite qtest trajectories fit int64_t, independent of device state widths.
 */
static bool next(Reference *r)
{
    r->level = r->integral + r->discrepancy <= r->density;
    r->discrepancy += (r->level ? 128 : -128) - r->density;
    r->integral += r->discrepancy;
    return r->level;
}

static void wr(unsigned r, uint32_t v) { qtest_writel(s, BASE + r, v); }
static uint32_t rd(unsigned r) { return qtest_readl(s, BASE + r); }
static void step(uint64_t ns) { qtest_clock_step(s, ns); }
static void configure(unsigned ch, int density, unsigned divider)
{
    wr(ch * 4, divider << 8 | (uint8_t)density);
}

static char *property(const char *path, const char *name)
{
    QDict *r = qtest_qmp(s, "{'execute':'qom-get','arguments':{'path':%s,'property':%s}}", path, name);
    g_assert_false(qdict_haskey(r, "error"));
    char *value = g_strdup(qdict_get_str(r, "return"));
    qobject_unref(r);
    return value;
}

static QDict *snapshot(void)
{
    g_autofree char *json = property("/machine/soc/electrical", "snapshot-json");
    return qobject_to(QDict, qobject_from_json(json, &error_abort));
}

static double voltage(QDict *snap, unsigned gpio)
{
    const QListEntry *entry;
    QLIST_FOREACH_ENTRY(qdict_get_qlist(snap, "pads"), entry) {
        QDict *pad = qobject_to(QDict, qlist_entry_obj(entry));
        if (qdict_get_int(pad, "gpio") == gpio) {
            g_assert_true(qdict_get_bool(pad, "valid"));
            QNum *v = qobject_to(QNum, qdict_get(pad, "voltage_v"));
            g_assert_nonnull(v);
            return qnum_get_double(v);
        }
    }
    g_assert_not_reached();
}

static void sample(unsigned gpio, bool high)
{
    g_assert_cmphex(qtest_readl(s, GPIO(A_GPIO_IN)) & BIT(gpio), ==,
                    high ? BIT(gpio) : 0);
    QDict *snap = snapshot();
    double v = voltage(snap, gpio);
    g_assert_cmpfloat_with_epsilon(v, high ? 3.3 : 0, .001);
    qobject_unref(snap);
}

/* Public v3 graph shape and RC quantities match electrical-runtime qtests. */
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

static void quantity(QDict *c, const char *name, double value, const char *unit)
{
    QDict *q = qdict_new();
    qdict_put(q, "value", qnum_from_double(value));
    qdict_put_str(q, "unit", unit);
    qdict_put(qdict_get_qdict(c, "parameters"), name, q);
}

static void net(QDict *p, const char *id, const char *const *ends)
{
    QDict *n = qdict_new();
    QList *e = qlist_new();
    qdict_put_str(n, "id", id);
    qdict_put_str(n, "name", id);
    for (; *ends; ends++) {
        qlist_append_str(e, *ends);
    }
    qdict_put(n, "endpoints", e);
    qlist_append(qdict_get_qlist(p, "nets"), n);
}
#define NET(p, id, ...) net(p, id, (const char *const[]){ __VA_ARGS__, NULL })

static void graph(bool parallel, bool rc)
{
    QDict *p = qobject_to(QDict, qobject_from_json(
        "{\"version\":3,\"id\":\"sdm-native\",\"name\":\"SDM physical nets\","
        "\"profile\":{\"chip\":\"esp32s3\",\"board\":\"esp32-s3-devkitc-1\",\"module\":\"esp32-s3-wroom-1\"},"
        "\"firmware\":{},\"runtime\":{\"electrical\":{\"driver_profile\":\"s3-explicit-finite-v1\",\"mode\":\"dc\"}},"
        "\"geometry\":{\"components\":{},\"nets\":{}},\"components\":[],\"nets\":[]}", &error_abort));
    QDict *c = component(p, "U", "mcu");
    terminal(c, "U.vdd", "vdd", "power", -1);
    terminal(c, "U.gnd", "gnd", "ground", -1);
    for (unsigned pad = 4; pad < (parallel ? 20 : 6); pad++) {
        g_autofree char *id = g_strdup_printf("U.io%u", pad);
        g_autofree char *role = g_strdup_printf("io%u", pad);
        terminal(c, id, role, "digital", pad);
    }
    c = component(p, "G", "ground");
    terminal(c, "G.ref", "ref", "ground", -1);
    c = component(p, "V", "voltage-source");
    terminal(c, "V.p", "p", "power", -1);
    terminal(c, "V.n", "n", "ground", -1);
    quantity(c, "voltage", 3.3, "V");
    NET(p, "vdd", "V.p", "U.vdd");
    if (rc) {
        QDict *e = qdict_get_qdict(qdict_get_qdict(p, "runtime"), "electrical");
        qdict_put_str(e, "mode", "rc");
        qdict_put_str(e, "edit_charge", "reset");
        c = component(p, "R", "resistor");
        terminal(c, "R.a", "a", "passive", -1);
        terminal(c, "R.b", "b", "passive", -1);
        quantity(c, "resistance", 1000, "ohm");
        c = component(p, "C", "capacitor");
        terminal(c, "C.p", "p", "passive", -1);
        terminal(c, "C.n", "n", "passive", -1);
        quantity(c, "capacitance", 1, "nF");
        quantity(c, "initial_voltage", 0, "V");
        NET(p, "gnd", "G.ref", "V.n", "U.gnd", "C.n");
        NET(p, "pulse", "U.io4", "R.a");
        NET(p, "filtered", "R.b", "C.p", "U.io5");
    } else {
        NET(p, "gnd", "G.ref", "V.n", "U.gnd");
        for (unsigned n = 0; n < (parallel ? 8 : 1); n++) {
            g_autofree char *out = g_strdup_printf("U.io%u", 4 + n);
            g_autofree char *in = g_strdup_printf("U.io%u", parallel ? 12 + n : 5);
            g_autofree char *id = g_strdup_printf("pulse%u", n);
            NET(p, id, out, in);
        }
    }
    GString *json = qobject_to_json(QOBJECT(p));
    QDict *reply = qtest_qmp(s, "{'execute':'qom-set','arguments':{'path':'/machine/soc/electrical','property':'project-json','value':%s}}", json->str);
    g_assert_false(qdict_haskey(reply, "error"));
    qobject_unref(reply);
    reply = qtest_qmp(s, "{'execute':'cont'}");
    g_assert_false(qdict_haskey(reply, "error"));
    qobject_unref(reply);
    g_string_free(json, true);
    qobject_unref(p);
}

static void route(unsigned pad, unsigned channel, bool invert)
{
    qtest_writel(s, GPIO(GPIO_FUNC_OUT_SEL_CFG_OFFSET(pad)),
                 (93 + channel) | (invert ? BIT(9) : 0));
}

static void start(bool parallel, bool rc)
{
    s = qtest_initf("-machine esp32s3 -S -L pc-bios -global driver=esp32s3.gpio,property=strap_mode,value=0x00");
    g_autofree char *profile = property("/machine/soc/sdm", "waveform-profile");
    g_autofree char *assumptions = property("/machine/soc/sdm", "waveform-assumptions");
    g_assert_cmpstr(profile, ==, PROFILE);
    g_assert_nonnull(strstr(assumptions, "NOT silicon-bit-exact"));
    g_assert_nonnull(strstr(assumptions, "tie-high"));
    g_assert_nonnull(strstr(assumptions, "density-change"));
    qtest_writel(s, SYSCLK, 0x28000); /* XTAL / (0 + 1), APB 40 MHz */
    unsigned mux = R_IO_MUX_GPIOn_FUN_IE_MASK |
        (ESP32S3_IOMUX_MCU_SEL_GPIO << R_IO_MUX_GPIOn_MCU_SEL_SHIFT);
    for (unsigned pad = 4; pad < (parallel ? 20 : 6); pad++) {
        qtest_writel(s, 0x60009000ULL + IO_MUX_GPIOn_REG_OFFSET(pad), mux);
    }
    graph(parallel, rc);
    for (unsigned ch = 0; ch < 8; ch++) {
        g_assert_cmphex(rd(ch * 4), ==, 0xff00);
    }
    g_assert_cmphex(rd(0x20), ==, 0);
    g_assert_cmphex(rd(0x24), ==, 0);
    g_assert_cmphex(rd(0x28), ==, 0x01802260);
}

static void test_channels(void)
{
    static const int densities[] = { 0, -128, 127 };
    for (unsigned ch = 0; ch < 8; ch++) {
        for (unsigned d = 0; d < G_N_ELEMENTS(densities); d++) {
            for (unsigned invert = 0; invert < 2; invert++) {
                start(false, false);
                route(4, ch, invert);
                configure(ch, densities[d], DIVIDER);
                Reference r = { .density = densities[d] };
                sample(5, invert);
                wr(0x24, FUNCTION);
                unsigned high = 0;
                for (unsigned n = 0; n < 512; n++) {
                    step(999); sample(5, r.level ^ invert);
                    step(1); high += next(&r); sample(5, r.level ^ invert);
                }
                g_test_message("SDM_EVIDENCE profile=%s ch=%u density=%d invert=%u ticks=512 high=%u", PROFILE, ch, densities[d], invert, high);
                qtest_quit(s);
            }
        }
    }
}

static void test_simultaneous(void)
{
    static const int density[] = { -128, 127, 0, -96, -32, 32, 64, 96 };
    Reference r[8] = { 0 };
    start(true, false);
    for (unsigned ch = 0; ch < 8; ch++) {
        r[ch].density = density[ch];
        route(4 + ch, ch, ch & 1);
        configure(ch, density[ch], DIVIDER);
    }
    wr(0x24, FUNCTION);
    for (unsigned n = 0; n < 512; n++) {
        step(1000);
        for (unsigned ch = 0; ch < 8; ch++) {
            sample(12 + ch, next(&r[ch]) ^ (ch & 1));
        }
    }
    g_test_message("SDM_EVIDENCE simultaneous_channels=8 ticks=512 powered_separate_nets=8");
    qtest_quit(s);
}

static void test_reconfiguration(void)
{
    start(false, false);
    route(4, 0, false);
    Reference r = { .density = 0 };
    configure(0, 0, DIVIDER);
    wr(0x24, FUNCTION);
    step(1000); sample(5, next(&r));
    step(250); /* quarter cycle saved across function-clock stop */
    wr(0x24, 0);
    step(123456); sample(5, r.level);
    wr(0x24, FUNCTION);
    step(749); sample(5, r.level);
    step(1); sample(5, next(&r));
    /* CG31 is register autogate override, not waveform enable. */
    wr(0x20, BIT(31));
    step(1000); sample(5, next(&r));
    wr(0x20, 0);
    step(250);
    configure(0, 0, 79); /* quarter phase retained: 1500 ns remain */
    step(1499); sample(5, r.level);
    step(1); sample(5, next(&r));
    /* The following tick also checks the unchanged divider trajectory. */
    step(2000); sample(5, next(&r));
    step(500); /* quarter phase at divider 80 */
    qtest_writel(s, SYSCLK, 0x28001); /* APB becomes XTAL/2 */
    step(2999); sample(5, r.level);
    step(1); sample(5, next(&r));
    qtest_writel(s, SYSCLK, 0x28000);
    configure(0, 127, DIVIDER);
    r = (Reference){ .density = 127, .level = r.level };
    for (unsigned n = 0; n < 64; n++) {
        step(1000); sample(5, next(&r));
    }
    /* CPU-only reset retains peripheral registers, state and pulse phase. */
    step(250);
    qtest_writel(s, 0x60008000ULL, BIT(5));
    step(0);
    g_assert_cmphex(rd(0), ==, (DIVIDER << 8) | 127);
    step(749); sample(5, r.level);
    step(1); sample(5, next(&r));
    /* Real shared peripheral reset, not an invented dedicated SDM bit. */
    QDict *reply = qtest_qmp(s, "{'execute':'system_reset'}");
    g_assert_false(qdict_haskey(reply, "error"));
    qobject_unref(reply);
    step(0);
    for (unsigned ch = 0; ch < 8; ch++) {
        g_assert_cmphex(rd(ch * 4), ==, 0xff00);
    }
    g_assert_cmphex(rd(0x24), ==, 0);
    g_assert_cmphex(rd(0x20), ==, 0);
    /* Re-route after GPIO reset; output-low reset state remains a real drive. */
    unsigned mux = R_IO_MUX_GPIOn_FUN_IE_MASK |
        (ESP32S3_IOMUX_MCU_SEL_GPIO << R_IO_MUX_GPIOn_MCU_SEL_SHIFT);
    for (unsigned pad = 4; pad <= 5; pad++) {
        qtest_writel(s, 0x60009000ULL + IO_MUX_GPIOn_REG_OFFSET(pad), mux);
    }
    route(4, 0, false);
    sample(5, false);
    g_test_message("SDM_EVIDENCE gate_phase=retained divider_phase=retained source_phase=retained density_errors=zero cpu_reset=retained system_reset=zero");
    qtest_quit(s);
}

static void test_rc(void)
{
    start(false, true);
    route(4, 0, false);
    configure(0, 0, DIVIDER);
    Reference r = { .density = 0 };
    wr(0x24, FUNCTION);
    double expected = 0, low = 3.3, high = 0, max_error = 0;
    double window_initial = 0, window_final = 0;
    unsigned window_high_ticks = 0;
    int64_t epoch = qtest_clock_step(s, 0);
    /* Explicit finite driver R=40 ohm, R=1 kohm, C=1 nF.
     * snapshot-json is a committed solver frame, not an implicit live sample.
     * Compare the independent exponential trajectory at its actual timestamp.
     * Integrate the physical RC mean by KCL over the complete steady window:
     * integral(Vc dt) = integral(Vsource dt) - RC * (Vc_end - Vc_start).
     * Neither pad voltage nor the SDM raw pulse source is averaged/replaced.
     */
    const double tau = 1040 * 1e-9;
    const double decay = exp(-1e-6 / tau);
    for (unsigned n = 0; n < 512; n++) {
        double interval_initial = expected;
        double target = r.level ? 3.3 : 0;
        if (n >= 256 && r.level) {
            window_high_ticks++;
        }
        expected = target + (expected - target) * decay;
        step(1000);
        next(&r);
        QDict *snap = snapshot();
        int64_t sampled_ns = g_ascii_strtoll(qdict_get_str(snap, "timestamp_ns"), NULL, 10);
        int64_t interval_ns = epoch + n * 1000;
        g_assert_cmpint(sampled_ns, >=, interval_ns);
        g_assert_cmpint(sampled_ns, <=, interval_ns + 1000);
        double sampled_expected = target + (interval_initial - target) *
                                  exp(-(sampled_ns - interval_ns) * 1e-9 / tau);
        double actual = voltage(snap, 5);
        bool source_high = sampled_ns == interval_ns + 1000 ? r.level : target != 0;
        double source_v = voltage(snap, 4);
        g_assert_true(source_high ? source_v > 2.4 : source_v < .8);
        max_error = MAX(max_error, fabs(actual - sampled_expected));
        g_test_message("SDM_RC_SAMPLE tick=%u requested_ns=%" PRId64 " sampled_ns=%" PRId64
                       " voltage_v=%.9f expected_v=%.9f source_high=%u",
                       n + 1, interval_ns + 1000, sampled_ns, actual, sampled_expected, source_high);
        g_assert_cmpfloat_with_epsilon(actual, sampled_expected, .015);
        if (n == 255) {
            g_assert_cmpint(sampled_ns, ==, interval_ns + 1000);
            window_initial = actual;
        }
        if (n == 511) {
            g_assert_cmpint(sampled_ns, ==, interval_ns + 1000);
        }
        if (n >= 256) {
            window_final = actual;
            low = MIN(low, actual);
            high = MAX(high, actual);
        }
        qobject_unref(snap);
    }
    double mean = 3.3 * window_high_ticks / 256 -
                  tau * (window_final - window_initial) / (256e-6);
    g_assert_cmpfloat_with_epsilon(mean, 1.65, .015);
    g_assert_cmpfloat(high - low, >, .1); /* pulse-resolved ripple, not DC mean */
    wr(0x24, 0);
    QDict *before = snapshot();
    double charged = voltage(before, 5);
    g_autofree char *generation = g_strdup(qdict_get_str(before, "generation"));
    qobject_unref(before);
    QDict *reply = qtest_qmp(s, "{'execute':'system_reset'}");
    g_assert_false(qdict_haskey(reply, "error"));
    qobject_unref(reply);
    step(0);
    QDict *after = snapshot();
    g_assert_cmpstr(qdict_get_str(after, "generation"), ==, generation);
    g_assert_cmpfloat_with_epsilon(voltage(after, 5), charged, .000001);
    qobject_unref(after);
    g_test_message("SDM_EVIDENCE rc_mean_v=%.6f rc_ripple_v=%.6f max_exp_error_v=%.6f charge_reset=retained mean_method=physical_KCL_complete_window", mean, high - low, max_error);
    qtest_quit(s);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/esp32s3/sdm/all8-density-extremes-inversion-cadence", test_channels);
    qtest_add_func("/esp32s3/sdm/simultaneous-powered-nets", test_simultaneous);
    qtest_add_func("/esp32s3/sdm/gates-source-divider-density-reset", test_reconfiguration);
    qtest_add_func("/esp32s3/sdm/passive-rc-physical-mean-ripple-charge", test_rc);
    return g_test_run();
}
