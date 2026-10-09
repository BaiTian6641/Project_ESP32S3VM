/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Native, virtual-time register tests. No hostbus or injected I2C provider.
 * Covers both timed master controllers, routed slave bytes/stretch/general-call/
 * ten-bit matching, SDA glitch rejection, address arbitration, FIFO/command/IRQ
 * boundaries, gate/reset cancellation and graph reachability/power errors.
 * Full master-data edge/fast-path equivalence and silicon qualification remain
 * separate from these software vectors.
 */
#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "libqtest.h"
#include "qapi/qmp/qjson.h"
#include "qapi/error.h"
#include "qapi/qmp/qlist.h"
#include "qapi/qmp/qnum.h"
#include "hw/gpio/esp32s3_gpio.h"
#include "hw/gpio/esp32s3_iomux.h"

#define GPIO(a) (0x60004000ULL + (a))
#define SYS(a) (0x600c0000ULL + (a))
#define PAD(n) (0x60009000ULL + IO_MUX_GPIOn_REG_OFFSET(n))
#define INPUT (R_IO_MUX_GPIOn_FUN_IE_MASK | (ESP32S3_IOMUX_MCU_SEL_GPIO << R_IO_MUX_GPIOn_MCU_SEL_SHIFT))
#define DONE BIT(31)
#define COMPLETE BIT(7)
#define NACK BIT(10)
#define TIMEOUT BIT(8)
#define END BIT(3)
#define START (6 << 11)
#define STOP (2 << 11)
#define FINISH (4 << 11)
#define WRITE(n) ((1 << 11) | BIT(8) | (n))
#define READ(n) ((3 << 11) | (n))
#define LAST(n) (READ(n) | BIT(10))
static QTestState *s;
static unsigned bus;
static uint64_t base(void) { return bus ? 0x60027000ULL : 0x60013000ULL; }
static uint32_t rd(unsigned reg) { return qtest_readl(s, base() + reg); }
static void wr(unsigned reg, uint32_t value) { qtest_writel(s, base() + reg, value); }
static void ticks(int64_t ns) { qtest_clock_step(s, ns); }
static void command(unsigned index, uint32_t value) { wr(0x58 + 4 * index, value); }
static void launch(void) { wr(4, BIT(4) | BIT(5) | 3); }
static void clean(void)
{
    wr(4, BIT(10) | BIT(4) | 3);
    wr(0x18, BIT(12) | BIT(13));
    wr(0x24, 0x3ffff);
    for (unsigned i = 0; i < 8; i++) { command(i, FINISH); }
}
static void tx(const uint8_t *bytes, size_t count)
{
    for (size_t i = 0; i < count; i++) { wr(0x1c, bytes[i]); }
}
static void complete(void)
{
    ticks(2000000);
    g_assert_cmphex(rd(0x20) & (COMPLETE | NACK | TIMEOUT), ==, COMPLETE);
    g_assert_cmphex(rd(8) & BIT(4), ==, 0);
}
static QDict *component(QDict *p, const char *id, const char *kind)
{
    QDict *c = qdict_new();
    qdict_put_str(c, "id", id); qdict_put_str(c, "name", id);
    qdict_put_str(c, "kind", kind); qdict_put_str(c, "type", kind);
    qdict_put(c, "parameters", qdict_new()); qdict_put(c, "terminals", qlist_new());
    qlist_append(qdict_get_qlist(p, "components"), c);
    return c;
}
static void terminal(QDict *c, const char *role, const char *domain,
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
static void quantity(QDict *c, const char *name, double value, const char *unit)
{
    QDict *q = qdict_new();
    qdict_put(q, "value", qnum_from_double(value)); qdict_put_str(q, "unit", unit);
    qdict_put(qdict_get_qdict(c, "parameters"), name, q);
}
static void net(QDict *p, const char *id, const char *a, const char *b)
{
    const QListEntry *e;
    QDict *n = NULL;
    QLIST_FOREACH_ENTRY(qdict_get_qlist(p, "nets"), e) {
        QDict *candidate = qobject_to(QDict, qlist_entry_obj(e));
        if (!strcmp(qdict_get_str(candidate, "id"), id)) { n = candidate; break; }
    }
    if (!n) {
        n = qdict_new(); qdict_put_str(n, "id", id); qdict_put_str(n, "name", id);
        qdict_put(n, "endpoints", qlist_new()); qlist_append(qdict_get_qlist(p, "nets"), n);
    }
    qlist_append_str(qdict_get_qlist(n, "endpoints"), a);
    if (b) { qlist_append_str(qdict_get_qlist(n, "endpoints"), b); }
}
/* Fault: 1=no pulls, 2=SCL grounded, 3=SDA grounded, 4=disconnected
 * matching address, 5=wrong address, 6=unpowered device. */
static void graph(unsigned fault)
{
    QObject *o = qobject_from_json("{\"version\":3,\"id\":\"i2c-native\",\"name\":\"I2C native\","
        "\"profile\":{\"chip\":\"esp32s3\",\"board\":\"esp32-s3-devkitc-1\",\"module\":\"esp32-s3-wroom-1\"},"
        "\"firmware\":{},\"runtime\":{\"electrical\":{\"driver_profile\":\"s3-explicit-finite-v1\",\"mode\":\"dc\"}},"
        "\"geometry\":{\"components\":{},\"nets\":{}},\"components\":[],\"nets\":[]}", &error_abort);
    QDict *p = qobject_to(QDict, o), *c = component(p, "U", "mcu");
    terminal(c, "vdd", "power", "input", -1); terminal(c, "gnd", "ground", "input", -1);
    for (unsigned i = 8; i <= 11; i++) {
        char role[16]; snprintf(role, sizeof(role), "io%u", i);
        terminal(c, role, "digital", "inout", i);
    }
    c = component(p, "G", "ground"); terminal(c, "ref", "ground", "unspecified", -1);
    c = component(p, "V", "voltage-source");
    terminal(c, "p", "power", "unspecified", -1); terminal(c, "n", "ground", "unspecified", -1);
    quantity(c, "voltage", 3.3, "V");
    net(p, "vdd", "V.p", "U.vdd"); net(p, "gnd", "G.ref", "V.n"); net(p, "gnd", "U.gnd", NULL);
    /* MCU and pulls never lose supply when the device rail is hot-switched. */
    c = component(p, "VDev", "voltage-source");
    terminal(c, "p", "power", "unspecified", -1);
    terminal(c, "n", "ground", "unspecified", -1);
    quantity(c, "voltage", fault == 8 ? 0 : 3.3, "V");
    net(p, "device-vdd", "VDev.p", NULL); net(p, "gnd", "VDev.n", NULL);
    for (unsigned b = 0; b < 2; b++) {
        char dev[16]; snprintf(dev, sizeof(dev), "D%u", b);
        c = component(p, dev, "device"); qdict_put_str(c, "type", b ? "24c02" : "sht21");
        terminal(c, "sda", "digital", "inout", -1); terminal(c, "scl", "digital", "inout", -1);
        terminal(c, "vdd", "power", "input", -1); terminal(c, "gnd", "ground", "input", -1);
        QDict *address = qdict_new(); qdict_put_int(address, "value", (b ? 0x50 : 0x40) + (fault == 5));
        qdict_put_str(address, "unit", "count"); qdict_put(qdict_get_qdict(c, "parameters"), "address", address);
        char endpoint[32]; snprintf(endpoint, sizeof(endpoint), "%s.gnd", dev); net(p, "gnd", endpoint, NULL);
        snprintf(endpoint, sizeof(endpoint), "%s.vdd", dev); net(p, fault == 6 ? "gnd" : "device-vdd", endpoint, NULL);
        for (unsigned line = 0; line < 2; line++) {
            char name[32], pad[32], device[32], r[32], a[40], z[40];
            snprintf(name, sizeof(name), "bus%u-%u", b, line);
            snprintf(pad, sizeof(pad), "U.io%u", 8 + b * 2 + line);
            snprintf(device, sizeof(device), "%s.%s", dev, line ? "scl" : "sda");
            net(p, name, pad, fault == 4 ? NULL : device);
            if (fault != 1) {
                snprintf(r, sizeof(r), "R%u%u", b, line); c = component(p, r, "resistor");
                terminal(c, "a", "passive", "unspecified", -1); terminal(c, "b", "passive", "unspecified", -1);
                quantity(c, "resistance", 4700, "ohm");
                snprintf(a, sizeof(a), "%s.a", r); snprintf(z, sizeof(z), "%s.b", r);
                net(p, "vdd", a, NULL); net(p, name, z, NULL);
            }
            if ((fault == 2 && line) || (fault == 3 && !line)) {
                snprintf(r, sizeof(r), "SHORT%u%u", b, line);
                c = component(p, r, "resistor");
                terminal(c, "a", "passive", "unspecified", -1);
                terminal(c, "b", "passive", "unspecified", -1);
                quantity(c, "resistance", 10, "ohm");
                snprintf(a, sizeof(a), "%s.a", r); snprintf(z, sizeof(z), "%s.b", r);
                net(p, name, a, NULL); net(p, "gnd", z, NULL);
            }
        }
    }
    QDict *status = qtest_qmp(s, "{'execute':'query-status'}");
    bool was_running = qdict_get_bool(qdict_get_qdict(status, "return"), "running");
    qobject_unref(status);
    if (was_running) {
        qtest_qmp_assert_success(s, "{'execute':'stop'}");
    }
    GString *json = qobject_to_json(o);
    QDict *reply = qtest_qmp(s, "{'execute':'qom-set','arguments':{'path':'/machine/soc/electrical','property':'project-json','value':%s}}", json->str);
    g_assert_false(qdict_haskey(reply, "error"));
    qobject_unref(reply); g_string_free(json, true); qobject_unref(o);
    if (was_running) {
        qtest_qmp_assert_success(s, "{'execute':'cont'}");
    }
}
static void setup(unsigned fault, unsigned irq_controller)
{
    s = qtest_initf("-machine esp32s3 -S -L pc-bios -global driver=esp32s3.gpio,property=strap_mode,value=0x00");
    qtest_irq_intercept_out_named(s, irq_controller ? "/machine/soc/i2c1" :
                                 "/machine/soc/i2c0", "sysbus-irq");
    qtest_writel(s, SYS(0x18), qtest_readl(s, SYS(0x18)) | BIT(7) | BIT(18));
    for (unsigned b = 0; b < 2; b++) {
        bus = b;
        for (unsigned line = 0; line < 2; line++) {
            unsigned pad = 8 + b * 2 + line, signal = (line ? 89 : 90) + b * 2;
            qtest_writel(s, PAD(pad), INPUT);
            qtest_writel(s, GPIO(GPIO_PINn_REG_OFFSET(pad)), R_GPIO_PINn_PAD_DRIVER_MASK);
            qtest_writel(s, GPIO(GPIO_FUNC_IN_SEL_CFG_OFFSET(signal)), BIT(7) | pad);
            qtest_writel(s, GPIO(GPIO_FUNC_OUT_SEL_CFG_OFFSET(pad)), signal);
        }
        wr(0, 199); wr(0x38, 200); wr(0x54, BIT(21)); wr(0x0c, BIT(5) | 16);
    }
    graph(fault);
    qtest_qmp_assert_success(s, "{'execute':'cont'}");
    /* Device rail Apply starts real power-on recovery before any address. */
    ticks(20000000);
}
static void write_stop(const uint8_t *data, unsigned count)
{
    clean(); tx(data, count); command(0, START); command(1, WRITE(count)); command(2, STOP); launch(); complete();
}
static void pointer(unsigned value)
{
    uint8_t bytes[] = {0xa0, value}; write_stop(bytes, sizeof(bytes));
}
static void read_eeprom(unsigned count, bool nack)
{
    clean(); wr(0x1c, 0xa1); command(0, START); command(1, WRITE(1));
    command(2, nack ? LAST(count) : READ(count)); command(3, STOP); launch();
}
static void test_masters_irq(void)
{
    for (unsigned controller = 0; controller < 2; controller++) {
        setup(0, controller); bus = controller;
        clean(); wr(0x28, 0); wr(0x1c, bus ? 0xa0 : 0x80); wr(0x1c, bus ? 0x31 : 0xe7);
        wr(0x1c, bus ? 0xa1 : 0x81);
        command(0, START); command(1, WRITE(2)); command(2, START);
        command(3, WRITE(1)); command(4, LAST(1)); command(5, STOP); launch();
        ticks(100); g_assert_cmphex(rd(8) & BIT(4), ==, BIT(4));
        g_assert_cmphex(rd(0x20) & COMPLETE, ==, 0);
        complete(); g_assert_cmphex(rd(0x2c), ==, 0);
        wr(0x28, COMPLETE); g_assert_cmphex(rd(0x2c), ==, COMPLETE);
        /* Each isolated machine intercepts its selected controller's IRQ0. */
        g_assert_true(qtest_get_irq(s, 0));
        g_assert_cmphex(rd(0x1c), ==, bus ? 0xff : 0x3a);
        for (unsigned i = 0; i < 6; i++) { g_assert_cmphex(rd(0x58 + i * 4) & DONE, ==, DONE); }
        wr(0x24, COMPLETE); g_assert_cmphex(rd(0x2c), ==, 0); g_assert_false(qtest_get_irq(s, 0));
        qtest_quit(s);
    }
}
static void test_eeprom_commit(void)
{
    setup(0, 1); bus = 1;
    uint8_t staged[] = {0xa0, 0x20, 0x55, 0xa1};
    clean(); tx(staged, sizeof(staged));
    command(0, START); command(1, WRITE(3)); command(2, START); command(3, WRITE(1));
    command(4, LAST(1)); command(5, STOP); launch(); complete();
    /* Repeated START reads erased memory, STOP then commits the staged page. */
    g_assert_cmphex(rd(0x1c), ==, 0xff);
    clean(); wr(0x1c, 0xa1); command(0, START); command(1, WRITE(1)); command(2, STOP); launch();
    ticks(500000); g_assert_cmphex(rd(0x20) & NACK, ==, NACK);
    ticks(6000000); pointer(0x20); read_eeprom(1, true); complete(); g_assert_cmphex(rd(0x1c), ==, 0x55);
    pointer(0x20);
    clean(); wr(0x1c, 0xa1); command(0, START); command(1, WRITE(1));
    command(2, LAST(1)); command(3, READ(1)); command(4, STOP); launch(); ticks(2000000);
    g_assert_cmphex(rd(0x20) & NACK, ==, NACK); g_assert_cmphex((rd(8) >> 8) & 63, ==, 0);
    qtest_quit(s);
}
static void test_end_and_large_transfer(void)
{
    setup(0, 1); bus = 1;
    uint8_t bytes[32] = {0xa0, 0x40};
    for (unsigned i = 2; i < 32; i++) { bytes[i] = i - 2; }
    clean(); tx(bytes, 32); command(0, START); command(1, WRITE(32)); command(2, FINISH); launch(); ticks(4000000);
    g_assert_cmphex(rd(0x20) & (END | COMPLETE), ==, END); g_assert_cmphex(rd(8) & BIT(4), ==, 0);
    /* Refill without reset/START: 48 payload bytes wrap the 16-byte page. */
    for (unsigned i = 30; i < 48; i++) { wr(0x1c, i); }
    command(0, WRITE(18)); command(1, STOP); wr(0x24, 0x3ffff); launch(); complete(); ticks(6000000);
    pointer(0x40);
    clean(); wr(0x1c, 0xa1); command(0, START); command(1, WRITE(1)); command(2, READ(32)); command(3, FINISH); launch(); ticks(4000000);
    g_assert_cmphex((rd(8) >> 8) & 63, ==, 32);
    for (unsigned i = 0; i < 32; i++) { g_assert_cmphex(rd(0x1c), ==, i < 16 ? 32 + i : 0xff); }
    command(0, READ(15)); command(1, LAST(1)); command(2, STOP); wr(0x24, 0x3ffff); launch(); complete();
    for (unsigned i = 0; i < 16; i++) { g_assert_cmphex(rd(0x1c), ==, 0xff); }
    qtest_quit(s);
}
static void test_command_bounds_fifo(void)
{
    setup(0, 1); bus = 1;
    clean(); command(0, START);
    for (unsigned i = 1; i < 7; i++) { command(i, WRITE(0)); }
    command(7, FINISH); wr(0x78, 0x12345678); launch(); ticks(2000000);
    g_assert_cmphex(rd(0x74) & DONE, ==, DONE); g_assert_cmphex(rd(0x78), ==, 0x12345678);
    g_assert_cmphex(rd(0x20) & END, ==, END);
    clean(); for (unsigned i = 0; i < 8; i++) { command(i, WRITE(0)); }
    launch(); ticks(2000000); g_assert_cmphex(rd(0x20) & TIMEOUT, ==, TIMEOUT);
    clean(); command(0, 7 << 11); launch(); ticks(2000000); g_assert_cmphex(rd(0x20) & TIMEOUT, ==, TIMEOUT);
    clean(); g_assert_cmphex(rd(0x1c), ==, 0); g_assert_cmphex(rd(0x20) & BIT(12), ==, BIT(12));
    clean(); for (unsigned i = 0; i < 33; i++) { wr(0x1c, i); }
    g_assert_cmphex((rd(8) >> 18) & 63, ==, 32); g_assert_cmphex(rd(0x20) & BIT(11), ==, BIT(11));
    clean(); command(0, START); command(1, WRITE(1)); launch(); ticks(2000000);
    g_assert_cmphex(rd(0x20) & BIT(6), ==, BIT(6));
    pointer(0); read_eeprom(33, false); ticks(5000000);
    g_assert_cmphex(rd(0x20) & BIT(2), ==, BIT(2)); g_assert_cmphex((rd(8) >> 8) & 63, ==, 0);
    qtest_quit(s);
}
static void test_nack_no_stale(void)
{
    setup(0, 0);
    for (bus = 0; bus < 2; bus++) {
        clean(); wr(0x1c, 0xfe); command(0, START); command(1, WRITE(1)); command(2, STOP); launch(); ticks(2000000);
        g_assert_cmphex(rd(0x20) & NACK, ==, NACK); g_assert_cmphex(rd(8) & BIT(4), ==, 0);
        g_assert_cmphex((rd(8) >> 8) & 63, ==, 0); g_assert_cmphex(rd(0x1c), ==, 0);
    }
    bus = 0;
    /* Realistic SHT21 data NACK: address ACKs, unsupported command rejects. */
    clean(); wr(0x1c, 0x80); wr(0x1c, 0x00); command(0, START); command(1, WRITE(2)); command(2, STOP); launch(); ticks(2000000);
    g_assert_cmphex(rd(0x20) & NACK, ==, NACK); g_assert_cmphex(rd(0x5c) & DONE, ==, 0);
    g_assert_cmphex((rd(8) >> 18) & 63, ==, 0);
    qtest_quit(s);
}
static void test_clock_cancel(void)
{
    for (unsigned controller = 0; controller < 2; controller++) {
        setup(0, controller); bus = controller;
        for (unsigned change = 0; change < 5; change++) {
            clean(); wr(0x28, COMPLETE | BIT(9)); wr(0x1c, bus ? 0xa0 : 0x80);
            command(0, START); command(1, WRITE(1)); command(2, STOP); launch(); ticks(100);
            g_assert_cmphex(rd(8) & BIT(4), ==, BIT(4));
            unsigned bit = bus ? 18 : 7;
            uint32_t gate = qtest_readl(s, SYS(0x18));
            if (change < 3) { wr(0x54, change == 0 ? BIT(21) | 3 : change == 1 ? BIT(21) | BIT(20) : 0); }
            else if (change == 3) { qtest_writel(s, SYS(0x18), gate & ~BIT(bit)); }
            else { qtest_writel(s, SYS(0x20), BIT(bit)); }
            ticks(3000000); g_assert_cmphex(rd(8) & BIT(4), ==, 0);
            g_assert_cmphex(rd(0x20) & (change == 4 ? ~3u : UINT32_MAX), ==, 0);
            if (change == 4) { g_assert_cmphex(rd(0x28), ==, 0); }
            g_assert_cmphex(rd(0x2c), ==, 0); g_assert_false(qtest_get_irq(s, 0));
            qtest_writel(s, SYS(0x18), gate); qtest_writel(s, SYS(0x20), 0);
            wr(0x54, BIT(21)); wr(0, 199); wr(0x38, 200);
        }
        qtest_quit(s);
    }
}
static void test_electrical_faults(void)
{
    for (unsigned fault = 1; fault <= 6; fault++) {
        setup(fault, 0);
        for (bus = 0; bus < 2; bus++) {
            clean(); wr(0x1c, bus ? 0xa0 : 0x80); command(0, START); command(1, WRITE(1)); command(2, STOP); launch(); ticks(4000000);
            g_assert_cmphex(rd(0x20) & (TIMEOUT | NACK), ==, fault <= 3 ? TIMEOUT : NACK);
            g_assert_cmphex(rd(0x20) & COMPLETE, ==, 0); g_assert_cmphex((rd(8) >> 8) & 63, ==, 0);
        }
        qtest_quit(s);
    }
    setup(0, 0); bus = 0;
    /* Actual IO_MUX disconnection cannot be rescued by matching metadata. */
    qtest_writel(s, PAD(8), 0); clean(); wr(0x1c, 0x80); command(0, START); command(1, WRITE(1)); launch(); ticks(4000000);
    g_assert_cmphex(rd(0x20) & TIMEOUT, ==, TIMEOUT); qtest_quit(s);
}
static void test_hold_stretch(void)
{
    for (unsigned scenario = 0; scenario < 4; scenario++) {
        setup(0, 0); bus = 0; clean();
        wr(0x0c, BIT(5) | (scenario == 1 ? 16 : 22));
        wr(0x28, COMPLETE | TIMEOUT);
        uint8_t request[] = {0x80, 0xe3, 0x81};
        tx(request, sizeof(request));
        command(0, START); command(1, WRITE(2)); command(2, START);
        command(3, WRITE(1)); command(4, READ(2));
        command(5, LAST(1)); command(6, STOP); launch(); ticks(1000000);
        g_assert_cmphex(rd(8) & BIT(4), ==, BIT(4));
        g_assert_cmphex(rd(0x20) & (COMPLETE | TIMEOUT), ==, 0);
        g_assert_cmphex((rd(8) >> 8) & 63, ==, 0);
        /* Real SHT21 OD driver, on the solved graph, holds SCL low. */
        g_assert_cmphex(qtest_readl(s, GPIO(A_GPIO_IN)) & BIT(9), ==, 0);
        g_assert_cmphex(qtest_readl(s, GPIO(A_GPIO_IN)) & BIT(8), ==, BIT(8));
        if (scenario == 0) {
            ticks(80000000);
            g_assert_cmphex(rd(8) & BIT(4), ==, BIT(4));
            g_assert_cmphex(rd(0x20) & COMPLETE, ==, 0);
            ticks(10000000);
            g_assert_cmphex(rd(0x20) & (COMPLETE | TIMEOUT | NACK), ==, COMPLETE);
            uint8_t data[2] = {rd(0x1c), rd(0x1c)}, crc = 0;
            g_assert_cmphex(data[0], ==, 0x68); g_assert_cmphex(data[1], ==, 0x30);
            for (unsigned i = 0; i < 2; i++) {
                crc ^= data[i];
                for (unsigned bit = 0; bit < 8; bit++) {
                    crc = (crc << 1) ^ ((crc & 0x80) ? 0x31 : 0);
                }
            }
            g_assert_cmphex(rd(0x1c), ==, crc);
        } else if (scenario == 1) {
            ticks(3000000);
            g_assert_cmphex(rd(0x20) & (COMPLETE | TIMEOUT | NACK), ==, TIMEOUT);
            g_assert_cmphex((rd(8) >> 8) & 63, ==, 0);
            ticks(100000000);
            g_assert_cmphex(rd(0x20) & COMPLETE, ==, 0);
        } else {
            if (scenario == 2) {
                qtest_writel(s, SYS(0x18), qtest_readl(s, SYS(0x18)) & ~BIT(7));
            } else {
                qtest_writel(s, SYS(0x20), BIT(7));
            }
            ticks(100000000);
            g_assert_cmphex(rd(0x20) & (scenario == 3 ? ~3u : UINT32_MAX), ==, 0);
            if (scenario == 3) { g_assert_cmphex(rd(0x28), ==, 0); }
            g_assert_cmphex(rd(0x2c), ==, 0);
            g_assert_false(qtest_get_irq(s, 0));
            g_assert_cmphex((rd(8) >> 8) & 63, ==, 0);
        }
        g_assert_cmphex(rd(8) & BIT(4), ==, 0);
        g_assert_cmphex(qtest_readl(s, GPIO(A_GPIO_IN)) & BIT(9), ==, BIT(9));
        qtest_quit(s);
    }
}
static void sht_no_hold(void)
{
    uint8_t request[] = {0x80, 0xf3};
    write_stop(request, sizeof(request));
}
static void sht_read_sample(void)
{
    clean(); wr(0x1c, 0x81);
    command(0, START); command(1, WRITE(1)); command(2, READ(2));
    command(3, LAST(1)); command(4, STOP); launch();
}
static void sample_crc(uint16_t expected)
{
    uint8_t data[2] = {rd(0x1c), rd(0x1c)}, crc = 0;
    g_assert_cmphex(((unsigned)data[0] << 8) | data[1], ==, expected);
    for (unsigned i = 0; i < 2; i++) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; bit++) {
            crc = (crc << 1) ^ ((crc & 0x80) ? 0x31 : 0);
        }
    }
    g_assert_cmphex(rd(0x1c), ==, crc);
}
static void test_hot_power_and_unplug(void)
{
    setup(0, 0); bus = 0;
    sht_no_hold();
    graph(4); ticks(10000000); graph(0); ticks(90000000);
    /* Wire-only unplug cannot power-reset an independently powered sensor. */
    sht_read_sample(); complete(); sample_crc(0x6830);
    sht_no_hold(); graph(8); ticks(1000000); graph(0);
    /* Real rail loss clears the old conversion; even after POR it is absent. */
    ticks(20000000); sht_read_sample(); ticks(2000000);
    g_assert_cmphex(rd(0x20) & (NACK | COMPLETE), ==, NACK);
    g_assert_cmphex((rd(8) >> 8) & 63, ==, 0);
    sht_no_hold(); ticks(90000000);
    sht_read_sample(); complete(); sample_crc(0x6830);
    bus = 1;
    uint8_t committed[] = {0xa0, 0x10, 0xa6};
    write_stop(committed, sizeof(committed)); ticks(6000000);
    graph(8); ticks(1000000); graph(0); ticks(20000000);
    pointer(0x10); read_eeprom(1, true); complete();
    g_assert_cmphex(rd(0x1c), ==, 0xa6);
    uint8_t staged[] = {0xa0, 0x10, 0x33};
    clean(); tx(staged, sizeof(staged));
    command(0, START); command(1, WRITE(3)); command(2, FINISH);
    launch(); ticks(2000000);
    g_assert_cmphex(rd(0x20) & END, ==, END);
    graph(8); ticks(1000000); graph(0); ticks(20000000);
    pointer(0x10); read_eeprom(1, true); complete();
    g_assert_cmphex(rd(0x1c), ==, 0xa6);
    qtest_quit(s);
}
/* ------------------------------------------------------------------ */
/* Advanced modes: the native slave edge FSM driven by a scripted graph
 * master peer, 10-bit/general-call addressing, master arbitration and the
 * input glitch filter, all through the shared electrical net. */
