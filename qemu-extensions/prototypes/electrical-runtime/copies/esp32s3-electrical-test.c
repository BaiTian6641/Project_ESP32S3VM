/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include <math.h>
#include "qapi/error.h"
#include "qemu/bitops.h"
#include "libqtest.h"
#include "qapi/qmp/qjson.h"
#include "qapi/qmp/qnum.h"
#include "qapi/qmp/qlist.h"
#include "hw/gpio/esp32s3_gpio.h"
#include "hw/gpio/esp32s3_iomux.h"

#define PATH "/machine/soc/electrical"
#define GPIO(x) (0x60004000ULL + (x))
#define PAD(n) (0x60009000ULL + IO_MUX_GPIOn_REG_OFFSET(n))
#define INPUT (R_IO_MUX_GPIOn_FUN_IE_MASK | (ESP32S3_IOMUX_MCU_SEL_GPIO << R_IO_MUX_GPIOn_MCU_SEL_SHIFT))
static QTestState *s;

static QDict *object(const char *json)
{
    QObject *o = qobject_from_json(json, &error_abort);
    g_assert_nonnull(qobject_to(QDict, o));
    return qobject_to(QDict, o);
}

static QDict *component(QDict *p, const char *id)
{
    const QListEntry *e;
    QLIST_FOREACH_ENTRY(qdict_get_qlist(p, "components"), e) {
        QDict *c = qobject_to(QDict, qlist_entry_obj(e));
        if (!strcmp(qdict_get_str(c, "id"), id)) {
            return c;
        }
    }
    g_assert_not_reached();
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

static QDict *add_component(QDict *p, const char *id, const char *kind)
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

static void quantity(QDict *c, const char *name, double value, const char *unit)
{
    QDict *q = qdict_new();
    qdict_put(q, "value", qnum_from_double(value));
    qdict_put_str(q, "unit", unit);
    qdict_put(qdict_get_qdict(c, "parameters"), name, q);
}

static void net(QDict *p, const char *id, const char *const *ids)
{
    const QListEntry *e;
    QLIST_FOREACH_ENTRY(qdict_get_qlist(p, "nets"), e) {
        QDict *existing = qobject_to(QDict, qlist_entry_obj(e));
        if (!strcmp(qdict_get_str(existing, "id"), id)) {
            for (; *ids; ids++) {
                qlist_append_str(qdict_get_qlist(existing, "endpoints"), *ids);
            }
            return;
        }
    }
    QDict *n = qdict_new();
    QList *ends = qlist_new();
    qdict_put_str(n, "id", id);
    qdict_put_str(n, "name", id);
    for (; *ids; ids++) {
        qlist_append_str(ends, *ids);
    }
    qdict_put(n, "endpoints", ends);
    qlist_append(qdict_get_qlist(p, "nets"), n);
}
#define NET(p, id, ...) net(p, id, (const char *const[]){__VA_ARGS__, NULL})

/* Complete public ProjectDocument v3 shape; no test-only graph properties. */
static QDict *project(void)
{
    QDict *p = object("{\"version\":3,\"id\":\"native-fixture\",\"name\":\"Native fixture\","
        "\"profile\":{\"chip\":\"esp32s3\",\"board\":\"esp32-s3-devkitc-1\",\"module\":\"esp32-s3-wroom-1\"},"
        "\"firmware\":{},\"runtime\":{\"electrical\":{\"driver_profile\":\"s3-explicit-finite-v1\",\"mode\":\"dc\"}},"
        "\"geometry\":{\"components\":{},\"nets\":{}},\"components\":[],\"nets\":[]}");
    QDict *c = add_component(p, "U1", "mcu");
    terminal(c, "U1.vdd", "vdd", "power", -1);
    terminal(c, "U1.gnd", "gnd", "ground", -1);
    terminal(c, "U1.io1", "io1", "analog", 1);
    terminal(c, "U1.io4", "io4", "digital", 4);
    terminal(c, "U1.io5", "io5", "digital", 5);
    /* Deliberately matching legacy address/device metadata cannot wire pads. */
    qdict_put_int(c, "address", 0x20);
    qdict_put_str(c, "device", "gpio");
    c = add_component(p, "G", "ground");
    terminal(c, "G.ref", "ref", "ground", -1);
    c = add_component(p, "V", "voltage-source");
    terminal(c, "V.p", "p", "power", -1);
    terminal(c, "V.n", "n", "ground", -1);
    quantity(c, "voltage", 3.3, "V");
    NET(p, "gnd", "G.ref", "V.n", "U1.gnd");
    NET(p, "vdd", "V.p", "U1.vdd");
    return p;
}

static void resistor(QDict *p, const char *id, double ohm)
{
    QDict *c = add_component(p, id, "resistor");
    g_autofree char *a = g_strdup_printf("%s.a", id);
    g_autofree char *b = g_strdup_printf("%s.b", id);
    terminal(c, a, "a", "passive", -1);
    terminal(c, b, "b", "passive", -1);
    quantity(c, "resistance", ohm, "ohm");
}

static QDict *apply_reply(QDict *p)
{
    GString *json = qobject_to_json(QOBJECT(p));
    QDict *r = qtest_qmp(s, "{'execute':'qom-set','arguments':{'path':%s,'property':'project-json','value':%s}}",
                         PATH, json->str);
    g_string_free(json, true);
    return r;
}

static void apply(QDict *p)
{
    QDict *r = apply_reply(p);
    g_assert_false(qdict_haskey(r, "error"));
    qobject_unref(r);
}

static char *get_string(const char *property)
{
    QDict *r = qtest_qmp(s, "{'execute':'qom-get','arguments':{'path':%s,'property':%s}}", PATH, property);
    g_assert_false(qdict_haskey(r, "error"));
    char *result = g_strdup(qdict_get_str(r, "return"));
    qobject_unref(r);
    return result;
}

static QDict *snapshot(void)
{
    g_autofree char *json = get_string("snapshot-json");
    QDict *result = object(json);
    g_assert_cmpint(qdict_get_int(result, "abi"), ==, 1);
    g_assert_nonnull(qdict_get_try_str(result, "generation"));
    g_assert_nonnull(qdict_get_try_str(result, "source_generation"));
    g_assert_nonnull(qdict_get_try_str(result, "timestamp_ns"));
    return result;
}

static uint64_t counter(QDict *d, const char *key)
{
    const char *text = qdict_get_str(d, key);
    char *end;
    g_assert_true(g_ascii_isdigit(*text));
    errno = 0;
    uint64_t value = g_ascii_strtoull(text, &end, 10);
    g_assert_cmpint(errno, ==, 0);
    g_assert_cmpstr(end, ==, "");
    return value;
}

static QDict *pad(QDict *snap, unsigned gpio)
{
    const QListEntry *e;
    QLIST_FOREACH_ENTRY(qdict_get_qlist(snap, "pads"), e) {
        QDict *p = qobject_to(QDict, qlist_entry_obj(e));
        if (qdict_get_int(p, "gpio") == gpio) {
            return p;
        }
    }
    g_assert_not_reached();
}

static QDict *physical_pad(QDict *snap, unsigned gpio)
{
    const QListEntry *entry;
    QLIST_FOREACH_ENTRY(qdict_get_qlist(snap, "physical_pads"), entry) {
        QDict *p = qobject_to(QDict, qlist_entry_obj(entry));
        if (qdict_get_int(p, "gpio") == gpio) {
            return p;
        }
    }
    g_assert_not_reached();
}

static double number(QDict *d, const char *key)
{
    QNum *n = qobject_to(QNum, qdict_get(d, key));
    g_assert_nonnull(n);
    return qnum_get_double(n);
}

static void voltage(QDict *snap, unsigned gpio, double expected)
{
    QDict *p = pad(snap, gpio);
    g_assert_true(qdict_get_bool(p, "valid"));
    g_assert_cmpfloat_with_epsilon(number(p, "voltage_v"), expected, 1e-7);
}

static void start(void)
{
    s = qtest_initf("-machine esp32s3 -S -L pc-bios -global driver=esp32s3.gpio,property=strap_mode,value=0x00");
}

static void set_running(bool running)
{
    QDict *r = qtest_qmp(s, "{'execute':%s}", running ? "cont" : "stop");
    g_assert_false(qdict_haskey(r, "error"));
    qobject_unref(r);
    r = qtest_qmp(s, "{'execute':'query-status'}");
    g_assert_cmpint(qdict_get_bool(qdict_get_qdict(r, "return"), "running"), ==, running);
    qobject_unref(r);
}

static void test_divider_irq(void)
{
    start();
    QDict *p = project();
    resistor(p, "Rtop", 10000);
    resistor(p, "Rlow", 10000);
    NET(p, "vdd", "Rtop.a");
    NET(p, "gnd", "Rlow.b");
    NET(p, "mid", "Rtop.b", "Rlow.a", "U1.io1");
    NET(p, "button", "U1.io4", "U1.io5");
    qtest_writel(s, PAD(4), INPUT);
    qtest_writel(s, PAD(5), INPUT);
    qtest_writel(s, GPIO(A_GPIO_ENABLE_W1TS), BIT(4));
    qtest_writel(s, GPIO(GPIO_PINn_REG_OFFSET(5)),
        (ESP32S3_GPIO_INT_RISING << R_GPIO_PINn_INT_TYPE_SHIFT) | ESP32S3_GPIO_INT_ENA_PROCPU);
    apply(p);
    QDict *snap = snapshot();
    voltage(snap, 1, 1.65);
    qobject_unref(snap);
    qtest_writel(s, GPIO(A_GPIO_OUT_W1TS), BIT(4));
    g_assert_cmphex(qtest_readl(s, GPIO(A_GPIO_IN)) & BIT(5), ==, BIT(5));
    g_assert_cmphex(qtest_readl(s, GPIO(A_GPIO_PCPU_INT)) & BIT(5), ==, BIT(5));
    snap = snapshot();
    voltage(snap, 5, 3.3);
    qobject_unref(snap);
    qobject_unref(p);
    qtest_quit(s);
}

static void test_disconnect_open_drain(void)
{
    start();
    QDict *p = project();
    NET(p, "out", "U1.io4");
    NET(p, "in", "U1.io5");
    qtest_writel(s, PAD(4), INPUT);
    qtest_writel(s, GPIO(A_GPIO_ENABLE_W1TS), BIT(4));
    qtest_writel(s, GPIO(A_GPIO_OUT_W1TS), BIT(4));
    apply(p);
    QDict *snap = snapshot();
    voltage(snap, 4, 3.3);
    QDict *in = pad(snap, 5);
    g_assert_false(qdict_get_bool(in, "valid"));
    g_assert_false(qdict_get_bool(in, "digital_valid"));
    g_assert_true(qdict_get_bool(in, "floating"));
    g_assert_cmpint(qobject_type(qdict_get(in, "voltage_v")), ==, QTYPE_QNULL);
    qobject_unref(snap);
    /* GPIO2 is deliberately absent from v3. Real enabled firmware pulls still
     * solve its private physical pad, without inventing graph endpoints. */
    qtest_writel(s, PAD(2), INPUT | R_IO_MUX_GPIOn_FUN_PU_MASK |
                 R_IO_MUX_GPIOn_SLP_SEL_MASK);
    uint32_t inputs = qtest_readl(s, GPIO(A_GPIO_IN));
    g_assert_cmphex(inputs & BIT(2), ==, BIT(2));
    g_assert_cmphex(inputs & (BIT(6) | (0xfU << 22)), ==, 0);
    snap = snapshot();
    g_assert_cmpuint(qlist_size(qdict_get_qlist(snap, "pads")), ==, 3);
    g_assert_cmpuint(qlist_size(qdict_get_qlist(snap, "physical_pads")), ==, 45);
    QDict *physical = physical_pad(snap, 2);
    g_assert_false(qdict_haskey(physical, "terminal_id"));
    g_assert_true(qdict_get_bool(physical, "valid"));
    g_assert_true(qdict_get_bool(physical, "digital_valid"));
    g_assert_false(qdict_get_bool(physical, "floating"));
    g_assert_cmpfloat_with_epsilon(number(physical, "voltage_v"), 3.3, 1e-7);
    QDict *controls = qdict_get_qdict(physical, "native_controls");
    g_assert_true(qdict_get_bool(controls, "receiver_enabled"));
    g_assert_true(qdict_get_bool(controls, "pull_up"));
    g_assert_true(qdict_get_bool(controls, "sleep_configured"));
    g_assert_false(qdict_get_bool(controls, "sleep_requested"));
    QDict *support = qdict_get_qdict(snap, "support");
    g_assert_true(qdict_get_bool(support, "awake_sleep_configuration"));
    g_assert_false(qdict_get_bool(support, "active_sleep"));
    g_assert_false(qdict_get_bool(support, "hold"));
    voltage(snap, 4, 3.3);
    const QListEntry *entry;
    unsigned native_branches = 0;
    QLIST_FOREACH_ENTRY(qdict_get_qlist(snap, "currents"), entry) {
        QDict *branch = qobject_to(QDict, qlist_entry_obj(entry));
        if (!strcmp(qdict_get_str(branch, "component_id"), "native-firmware-drive/pull")) {
            /* The explicit45k pull has I=0, hence V(GPIO2)=VDD=3.3V. */
            g_assert_cmpfloat_with_epsilon(number(branch, "current_a"), 0, 1e-10);
            ++native_branches;
        }
    }
    g_assert_cmpuint(native_branches, ==, 2);
    qobject_unref(snap);
    qtest_writel(s, PAD(2), R_IO_MUX_GPIOn_FUN_PU_MASK |
                 (ESP32S3_IOMUX_MCU_SEL_GPIO << R_IO_MUX_GPIOn_MCU_SEL_SHIFT));
    g_assert_cmphex(qtest_readl(s, GPIO(A_GPIO_IN)) & BIT(2), ==, 0);
    qtest_writel(s, PAD(2), 0);
    qtest_writel(s, GPIO(GPIO_PINn_REG_OFFSET(4)), R_GPIO_PINn_PAD_DRIVER_MASK);
    qtest_writel(s, PAD(4), INPUT | R_IO_MUX_GPIOn_FUN_PU_MASK);
    snap = snapshot();
    voltage(snap, 4, 3.3);
    qobject_unref(snap);
    qtest_writel(s, GPIO(A_GPIO_OUT_W1TC), BIT(4));
    snap = snapshot();
    voltage(snap, 4, 3.3 * 40 / (45000 + 40));
    qobject_unref(snap);
    qtest_writel(s, GPIO(A_GPIO_OUT_W1TS), BIT(4));
    qtest_writel(s, PAD(4), INPUT);
    snap = snapshot();
    g_assert_false(qdict_get_bool(pad(snap, 4), "valid"));
    g_assert_true(qdict_get_bool(pad(snap, 4), "floating"));
    qobject_unref(snap);
    /* Disable the declared floating pad's input buffer, isolating the next
     * failure to the genuinely enabled, omitted physical GPIO2 consumer. */
    qtest_writel(s, PAD(4), ESP32S3_IOMUX_MCU_SEL_GPIO << R_IO_MUX_GPIOn_MCU_SEL_SHIFT);
    qtest_writel(s, PAD(2), INPUT | R_IO_MUX_GPIOn_FUN_PU_MASK);
    QDict *run = qtest_qmp(s, "{'execute':'cont'}");
    g_assert_false(qdict_haskey(run, "error"));
    qobject_unref(run);
    run = qtest_qmp(s, "{'execute':'query-status'}");
    g_assert_true(qdict_get_bool(qdict_get_qdict(run, "return"), "running"));
    qobject_unref(run);
    g_assert_cmphex(qtest_readl(s, GPIO(A_GPIO_IN)) & BIT(2), ==, BIT(2));
    qtest_writel(s, PAD(2), INPUT);
    qtest_readl(s, GPIO(A_GPIO_IN));
    run = qtest_qmp(s, "{'execute':'query-status'}");
    g_assert_false(qdict_get_bool(qdict_get_qdict(run, "return"), "running"));
    g_assert_cmpstr(qdict_get_str(qdict_get_qdict(run, "return"), "status"), ==, "paused");
    qobject_unref(run);
    snap = snapshot();
    g_assert_cmpuint(qlist_size(qdict_get_qlist(snap, "physical_pads")), ==, 45);
    physical = physical_pad(snap, 2);
    g_assert_false(qdict_haskey(physical, "terminal_id"));
    g_assert_false(qdict_get_bool(physical, "valid"));
    g_assert_false(qdict_get_bool(physical, "digital_valid"));
    g_assert_true(qdict_get_bool(physical, "floating"));
    g_assert_cmpint(qobject_type(qdict_get(physical, "voltage_v")), ==, QTYPE_QNULL);
    controls = qdict_get_qdict(physical, "native_controls");
    g_assert_true(qdict_get_bool(controls, "receiver_enabled"));
    g_assert_false(qdict_get_bool(controls, "pull_up"));
    qobject_unref(snap);
    /* SLP_SEL is configuration while awake, but actual RTC SLEEP_EN is an
     * explicitly unsupported consumer mode. Repair must not auto-resume. */
    qtest_writel(s, PAD(2), INPUT | R_IO_MUX_GPIOn_FUN_PU_MASK |
                 R_IO_MUX_GPIOn_SLP_SEL_MASK);
    apply(p);
    set_running(true);
    g_assert_cmphex(qtest_readl(s, GPIO(A_GPIO_IN)) & BIT(2), ==, BIT(2));
    qtest_writel(s, 0x60008018, BIT(31));
    run = qtest_qmp(s, "{'execute':'query-status'}");
    g_assert_false(qdict_get_bool(qdict_get_qdict(run, "return"), "running"));
    g_assert_cmpstr(qdict_get_str(qdict_get_qdict(run, "return"), "status"), ==, "paused");
    qobject_unref(run);
    snap = snapshot();
    g_assert_cmpstr(qdict_get_str(snap, "status"), ==, "failed");
    g_assert_nonnull(strstr(qdict_get_str(snap, "diagnostic"), "sleep"));
    controls = qdict_get_qdict(physical_pad(snap, 2), "native_controls");
    g_assert_true(qdict_get_bool(controls, "sleep_requested"));
    qobject_unref(snap);
    qtest_writel(s, 0x60008018, 0);
    apply(p);
    snap = snapshot();
    physical = physical_pad(snap, 2);
    g_assert_true(qdict_get_bool(physical, "valid"));
    g_assert_true(qdict_get_bool(physical, "digital_valid"));
    g_assert_cmpfloat_with_epsilon(number(physical, "voltage_v"), 3.3, 1e-7);
    controls = qdict_get_qdict(physical, "native_controls");
    g_assert_true(qdict_get_bool(controls, "sleep_configured"));
    g_assert_false(qdict_get_bool(controls, "sleep_requested"));
    qobject_unref(snap);
    run = qtest_qmp(s, "{'execute':'query-status'}");
    g_assert_false(qdict_get_bool(qdict_get_qdict(run, "return"), "running"));
    g_assert_cmpstr(qdict_get_str(qdict_get_qdict(run, "return"), "status"), ==, "paused");
    qobject_unref(run);
    qobject_unref(p);
    qtest_quit(s);
}

static void test_opposing_lu(void)
{
    start();
    QDict *p = project();
    NET(p, "bus", "U1.io4", "U1.io5");
    qtest_writel(s, PAD(4), INPUT);
    qtest_writel(s, PAD(5), INPUT);
    qtest_writel(s, GPIO(A_GPIO_ENABLE_W1TS), BIT(4) | BIT(5));
    qtest_writel(s, GPIO(A_GPIO_OUT_W1TS), BIT(4));
    apply(p);
    QDict *snap = snapshot();
    voltage(snap, 4, 1.65);
    g_assert_false(qdict_get_bool(pad(snap, 4), "digital_valid"));
    const QListEntry *e;
    unsigned count = 0;
    QLIST_FOREACH_ENTRY(qdict_get_qlist(snap, "currents"), e) {
        QDict *c = qobject_to(QDict, qlist_entry_obj(e));
        if (!strcmp(qdict_get_str(c, "component_id"), "native-firmware-drive/pull")) {
            g_assert_cmpfloat_with_epsilon(fabs(number(c, "current_a")), 3.3 / 80, 1e-9);
            count++;
        }
    }
    g_assert_cmpuint(count, ==, 2);
    uint64_t factors = qdict_get_int(snap, "dc_factorizations");
    uint64_t gen = counter(snap, "generation");
    uint64_t source = counter(snap, "source_generation");
    qobject_unref(snap);
    /* Firmware no-op reuses the accepted solved state. A physical OUT
     * polarity change moves a branch between rails and may refactor. */
    qtest_writel(s, GPIO(A_GPIO_OUT), BIT(4));
    snap = snapshot();
    g_assert_cmpuint(qdict_get_int(snap, "dc_factorizations"), ==, factors);
    g_assert_cmpuint(counter(snap, "source_generation"), ==, source);
    qobject_unref(snap);
    /* Actual source-only edit: fixed OE/branch endpoints, changed VDD RHS. */
    quantity(component(p, "V"), "voltage", 3.0, "V");
    apply(p);
    snap = snapshot();
    voltage(snap, 4, 1.5);
    g_assert_cmpuint(qdict_get_int(snap, "dc_factorizations"), ==, factors);
    g_assert_cmpuint(counter(snap, "generation"), ==, gen + 1);
    g_assert_cmpuint(counter(snap, "source_generation"), >, source);
    qobject_unref(snap);
    qobject_unref(p);
    qtest_quit(s);
}

static void reverse_array(QDict *d, const char *key)
{
    QList *old = qdict_get_qlist(d, key), *reversed = qlist_new();
    const QListEntry *e;
    GPtrArray *items = g_ptr_array_new();
    QLIST_FOREACH_ENTRY(old, e) {
        g_ptr_array_add(items, qobject_ref(qlist_entry_obj(e)));
    }
    for (size_t i = items->len; i > 0; i--) {
        qlist_append_obj(reversed, items->pdata[i - 1]);
    }
    g_ptr_array_unref(items);
    qdict_put(d, key, reversed);
}

static void test_transaction_generation(void)
{
    start();
    QDict *p = project();
    NET(p, "bus", "U1.io4");
    apply(p);
    QDict *snap = snapshot();
    uint64_t gen = counter(snap, "generation");
    uint64_t source = counter(snap, "source_generation");
    qobject_unref(snap);
    qdict_put_str(p, "name", "Renamed display only");
    qdict_put(qdict_get_qdict(qdict_get_qdict(p, "geometry"), "components"), "U1", object("{\"x\":173,\"y\":81}"));
    qdict_put_str(component(p, "U1"), "name", "New MCU label");
    quantity(component(p, "V"), "voltage", 3300, "mV");
    /* Every unordered collection is normalized, not merely component order. */
    reverse_array(component(p, "U1"), "terminals");
    const QListEntry *entry;
    QLIST_FOREACH_ENTRY(qdict_get_qlist(p, "nets"), entry) {
        QDict *n = qobject_to(QDict, qlist_entry_obj(entry));
        qdict_put_str(n, "name", "Renamed net label");
        reverse_array(n, "endpoints");
    }
    reverse_array(p, "nets");
    reverse_array(p, "components");
    apply(p);
    apply(p);
    snap = snapshot();
    g_assert_cmpuint(counter(snap, "generation"), ==, gen);
    g_assert_cmpuint(counter(snap, "source_generation"), ==, source);
    qobject_unref(snap);
    g_autofree char *accepted = get_string("project-json");
    g_autofree char *before = get_string("snapshot-json");
    qdict_put_int(p, "version", 2);
    QDict *r = apply_reply(p);
    g_assert_true(qdict_haskey(r, "error"));
    qobject_unref(r);
    qdict_put_int(p, "version", 3);
    add_component(p, "D", "device");
    r = apply_reply(p);
    g_assert_true(qdict_haskey(r, "error"));
    qobject_unref(r);
    g_autofree char *after = get_string("snapshot-json");
    g_autofree char *retained = get_string("project-json");
    g_assert_cmpstr(after, ==, before);
    g_assert_cmpstr(retained, ==, accepted);
    qobject_unref(p);
    qtest_quit(s);
}

static void test_native_adc(void)
{
    start();
    QDict *p = project();
    resistor(p, "Rtop", 10000);
    resistor(p, "Rlow", 10000);
    NET(p, "vdd", "Rtop.a");
    NET(p, "gnd", "Rlow.b");
    NET(p, "mid", "Rtop.b", "Rlow.a", "U1.io1");
    apply(p);
    QDict *snap = snapshot();
    voltage(snap, 1, 1.65);
    qobject_unref(snap);
    set_running(true);
    /* Native SENS ADC1 channel0 at6dB: round(1.65 / 2.1948 * 4095). */
    qtest_writel(s, 0x60008904, qtest_readl(s, 0x60008904) | BIT(30));
    qtest_writel(s, 0x60008814, 2);
    qtest_writel(s, 0x6000880c, BIT(18) | BIT(19) | BIT(31));
    qtest_writel(s, 0x6000880c, BIT(17) | BIT(18) | BIT(19) | BIT(31));
    qtest_clock_step(s, 800);
    g_assert_cmpuint(qtest_readl(s, 0x60008840) >> 22, ==, 2);
    g_assert_cmphex(qtest_readl(s, 0x6000880c) & BIT(16), ==, 0);
    int64_t acquired_ns = qtest_clock_step(s, 800);
    g_assert_cmpuint(qtest_readl(s, 0x60008840) >> 22, ==, 3);
    snap = snapshot();
    voltage(snap, 1, 1.65);
    /* DC snapshots retain the last solve time; aperture time is native SENS. */
    g_assert_cmpuint(counter(snap, "timestamp_ns"), <=, acquired_ns);
    qobject_unref(snap);
    qtest_clock_step(s, 27600);
    uint32_t result = qtest_readl(s, 0x6000880c);
    g_test_message("Native ADC ctrl2=%#x FSM=%u aperture_ns=%" PRId64,
                   result, qtest_readl(s, 0x60008840) >> 22, acquired_ns);
    g_assert_cmphex(result & BIT(16), ==, BIT(16));
    g_assert_cmpuint(result & 0xffff, ==, 3079);
    qobject_unref(p);
    qtest_quit(s);
}

static void test_unknown_consumer(void)
{
    start();
    QDict *p = project();
    NET(p, "floating", "U1.io5");
    qtest_writel(s, PAD(5), INPUT);
    apply(p);
    QDict *running = qtest_qmp(s, "{'execute':'cont'}");
    g_assert_false(qdict_haskey(running, "error"));
    qobject_unref(running);
    running = qtest_qmp(s, "{'execute':'query-status'}");
    g_assert_true(qdict_get_bool(qdict_get_qdict(running, "return"), "running"));
    qobject_unref(running);
    /* Enabling an IRQ makes this unresolved pad an active digital consumer. */
    qtest_writel(s, GPIO(GPIO_PINn_REG_OFFSET(5)),
        (ESP32S3_GPIO_INT_RISING << R_GPIO_PINn_INT_TYPE_SHIFT) | ESP32S3_GPIO_INT_ENA_PROCPU);
    qtest_readl(s, GPIO(A_GPIO_IN));
    QDict *r = qtest_qmp(s, "{'execute':'query-status'}");
    g_assert_false(qdict_get_bool(qdict_get_qdict(r, "return"), "running"));
    g_assert_cmpstr(qdict_get_str(qdict_get_qdict(r, "return"), "status"), ==, "paused");
    qobject_unref(r);
    QDict *snap = snapshot();
    g_assert_false(qdict_get_bool(pad(snap, 5), "digital_valid"));
    g_assert_false(qdict_get_bool(pad(snap, 5), "valid"));
    qobject_unref(snap);
    qobject_unref(p);
    qtest_quit(s);
}

static void test_rc_charge_reset(void)
{
    start();
    QDict *p = project();
    QDict *electrical = qdict_get_qdict(qdict_get_qdict(p, "runtime"), "electrical");
    qdict_put_str(electrical, "mode", "rc");
    qdict_put_str(electrical, "edit_charge", "reset");
    resistor(p, "R", 10000);
    QDict *c = add_component(p, "C", "capacitor");
    terminal(c, "C.p", "p", "passive", -1);
    terminal(c, "C.n", "n", "passive", -1);
    quantity(c, "capacitance", 1, "uF");
    quantity(c, "initial_voltage", 0, "V");
    NET(p, "vdd", "R.a");
    NET(p, "gnd", "C.n");
    NET(p, "charge", "R.b", "C.p", "U1.io5");
    apply(p);
    set_running(true);
    qtest_clock_step(s, 5000000);
    QDict *snap = snapshot();
    double charged = number(pad(snap, 5), "voltage_v");
    g_assert_cmpfloat_with_epsilon(charged, 3.3 * (1 - exp(-.005 / .01)), .005);
    uint64_t generation = counter(snap, "generation");
    qobject_unref(snap);
    for (unsigned cpu = 0; cpu < 2; cpu++) {
        qtest_writel(s, 0x60008000, cpu ? BIT(4) : BIT(5));
        qtest_clock_step(s, 0);
        snap = snapshot();
        g_assert_cmpuint(counter(snap, "generation"), ==, generation);
        voltage(snap, 5, charged);
        qobject_unref(snap);
    }
    /* Peripheral reset is not an external circuit power-cycle. */
    qtest_writel(s, 0x600c0020, BIT(13));
    qtest_clock_step(s, 0);
    snap = snapshot();
    g_assert_cmpuint(counter(snap, "generation"), ==, generation);
    voltage(snap, 5, charged);
    qobject_unref(snap);
    QDict *r = qtest_qmp(s, "{'execute':'system_reset'}");
    g_assert_false(qdict_haskey(r, "error"));
    qobject_unref(r);
    qtest_clock_step(s, 0);
    snap = snapshot();
    g_assert_cmpuint(counter(snap, "generation"), ==, generation);
    voltage(snap, 5, charged);
    qobject_unref(snap);
    set_running(false);
    /* Electrically changed Apply, not a no-op, selects keep/reset. */
    qdict_put_str(electrical, "edit_charge", "keep");
    quantity(component(p, "R"), "resistance", 20000, "ohm");
    apply(p);
    snap = snapshot();
    voltage(snap, 5, charged);
    qobject_unref(snap);
    qdict_put_str(electrical, "edit_charge", "reset");
    quantity(component(p, "R"), "resistance", 10000, "ohm");
    apply(p);
    snap = snapshot();
    voltage(snap, 5, 0);
    qobject_unref(snap);
    qobject_unref(p);
    qtest_quit(s);
}

static void test_rc_firmware_step(void)
{
    start();
    QDict *p = project();
    QDict *electrical = qdict_get_qdict(qdict_get_qdict(p, "runtime"), "electrical");
    qdict_put_str(electrical, "mode", "rc");
    qdict_put_str(electrical, "edit_charge", "reset");
    resistor(p, "R", 10000);
    QDict *c = add_component(p, "C", "capacitor");
    terminal(c, "C.p", "p", "passive", -1);
    terminal(c, "C.n", "n", "passive", -1);
    quantity(c, "capacitance", 1, "uF");
    quantity(c, "initial_voltage", 0, "V");
    NET(p, "out", "R.a", "U1.io4");
    NET(p, "gnd", "C.n");
    NET(p, "charge", "R.b", "C.p", "U1.io5");
    qtest_writel(s, PAD(4), INPUT);
    qtest_writel(s, GPIO(A_GPIO_ENABLE_W1TS), BIT(4));
    qtest_writel(s, GPIO(A_GPIO_OUT_W1TS), BIT(4));
    apply(p);
    set_running(true);
    qtest_clock_step(s, 5000000);
    QDict *snap = snapshot();
    double charged = number(pad(snap, 5), "voltage_v");
    g_assert_cmpfloat_with_epsilon(charged, 3.3 * (1 - exp(-.005 / .01004)), .005);
    uint64_t generation = counter(snap, "generation");
    uint64_t source = counter(snap, "source_generation");
    qobject_unref(snap);
    for (unsigned cpu = 0; cpu < 2; cpu++) {
        qtest_writel(s, 0x60008000, cpu ? BIT(4) : BIT(5));
        qtest_clock_step(s, 0);
        g_assert_cmphex(qtest_readl(s, GPIO(A_GPIO_ENABLE)) & BIT(4), ==, BIT(4));
        g_assert_cmphex(qtest_readl(s, GPIO(A_GPIO_OUT)) & BIT(4), ==, BIT(4));
        snap = snapshot();
        voltage(snap, 5, charged);
        g_assert_cmpuint(counter(snap, "generation"), ==, generation);
        qobject_unref(snap);
    }
    qtest_writel(s, GPIO(A_GPIO_OUT_W1TC), BIT(4));
    snap = snapshot();
    voltage(snap, 5, charged);
    g_assert_cmpuint(counter(snap, "generation"), ==, generation);
    g_assert_cmpuint(counter(snap, "source_generation"), >, source);
    qobject_unref(snap);
    qtest_clock_step(s, 1000000);
    snap = snapshot();
    g_assert_cmpfloat_with_epsilon(number(pad(snap, 5), "voltage_v"),
                                 charged * exp(-.001 / .01004), .005);
    double retained = number(pad(snap, 5), "voltage_v");
    qobject_unref(snap);
    QDict *reset = qtest_qmp(s, "{'execute':'system_reset'}");
    g_assert_false(qdict_haskey(reset, "error"));
    qobject_unref(reset);
    qtest_clock_step(s, 0);
    g_assert_cmphex(qtest_readl(s, GPIO(A_GPIO_ENABLE)) & BIT(4), ==, 0);
    snap = snapshot();
    voltage(snap, 5, retained);
    g_assert_cmpuint(counter(snap, "generation"), ==, generation);
    qobject_unref(snap);
    /* The released output contributes no fabricated discharge path. */
    qtest_clock_step(s, 1000000);
    snap = snapshot();
    voltage(snap, 5, retained);
    qobject_unref(snap);
    qobject_unref(p);
    qtest_quit(s);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("esp32s3-electrical/divider-gpio-irq", test_divider_irq);
    qtest_add_func("esp32s3-electrical/disconnection-open-drain", test_disconnect_open_drain);
    qtest_add_func("esp32s3-electrical/opposing-drivers-lu", test_opposing_lu);
    qtest_add_func("esp32s3-electrical/transaction-generation", test_transaction_generation);
    qtest_add_func("esp32s3-electrical/native-adc", test_native_adc);
    qtest_add_func("esp32s3-electrical/unknown-consumer", test_unknown_consumer);
    qtest_add_func("esp32s3-electrical/rc-charge-reset", test_rc_charge_reset);
    qtest_add_func("esp32s3-electrical/rc-firmware-step", test_rc_firmware_step);
    return g_test_run();
}