#define SLAVE_ADDR_REG 0x10
#define STRETCH_CONF 0x84
#define FILTER_CFG_REG 0x50
#define SLAVE_STRETCH_INT BIT(16)
#define DET_START_INT BIT(15)
#define GENERAL_CALL_INT BIT(17)
#define MAIN_ST_TO_INT BIT(14)

static void route_pads(unsigned first, unsigned count)
{
    for (unsigned line = 0; line < count * 2; line++) {
        unsigned pad = first + line, signal = (line & 1 ? 89 : 90) + (line / 2) * 2;
        qtest_writel(s, PAD(pad), INPUT);
        qtest_writel(s, GPIO(GPIO_PINn_REG_OFFSET(pad)), R_GPIO_PINn_PAD_DRIVER_MASK);
        qtest_writel(s, GPIO(GPIO_FUNC_IN_SEL_CFG_OFFSET(signal)), BIT(7) | pad);
        qtest_writel(s, GPIO(GPIO_FUNC_OUT_SEL_CFG_OFFSET(pad)), signal);
    }
}

static void peer_boot(uint32_t slave_addr_reg, bool stretch, bool slow_module_clock)
{
    s = qtest_initf("-machine esp32s3 -S -L pc-bios -global driver=esp32s3.gpio,property=strap_mode,value=0x00");
    qtest_irq_intercept_out_named(s, "/machine/soc/i2c0", "sysbus-irq");
    qtest_writel(s, SYS(0x18), qtest_readl(s, SYS(0x18)) | BIT(7) | BIT(18));
    bus = 0;
    route_pads(8, 1);
    wr(0, 199); wr(0x38, 200); wr(0x0c, BIT(5) | 16);
    wr(0x54, BIT(21) | (slow_module_clock ? 3 : 0));
    wr(SLAVE_ADDR_REG, slave_addr_reg);
    if (stretch) { wr(STRETCH_CONF, BIT(10) | 0x3ff); }
}

static void script_graph(const char *script_json, bool with_sensor,
                         bool expect_error)
{
    QObject *o = qobject_from_json("{\"version\":3,\"id\":\"i2c-peer\",\"name\":\"I2C peer graph\","
        "\"profile\":{\"chip\":\"esp32s3\",\"board\":\"esp32-s3-devkitc-1\",\"module\":\"esp32-s3-wroom-1\"},"
        "\"firmware\":{},\"runtime\":{\"electrical\":{\"driver_profile\":\"s3-explicit-finite-v1\",\"mode\":\"dc\"}},"
        "\"geometry\":{\"components\":{},\"nets\":{}},\"components\":[],\"nets\":[]}", &error_abort);
    QDict *p = qobject_to(QDict, o), *c = component(p, "U", "mcu");
    terminal(c, "vdd", "power", "input", -1); terminal(c, "gnd", "ground", "input", -1);
    for (unsigned i = 8; i <= 9; i++) {
        char role[16]; snprintf(role, sizeof(role), "io%u", i);
        terminal(c, role, "digital", "inout", i);
    }
    c = component(p, "G", "ground"); terminal(c, "ref", "ground", "unspecified", -1);
    c = component(p, "V", "voltage-source");
    terminal(c, "p", "power", "unspecified", -1); terminal(c, "n", "ground", "unspecified", -1);
    quantity(c, "voltage", 3.3, "V");
    net(p, "vdd", "V.p", "U.vdd"); net(p, "gnd", "G.ref", "V.n"); net(p, "gnd", "U.gnd", NULL);
    /* The graph-owned scripted master drives this bus as a physical peer. */
    c = component(p, "SM", "device"); qdict_put_str(c, "type", "i2c-scripted-master");
    terminal(c, "sda", "digital", "inout", -1); terminal(c, "scl", "digital", "inout", -1);
    terminal(c, "vdd", "power", "input", -1); terminal(c, "gnd", "ground", "input", -1);
    QDict *attributes = qdict_new();
    qdict_put(attributes, "native_i2c_script",
              qobject_from_json(script_json, &error_abort));
    qdict_put(c, "attributes", attributes);
    net(p, "bus-sda", "U.io8", "SM.sda"); net(p, "bus-scl", "U.io9", "SM.scl");
    net(p, "vdd", "SM.vdd", NULL); net(p, "gnd", "SM.gnd", NULL);
    if (with_sensor) {
        c = component(p, "D", "device"); qdict_put_str(c, "type", "sht21");
        terminal(c, "sda", "digital", "inout", -1); terminal(c, "scl", "digital", "inout", -1);
        terminal(c, "vdd", "power", "input", -1); terminal(c, "gnd", "ground", "input", -1);
        QDict *address = qdict_new(); qdict_put_int(address, "value", 0x40);
        qdict_put_str(address, "unit", "count");
        qdict_put(qdict_get_qdict(c, "parameters"), "address", address);
        net(p, "bus-sda", "D.sda", NULL); net(p, "bus-scl", "D.scl", NULL);
        net(p, "vdd", "D.vdd", NULL); net(p, "gnd", "D.gnd", NULL);
    }
    for (unsigned line = 0; line < 2; line++) {
        char r[16], a[24], z[24];
        snprintf(r, sizeof(r), "R%u", line); c = component(p, r, "resistor");
        terminal(c, "a", "passive", "unspecified", -1);
        terminal(c, "b", "passive", "unspecified", -1);
        quantity(c, "resistance", 4700, "ohm");
        snprintf(a, sizeof(a), "%s.a", r); snprintf(z, sizeof(z), "%s.b", r);
        net(p, "vdd", a, NULL);
        net(p, line ? "bus-scl" : "bus-sda", z, NULL);
    }
    qtest_qmp_assert_success(s, "{'execute':'stop'}");
    GString *json = qobject_to_json(o);
    QDict *reply = qtest_qmp(s, "{'execute':'qom-set','arguments':{'path':'/machine/soc/electrical','property':'project-json','value':%s}}", json->str);
    g_assert_cmpint(qdict_haskey(reply, "error"), ==, expect_error);
    qobject_unref(reply); g_string_free(json, true); qobject_unref(o);
    qtest_qmp_assert_success(s, "{'execute':'cont'}");
}

static void test_slave_script_write(void)
{
    peer_boot(0x2A, false, true);
    script_graph("{\"bit_ns\":10000,\"transactions\":[{\"address\":42,\"data\":[90,51,192]}]}", false, false);
    ticks(3000000);
    g_assert_cmphex(rd(0x20) & DET_START_INT, ==, DET_START_INT);
    g_assert_cmphex(rd(0x20) & COMPLETE, ==, COMPLETE);
    g_assert_cmphex((rd(0x14) >> 22) & 0xff, ==, 3);
    g_assert_cmphex(rd(0x1c), ==, 90);
    g_assert_cmphex(rd(0x1c), ==, 51);
    g_assert_cmphex(rd(0x1c), ==, 192);
    g_assert_cmphex((rd(8) >> 8) & 63, ==, 0);
    g_assert_cmphex(rd(8) & BIT(5), ==, 0);
    qtest_quit(s);
}

static void test_slave_read_stretch(void)
{
    /* Served read: stretch at address match, explicit clear, full drain. */
    peer_boot(0x2A, true, true);
    wr(0x1c, 0x12); wr(0x1c, 0x34);
    script_graph("{\"bit_ns\":10000,\"transactions\":[{\"address\":42,\"read\":true,\"count\":2}]}", false, false);
    ticks(150000);
    g_assert_cmphex(rd(0x20) & SLAVE_STRETCH_INT, ==, SLAVE_STRETCH_INT);
    g_assert_cmphex((rd(8) >> 14) & 3, ==, 0);
    g_assert_cmphex(rd(8) & BIT(5), ==, BIT(5));
    g_assert_cmphex(rd(8) & BIT(1), ==, BIT(1));
    g_assert_cmphex(rd(8) & BIT(4), ==, BIT(4));
    wr(0x84, BIT(11));
    ticks(3000000);
    g_assert_cmphex(rd(0x20) & COMPLETE, ==, COMPLETE);
    g_assert_cmphex((rd(8) >> 18) & 63, ==, 0);
    g_assert_cmphex(rd(8) & BIT(5), ==, 0);
    qtest_quit(s);
    /* Unserved read: stretch protection releases SCL and ends the transfer. */
    peer_boot(0x2A, true, true);
    wr(0x1c, 0x12); wr(0x1c, 0x34);
    script_graph("{\"bit_ns\":10000,\"transactions\":[{\"address\":42,\"read\":true,\"count\":2}]}", false, false);
    ticks(3000000);
    g_assert_cmphex(rd(0x20) & MAIN_ST_TO_INT, ==, MAIN_ST_TO_INT);
    g_assert_cmphex(rd(8) & BIT(5), ==, 0);
    g_assert_cmphex((rd(8) >> 18) & 63, ==, 2);
    qtest_quit(s);
}

static void test_slave_relay(void)
{
    peer_boot(0x2A, true, true);
    wr(0x1c, 0x11); wr(0x1c, 0x22); wr(0x1c, 0x33);
    script_graph("{\"bit_ns\":10000,\"transactions\":[{\"address\":42,\"relay\":3}]}", false, false);
    ticks(150000);
    g_assert_cmphex((rd(8) >> 14) & 3, ==, 0);
    wr(0x84, BIT(11));
    ticks(5000000);
    g_assert_cmphex(rd(0x20) & COMPLETE, ==, COMPLETE);
    /* The relay read drained TX; the write pass delivered the same bytes. */
    g_assert_cmphex((rd(8) >> 18) & 63, ==, 0);
    g_assert_cmphex((rd(0x14) >> 22) & 0xff, ==, 3);
    g_assert_cmphex(rd(0x1c), ==, 0x11);
    g_assert_cmphex(rd(0x1c), ==, 0x22);
    g_assert_cmphex(rd(0x1c), ==, 0x33);
    qtest_quit(s);
}

static void test_slave_ten_bit(void)
{
    /* 0x1A2 remapped into the SLAVE_ADDR register as the driver programs it. */
    peer_boot(BIT(31) | ((0xA2 << 7) | 0x79), false, true);
    script_graph("{\"bit_ns\":10000,\"transactions\":["
                 "{\"address\":418,\"ten_bit\":true,\"data\":[119]},"
                 "{\"address\":121,\"data\":[5]}]}", false, false);
    ticks(5000000);
    g_assert_cmphex(rd(0x20) & COMPLETE, ==, COMPLETE);
    g_assert_cmphex(rd(0x1c), ==, 119);
    /* The 7-bit alias of the 10-bit header is not this device's address. */
    g_assert_cmphex((rd(8) >> 8) & 63, ==, 0);
    qtest_quit(s);
}

static void test_slave_general_call(void)
{
    peer_boot(0x2A, false, true);
    wr(4, BIT(14) | 3);
    script_graph("{\"bit_ns\":10000,\"transactions\":[{\"address\":0,\"data\":[6]}]}", false, false);
    ticks(3000000);
    g_assert_cmphex(rd(0x20) & GENERAL_CALL_INT, ==, GENERAL_CALL_INT);
    g_assert_cmphex(rd(0x1c), ==, 6);
    /* Broadcast disabled: the second write is not acknowledged or received. */
    wr(4, 3);
    wr(0x24, 0x7ffff);
    script_graph("{\"bit_ns\":10000,\"transactions\":[{\"address\":0,\"data\":[7]}]}", false, false);
    ticks(3000000);
    g_assert_cmphex(rd(0x20) & GENERAL_CALL_INT, ==, 0);
    g_assert_cmphex((rd(8) >> 8) & 63, ==, 0);
    g_assert_cmphex(rd(0x20) & COMPLETE, ==, 0);
    qtest_quit(s);
}

static void test_slave_glitch_filter(void)
{
    peer_boot(0x2A, false, true);
    /* SDA filter: 8 module cycles at the divided clock (800 ns). */
    wr(FILTER_CFG_REG, BIT(9) | (8 << 4));
    script_graph("{\"bit_ns\":10000,\"transactions\":[{\"glitch_ns\":100}]}", false, false);
    ticks(100000);
    /* The 100 ns SDA excursion while SCL is high is not a START. */
    g_assert_cmphex(rd(0x20) & DET_START_INT, ==, 0);
    script_graph("{\"bit_ns\":10000,\"transactions\":[{\"address\":42,\"data\":[153]}]}", false, false);
    ticks(3000000);
    g_assert_cmphex(rd(0x20) & DET_START_INT, ==, DET_START_INT);
    g_assert_cmphex(rd(0x1c), ==, 153);
    g_assert_cmphex(rd(0x20) & COMPLETE, ==, COMPLETE);
    qtest_quit(s);
}

static void test_slave_wrong_address(void)
{
    peer_boot(0x2A, false, true);
    script_graph("{\"bit_ns\":10000,\"transactions\":[{\"address\":43,\"data\":[1]}]}", false, false);
    ticks(3000000);
    g_assert_cmphex(rd(0x20) & DET_START_INT, ==, DET_START_INT);
    g_assert_cmphex(rd(0x20) & COMPLETE, ==, 0);
    g_assert_cmphex((rd(8) >> 8) & 63, ==, 0);
    g_assert_cmphex(rd(8) & BIT(5), ==, 0);
    qtest_quit(s);
}

static void test_master_arbitration(void)
{
    /* The scripted master holds SDA low through its zero address bit; the
     * native master transmitting a one loses arbitration on the wire. */
    peer_boot(0, false, false);
    clean(); wr(0x1c, 0x80);
    command(0, START); command(1, WRITE(1)); command(2, STOP);
    script_graph("{\"bit_ns\":4000,\"start_delay_ns\":20000000,\"transactions\":[{\"address\":42,\"data\":[0]}]}", true, false);
    ticks(20000000);
    wr(4, BIT(9) | BIT(4) | BIT(5) | 3);
    ticks(2000000);
    g_assert_cmphex(rd(0x20) & BIT(5), ==, BIT(5));
    g_assert_cmphex(rd(0x20) & COMPLETE, ==, 0);
    g_assert_cmphex(rd(8) & BIT(3), ==, BIT(3));
    g_assert_cmphex(rd(8) & BIT(4), ==, 0);
    qtest_quit(s);
    /* Symmetric case: the native master's zero bit defeats the scripted
     * master, which withdraws, and the native transfer completes. */
    peer_boot(0, false, false);
    clean(); wr(0x1c, 0x80);
    command(0, START); command(1, WRITE(1)); command(2, STOP);
    script_graph("{\"bit_ns\":4000,\"start_delay_ns\":20000000,\"transactions\":[{\"address\":96,\"data\":[0]}]}", true, false);
    ticks(20000000);
    wr(4, BIT(9) | BIT(4) | BIT(5) | 3);
    ticks(2000000);
    g_assert_cmphex(rd(0x20) & BIT(5), ==, 0);
    g_assert_cmphex(rd(0x20) & COMPLETE, ==, COMPLETE);
    g_assert_cmphex(rd(8) & BIT(3), ==, 0);
    qtest_quit(s);
}

static void test_script_preflight_reject(void)
{
    static const char *invalid[] = {
        "{\"bit_ns\":\"bad\",\"transactions\":[{\"address\":42,\"data\":[1]}]}",
        "{\"bit_ns\":10000,\"transactions\":[{\"glitch_ns\":\"bad\"}]}",
        "{\"bit_ns\":10000,\"transactions\":[{\"address\":42,\"relay\":\"bad\"}]}"
    };
    peer_boot(0x2A, false, true);
    wr(0x1c, 0xa5);
    for (unsigned i = 0; i < G_N_ELEMENTS(invalid); ++i) {
        script_graph(invalid[i], false, true);
        ticks(100000);
        g_assert_cmphex((rd(8) >> 18) & 63, ==, 1);
        g_assert_cmphex(rd(0x20) & (DET_START_INT | COMPLETE), ==, 0);
    }
    script_graph("{\"bit_ns\":10000,\"transactions\":[{\"address\":42,\"relay\":1}]}",
                 false, false);
    ticks(3000000);
    g_assert_cmphex(rd(0x20) & COMPLETE, ==, COMPLETE);
    g_assert_cmphex((rd(8) >> 18) & 63, ==, 0);
    g_assert_cmphex((rd(8) >> 8) & 63, ==, 1);
    g_assert_cmphex(rd(0x1c), ==, 0xa5);
    qtest_quit(s);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/esp32s3/i2c/both-masters-timing-irq", test_masters_irq);
    qtest_add_func("/esp32s3/i2c/eeprom-stop-restart-final-nack", test_eeprom_commit);
    qtest_add_func("/esp32s3/i2c/end-refill-drain-over32", test_end_and_large_transfer);
    qtest_add_func("/esp32s3/i2c/command-bounds-fifo-errors", test_command_bounds_fifo);
    qtest_add_func("/esp32s3/i2c/address-data-nack-no-stale", test_nack_no_stale);
    qtest_add_func("/esp32s3/i2c/clock-gate-reset-cancel", test_clock_cancel);
    qtest_add_func("/esp32s3/i2c/electrical-route-power-pulls", test_electrical_faults);
    qtest_add_func("/esp32s3/i2c/hold-stretch-timeout-cancel", test_hold_stretch);
    qtest_add_func("/esp32s3/i2c/hot-power-versus-wire-unplug", test_hot_power_and_unplug);
    qtest_add_func("/esp32s3/i2c/slave-script-write", test_slave_script_write);
    qtest_add_func("/esp32s3/i2c/slave-read-stretch", test_slave_read_stretch);
    qtest_add_func("/esp32s3/i2c/slave-relay", test_slave_relay);
    qtest_add_func("/esp32s3/i2c/slave-ten-bit", test_slave_ten_bit);
    qtest_add_func("/esp32s3/i2c/slave-general-call", test_slave_general_call);
    qtest_add_func("/esp32s3/i2c/slave-glitch-filter", test_slave_glitch_filter);
    qtest_add_func("/esp32s3/i2c/slave-wrong-address", test_slave_wrong_address);
    qtest_add_func("/esp32s3/i2c/master-arbitration", test_master_arbitration);
    qtest_add_func("/esp32s3/i2c/script-preflight-reject", test_script_preflight_reject);
    return g_test_run();
}
