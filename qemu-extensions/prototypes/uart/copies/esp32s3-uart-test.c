/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Actual esp32s3 machine, GPIO matrix, ProjectDocument v3 physical nets and
 * native UART timers. No firmware, injected GPIO inputs, forced interrupts,
 * synthetic samples, helper ACKs or substitute UART instances are used.
 * UART_QTEST_PROJECT_DIR names the ordinary fixture's uart/firmware directory.
 */
#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "libqtest.h"
#include "hw/gpio/esp32s3_gpio.h"
#include "hw/gpio/esp32s3_iomux.h"

#define UART(c) ((c) == 2 ? 0x6002e000ULL : 0x60000000ULL + (c) * 0x10000ULL)
#define GPIO(r) (0x60004000ULL + (r))
#define PAD(n) (0x60009000ULL + IO_MUX_GPIOn_REG_OFFSET(n))
#define IN(n) GPIO(GPIO_FUNC_IN_SEL_CFG_OFFSET(n))
#define OUT(n) GPIO(GPIO_FUNC_OUT_SEL_CFG_OFFSET(n))
#define INPUT (R_IO_MUX_GPIOn_FUN_IE_MASK | \
               (ESP32S3_IOMUX_MCU_SEL_GPIO << R_IO_MUX_GPIOn_MCU_SEL_SHIFT))
#define EN(c) (0x600c0018ULL + ((c) == 2 ? 4 : 0))
#define RESET(c) (0x600c0020ULL + ((c) == 2 ? 4 : 0))
#define GATE(c) BIT((c) == 0 ? 2 : (c) == 1 ? 5 : 9)
#define SYSTEM_SYSCLK 0x600c0060ULL
#define FIFO 0x00
#define RAW 0x04
#define ST 0x08
#define ENA 0x0c
#define CLR 0x10
#define DIV 0x14
#define STATUS 0x1c
#define CONF0 0x20
#define CONF1 0x24
#define LOW 0x28
#define HIGH 0x2c
#define EDGES 0x30
#define IDLE_CONF 0x48
#define RS485 0x4c
#define AT_PRE 0x50
#define AT_POST 0x54
#define AT_GAP 0x58
#define AT_CHAR 0x5c
#define MEM_CONF 0x60
#define TX_PTR 0x64
#define RX_PTR 0x68
#define FSM 0x6c
#define POS_EDGE 0x70
#define NEG_EDGE 0x74
#define CLK 0x78
#define UPDATE 0x80
#define RX_FULL BIT(0)
#define TX_EMPTY BIT(1)
#define PARITY_ERR BIT(2)
#define FRAME_ERR BIT(3)
#define OVERFLOW BIT(4)
#define BREAK BIT(7)
#define TIMEOUT BIT(8)
#define TX_DONE BIT(14)
#define RS_PARITY BIT(15)
#define RS_FRAME BIT(16)
#define RS_CLASH BIT(17)
#define AT_DONE BIT(18)
#define ALL_IRQ 0x7ffffU
#define CLOCK_ON (BIT(22) | BIT(24) | BIT(25))
#define MEM_128 ((1U << 1) | (1U << 4))

/* Actual mapped UHCI0/GDMA and native internal DRAM; no standalone endpoints. */
#define UHCI(r) (0x60014000ULL + (r))
#define GDMA(c, out, r) (0x6003f000ULL + 0xc0 * (c) + ((out) ? 0x60 : 0) + (r))
#define D_CONF0 0x00
#define D_CONF1 0x04
#define D_RAW 0x08
#define D_CLR 0x14
#define D_LINK 0x20
#define D_EOF_DESC 0x28
#define D_ERR_DESC 0x2c
#define D_PERI 0x48
#define D_OWNER BIT(31)
#define D_EOF BIT(30)
#define D_DESC 0x3fc98000U
#define D_NEXT (D_DESC + 16)
#define D_BUFFER 0x3fc99000U
#define D_BUFFER_NEXT (D_BUFFER + 32)
#define D_UART_PERI 2U

/* SDK6.1 ESP32-S3 gpio_sig_map.h: directional matrix_signal namespaces overlap. */
static const unsigned matrix_signal[3] = {12, 15, 18};
static const unsigned cts_signal[3] = {13, 16, 19};
static const unsigned tx_pad[3] = {4, 17, 18};
static const unsigned rx_pad[3] = {5, 15, 16};
static const unsigned peer_pad[3] = {4, 18, 17};
static const unsigned destination[3] = {0, 2, 1};

typedef struct Fixture {
    QTestState *q;
    char *log;
    int64_t now;
} Fixture;

typedef struct Period {
    uint64_t num, den;
} Period;

static uint32_t rd(Fixture *f, unsigned c, unsigned reg)
{
    return qtest_readl(f->q, UART(c) + reg);
}

static void wr(Fixture *f, unsigned c, unsigned reg, uint32_t value)
{
    qtest_writel(f->q, UART(c) + reg, value);
}

static void step(Fixture *f, int64_t ns)
{
    g_assert_cmpint(ns, >=, 0);
    f->now = qtest_clock_step(f->q, ns);
}

static uint64_t duration(Period p, unsigned halves)
{
    return (p.num * halves + 2 * p.den - 1) / (2 * p.den);
}

static void at(Fixture *f, uint64_t origin, Period p, unsigned halves)
{
    step(f, origin + duration(p, halves) - f->now);
}

static unsigned rx_used(Fixture *f, unsigned c)
{
    return rd(f, c, STATUS) & 1023;
}

static unsigned tx_used(Fixture *f, unsigned c)
{
    return (rd(f, c, STATUS) >> 16) & 1023;
}

static void gate(Fixture *f, unsigned c, bool enabled)
{
    uint32_t value = qtest_readl(f->q, EN(c));
    qtest_writel(f->q, EN(c), enabled ? value | GATE(c) : value & ~GATE(c));
}

static void gpio(Fixture *f, unsigned pad, bool level)
{
    qtest_writel(f->q, GPIO(level ? A_GPIO_OUT_W1TS : A_GPIO_OUT_W1TC), BIT(pad));
}

static void peer(Fixture *f, unsigned pad, bool level)
{
    gpio(f, pad, level);
    qtest_writel(f->q, OUT(pad), GPIO_FUNC_OUT_SEL_NONE);
    qtest_writel(f->q, GPIO(A_GPIO_ENABLE_W1TS), BIT(pad));
}

static void sample(Fixture *f, unsigned c, unsigned pad, bool expected)
{
    bool actual = !!(qtest_readl(f->q, GPIO(A_GPIO_IN)) & BIT(pad));
    g_test_message("UART%u physical GPIO%u ns=%" PRId64 " level=%u",
                   c, pad, f->now, actual);
    g_assert_cmpint(actual, ==, expected);
}

static void format(Fixture *f, unsigned c, unsigned width,
                   int parity, unsigned stops, uint32_t flags)
{
    wr(f, c, CONF0, ((width - 5) << 2) | (stops << 4) |
       (parity >= 0 ? BIT(1) | (parity ? BIT(0) : 0) : 0) | flags);
}

static void clock_config(Fixture *f, unsigned c, unsigned source,
                         unsigned integer, unsigned fraction,
                         unsigned source_div, unsigned a, unsigned b)
{
    wr(f, c, DIV, integer | (fraction << 20));
    wr(f, c, CLK, CLOCK_ON | (source << 20) | ((source_div - 1) << 12) |
       (a << 6) | b);
}

static void reset_fifo(Fixture *f, unsigned c)
{
    uint32_t conf = rd(f, c, CONF0);
    wr(f, c, CONF0, conf | BIT(17) | BIT(18));
    wr(f, c, CONF0, conf);
    wr(f, c, CLR, ALL_IRQ);
}

static void graph(Fixture *f, const char *name)
{
    const char *directory = g_getenv("UART_QTEST_PROJECT_DIR");
    g_autofree char *path = NULL;
    g_autofree char *json = NULL;
    GError *error = NULL;

    g_assert_nonnull(directory);
    path = g_build_filename(directory, name, NULL);
    g_assert_true(g_file_get_contents(path, &json, NULL, &error));
    g_assert_no_error(error);
    /* project-json is a string property, not a test-only graph injection. */
    qtest_qmp_assert_success(f->q,
        "{'execute':'qom-set','arguments':{'path':'/machine/soc/electrical',"
        "'property':'project-json','value':%s}}", json);
}

/* The electrical device accepts a project only at a quiescent boundary, so a
 * graph change after the VM started must bracket it with stop/cont. */
static void graph_live(Fixture *f, const char *name)
{
    qtest_qmp_assert_success(f->q, "{'execute':'stop'}");
    graph(f, name);
    qtest_qmp_assert_success(f->q, "{'execute':'cont'}");
}

static Fixture setup(unsigned c)
{
    Fixture f = {0};
    GError *error = NULL;
    int fd = g_file_open_tmp("esp32s3-uart-qtest-XXXXXX", &f.log, &error);
    g_autofree char *args = NULL;
    g_autofree char *path = g_strdup_printf("/machine/soc/uart%u", c);

    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    close(fd);
    /* Start paused: the electrical project is only accepted at a quiescent
     * boundary, and the per-bit vectors need the virtual clock, which QEMU
     * enables when the VM is started. Order: configure, cont, then step. */
    args = g_strdup_printf("-machine esp32s3 -S -L pc-bios -D %s "
        "-global driver=esp32s3.gpio,property=strap_mode,value=0x00", f.log);
    f.q = qtest_init(args);
    f.now = qtest_clock_step(f.q, 0);
    graph(&f, "project-connected.json");
    qtest_qmp_assert_success(f.q, "{'execute':'cont'}");
    qtest_irq_intercept_out_named(f.q, path, "sysbus-irq");
    /* Program XTAL/PRE_DIV0 explicitly: APB40MHz, independent of ROM/reset. */
    qtest_writel(f.q, SYSTEM_SYSCLK, 0);
    /* Establish the explicit idle supply before enabling any matrix input. */
    qtest_writel(f.q, PAD(21), INPUT);
    for (unsigned i = 0; i < 3; i++) {
        qtest_writel(f.q, IN(matrix_signal[i]), BIT(7) | 21);
    }
    for (unsigned i = 0; i < 3; i++) {
        g_autofree char *uart_path = g_strdup_printf("/machine/soc/uart%u", i);
        qtest_qmp_assert_success(f.q,
            "{'execute':'qom-set','arguments':{'path':%s,'property':'trace','value':true}}",
            uart_path);
        qtest_writel(f.q, RESET(i), qtest_readl(f.q, RESET(i)) & ~GATE(i));
        gate(&f, i, true);
        wr(&f, i, UPDATE, BIT(30)); /* SDK's immediate/APB update mode. */
        clock_config(&f, i, 3, 40, 0, 1, 0, 0); /* XTAL 1 Mbaud. */
        format(&f, i, 8, -1, 1, 0);
        wr(&f, i, MEM_CONF, MEM_128);
        wr(&f, i, CONF1, 1);
        /* Frame vectors isolate shifter timing from the reset's 256-bit idle. */
        wr(&f, i, IDLE_CONF, 256); /* RX idle threshold256, TX idle zero. */
        wr(&f, i, ENA, 0);
        reset_fifo(&f, i);
        qtest_writel(f.q, PAD(tx_pad[i]), INPUT);
        qtest_writel(f.q, PAD(rx_pad[i]), INPUT);
        qtest_writel(f.q, OUT(tx_pad[i]), matrix_signal[i]);
        qtest_writel(f.q, GPIO(A_GPIO_ENABLE_W1TS), BIT(tx_pad[i]));
    }
    return f;
}

static void route_rx(Fixture *f, unsigned c)
{
    qtest_writel(f->q, IN(matrix_signal[c]), BIT(7) | rx_pad[c]);
}

static void trace_at(const char *text, unsigned c, const char *event,
                     uint64_t ns, const char *suffix)
{
    char expected[160];

    g_snprintf(expected, sizeof(expected),
               "esp32s3-uart%u event=%s ns=%" PRIu64 "%s", c, event, ns, suffix);
    g_assert_nonnull(strstr(text, expected));
    g_test_message("native trace: %s", expected);
}

static void teardown(Fixture *f)
{
    qtest_quit(f->q);
    g_assert_cmpint(unlink(f->log), ==, 0);
    g_free(f->log);
}

static void test_uart1_iomux_tx(void)
{
    Fixture f = setup(2);
    const Period p = {1000, 1};
    const uint8_t bytes[] = {0xa6, 0x3c};

    route_rx(&f, 2);
    qtest_writel(f.q, OUT(17), GPIO_FUNC_OUT_SEL_NONE);
    qtest_writel(f.q, GPIO(A_GPIO_ENABLE_W1TC), BIT(17));
    qtest_writel(f.q, PAD(17), 2U << R_IO_MUX_GPIOn_MCU_SEL_SHIFT);
    step(&f, 1000);
    sample(&f, 2, 16, true);

    for (unsigned n = 0; n < G_N_ELEMENTS(bytes); ++n) {
        uint64_t origin = f.now;
        wr(&f, 1, FIFO, bytes[n]);
        for (unsigned bit = 0; bit < 10; ++bit) {
            at(&f, origin, p, bit * 2 + 1);
            bool level = bit == 0 ? false : bit == 9 ? true :
                         !!(bytes[n] & BIT(bit - 1));
            sample(&f, 2, 16, level);
        }
        at(&f, origin, p, 20);
        g_assert_cmpuint(rx_used(&f, 2), ==, 1);
        g_assert_cmphex(rd(&f, 2, FIFO), ==, bytes[n]);
        if (n == 0) {
            /* A different mux function must release the native driver even
             * while the UART keeps sending. GPIO OE remains disabled. */
            qtest_writel(f.q, PAD(17), INPUT);
            wr(&f, 1, FIFO, 0);
            step(&f, 10000);
            sample(&f, 2, 16, true);
            g_assert_cmpuint(rx_used(&f, 2), ==, 0);
            qtest_writel(f.q, PAD(17), 2U << R_IO_MUX_GPIOn_MCU_SEL_SHIFT);
            step(&f, 1000);
        }
    }
    teardown(&f);
}

static bool parity_bit(uint8_t byte, unsigned width, bool odd)
{
    bool result = odd;
    for (unsigned i = 0; i < width; i++) {
        result ^= !!(byte & BIT(i));
    }
    return result;
}

/* Drive a real peer GPIO output, not a GPIO input IRQ or UART RX helper.
 * Log the physical pad at each receiver sampling center. */
static void frame(Fixture *f, unsigned c, uint8_t byte, unsigned width,
                  int parity, unsigned stops, bool bad_parity, bool bad_stop)
{
    unsigned driver = peer_pad[c];
    unsigned count = 1 + width + (parity >= 0);
    uint64_t origin;
    const Period p = {1000, 1};

    peer(f, driver, true);
    route_rx(f, c);
    step(f, 1000);
    origin = f->now;
    gpio(f, driver, false);
    for (unsigned i = 0; i < count; i++) {
        bool level = i ? (i <= width ? !!(byte & BIT(i - 1)) :
                     parity_bit(byte, width, parity) ^ bad_parity) : false;
        at(f, origin, p, i * 2);
        gpio(f, driver, level);
        at(f, origin, p, i * 2 + 1);
        sample(f, c, rx_pad[c], level);
    }
    at(f, origin, p, count * 2);
    gpio(f, driver, !bad_stop);
    at(f, origin, p, count * 2 + 1);
    sample(f, c, rx_pad[c], !bad_stop);
    at(f, origin, p, count * 2 + (stops == 2 ? 3 : stops == 3 ? 4 : 2));
    gpio(f, driver, true);
    step(f, 1000);
}

/* Exact TX edge and native RX sample deadlines, independent rational oracle. */
static void transmitted(Fixture *f, unsigned c, uint8_t byte, unsigned width,
                        int parity, unsigned stops, Period p)
{
    unsigned receiver = destination[c];
    unsigned bits = 1 + width + (parity >= 0);
    unsigned stop_halves = stops == 2 ? 3 : stops == 3 ? 4 : 2;
    unsigned end = bits * 2 + stop_halves;
    uint64_t origin = f->now;
    g_autofree char *log = NULL;
    GError *error = NULL;

    route_rx(f, receiver);
    wr(f, c, FIFO, byte);
    g_assert_cmpuint(tx_used(f, c), ==, 0); /* byte is in the shifter */
    g_assert_cmphex(rd(f, c, FSM) & 0x10, ==, 0x10);
    g_assert_cmphex(rd(f, c, RAW) & TX_DONE, ==, 0);
    for (unsigned i = 0; i < bits; i++) {
        bool expected = i ? (i <= width ? !!(byte & BIT(i - 1)) :
                            parity_bit(byte, width, parity)) : false;
        at(f, origin, p, 2 * i);
        sample(f, c, rx_pad[receiver], expected);
        at(f, origin, p, 2 * i + 1);
        sample(f, receiver, rx_pad[receiver], expected);
        g_assert_cmpuint(rx_used(f, receiver), ==, 0);
    }
    at(f, origin, p, 2 * bits);
    sample(f, c, rx_pad[receiver], true);
    step(f, origin + duration(p, end - 1) - f->now - 1);
    g_assert_cmpuint(rx_used(f, receiver), ==, 0);
    step(f, 1);
    g_assert_cmpuint(rx_used(f, receiver), ==, 1);
    g_assert_cmphex(rd(f, receiver, FIFO), ==, byte & ((1U << width) - 1));
    g_assert_cmphex(rd(f, receiver, RAW) & (PARITY_ERR | FRAME_ERR), ==, 0);
    step(f, origin + duration(p, end) - f->now - 1);
    g_assert_cmphex(rd(f, c, RAW) & TX_DONE, ==, 0);
    step(f, 1);
    g_assert_cmphex(rd(f, c, FSM), ==, 0);
    g_assert_cmphex(rd(f, c, RAW) & TX_DONE, ==, TX_DONE);
    g_assert_true(g_file_get_contents(f->log, &log, NULL, &error));
    g_assert_no_error(error);
    for (unsigned i = 0; i <= bits; i++) {
        bool level = i == bits ? true : i ? (i <= width ?
                     !!(byte & BIT(i - 1)) : parity_bit(byte, width, parity)) : false;
        char suffix[64];

        g_snprintf(suffix, sizeof(suffix), " level=%u", level);
        trace_at(log, c, "tx-drive", origin + duration(p, 2 * i), suffix);
        g_snprintf(suffix, sizeof(suffix), " bit=%u level=%u", i, level);
        trace_at(log, receiver, "rx-sample", origin + duration(p, 2 * i + 1), suffix);
    }
    if (stop_halves > 2) {
        char suffix[64];
        g_snprintf(suffix, sizeof(suffix), " bit=%u level=1", bits + 1);
        trace_at(log, receiver, "rx-sample", origin + duration(p, end - 1), suffix);
    }
}

static void test_formats(gconstpointer opaque)
{
    unsigned c = GPOINTER_TO_UINT(opaque);
    Fixture f = setup(c);
    unsigned receiver = destination[c];
    for (unsigned width = 5; width <= 8; width++) {
        for (int parity = -1; parity <= 1; parity++) {
            for (unsigned stops = 1; stops <= 3; stops++) {
                reset_fifo(&f, c);
                reset_fifo(&f, receiver);
                format(&f, c, width, parity, stops, 0);
                format(&f, receiver, width, parity, stops, 0);
                transmitted(&f, c, 0xa5, width, parity, stops, (Period){1000, 1});
            }
        }
    }
    teardown(&f);
}

static void test_clock_sources(gconstpointer opaque)
{
    /* Fractional rows qualify native rational-model semantics only.
     * Primary TRM's B/A formula conflicts with pinned register comments A/B;
     * target source-fraction encoding remains unqualified. UART FRAG hardware
     * uses a nonuniform16-pulse interleave; this oracle tests average periods,
     * not silicon cycle jitter. Integer NUM+1/source rows are separate. */
    static const struct {
        unsigned source, integer, fraction, div, a, b;
        Period p;
    } vectors[] = {
        {1, 40, 0, 1, 0, 0, {1000, 1}},   /* Explicit APB 40 MHz */
        {2, 35, 0, 1, 0, 0, {2000, 1}},   /* RC_FAST 17.5 MHz */
        {3, 40, 0, 1, 0, 0, {1000, 1}},   /* XTAL 40 MHz */
        {3, 40, 8, 1, 0, 0, {2025, 2}},   /* Model average UART FRAG period */
        {3, 40, 0, 1, 1, 3, {4000, 3}},   /* Model A/B interpretation only */
        {3, 20, 0, 2, 0, 0, {1000, 1}},   /* source integer divider */
    };
    unsigned c = GPOINTER_TO_UINT(opaque);
    Fixture f = setup(c);
    unsigned receiver = destination[c];

    for (unsigned i = 0; i < G_N_ELEMENTS(vectors); i++) {
        if (i == 3 || i == 4) {
            g_test_message("Fraction vector%u: model-semantic-only; "
                           "target encoding/cycle jitter not qualified", i);
        }
        clock_config(&f, c, vectors[i].source, vectors[i].integer,
                     vectors[i].fraction, vectors[i].div, vectors[i].a, vectors[i].b);
        clock_config(&f, receiver, vectors[i].source, vectors[i].integer,
                     vectors[i].fraction, vectors[i].div, vectors[i].a, vectors[i].b);
        transmitted(&f, c, 0x96, 8, -1, 1, vectors[i].p);
    }
    teardown(&f);
}

static void test_isolation(gconstpointer opaque)
{
    unsigned c = GPOINTER_TO_UINT(opaque);
    Fixture f = setup(c);
    unsigned receiver = destination[c];

    for (unsigned i = 0; i < 3; i++) {
        route_rx(&f, i);
    }
    wr(&f, c, FIFO, 0x31 + c);
    step(&f, 10000);
    for (unsigned i = 0; i < 3; i++) {
        g_assert_cmpuint(rx_used(&f, i), ==, i == receiver ? 1 : 0);
    }
    g_assert_cmphex(rd(&f, receiver, FIFO), ==, 0x31 + c);
    teardown(&f);
}

static void test_rx_errors(gconstpointer opaque)
{
    unsigned c = GPOINTER_TO_UINT(opaque);
    Fixture f = setup(c);
    uint32_t errors = PARITY_ERR | FRAME_ERR | BREAK;

    format(&f, c, 8, 0, 1, 0);
    wr(&f, c, ENA, errors);
    frame(&f, c, 0xa5, 8, 0, 1, true, false);
    g_assert_cmphex(rd(&f, c, RAW) & errors, ==, PARITY_ERR);
    g_assert_cmphex(rd(&f, c, ST), ==, PARITY_ERR);
    g_assert_true(qtest_get_irq(f.q, 0));
    g_assert_cmphex(rd(&f, c, FIFO), ==, 0xa5);
    wr(&f, c, CLR, PARITY_ERR);
    g_assert_false(qtest_get_irq(f.q, 0));
    reset_fifo(&f, c);
    format(&f, c, 8, -1, 1, 0);
    frame(&f, c, 0x5a, 8, -1, 1, false, true);
    g_assert_cmphex(rd(&f, c, RAW) & errors, ==, FRAME_ERR);
    g_assert_cmphex(rd(&f, c, FIFO), ==, 0x5a);
    reset_fifo(&f, c);
    frame(&f, c, 0, 8, -1, 1, false, true);
    g_assert_cmphex(rd(&f, c, RAW) & errors, ==, FRAME_ERR | BREAK);
    reset_fifo(&f, c);
    format(&f, c, 8, 0, 1, BIT(26)); /* drop errored frames */
    frame(&f, c, 0x33, 8, 0, 1, true, false);
    g_assert_cmpuint(rx_used(&f, c), ==, 0);
    g_assert_cmphex(rd(&f, c, RAW) & PARITY_ERR, ==, PARITY_ERR);
    teardown(&f);
}

static void test_threshold_timeout(gconstpointer opaque)
{
    unsigned c = GPOINTER_TO_UINT(opaque);
    Fixture f = setup(c);

    wr(&f, c, CONF1, 2 | BIT(23));
    wr(&f, c, MEM_CONF, MEM_128 | (20U << 17));
    wr(&f, c, ENA, RX_FULL | TIMEOUT);
    frame(&f, c, 0x11, 8, -1, 1, false, false);
    g_assert_cmphex(rd(&f, c, RAW) & (RX_FULL | TIMEOUT), ==, 0);
    frame(&f, c, 0x22, 8, -1, 1, false, false);
    g_assert_cmphex(rd(&f, c, ST), ==, RX_FULL);
    g_assert_true(qtest_get_irq(f.q, 0));
    /* Completion sample was 1.5 us before frame() returns. */
    step(&f, 18499);
    g_assert_cmphex(rd(&f, c, RAW) & TIMEOUT, ==, 0);
    step(&f, 1);
    g_assert_cmphex(rd(&f, c, ST), ==, RX_FULL | TIMEOUT);
    g_assert_cmphex(rd(&f, c, FIFO), ==, 0x11);
    g_assert_cmphex(rd(&f, c, ST), ==, TIMEOUT);
    wr(&f, c, CLR, TIMEOUT);
    g_assert_false(qtest_get_irq(f.q, 0));
    g_assert_cmphex(rd(&f, c, FIFO), ==, 0x22);
    step(&f, 100000);
    g_assert_cmphex(rd(&f, c, ST), ==, 0);
    g_assert_false(qtest_get_irq(f.q, 0));
    /* A genuine queued frame arms a timeout; disabling the latched enable
     * must cancel that pending window, not leave a stale IRQ behind. */
    frame(&f, c, 0x33, 8, -1, 1, false, false);
    wr(&f, c, CONF1, 2);
    step(&f, 100000);
    g_assert_cmpuint(rx_used(&f, c), ==, 1);
    g_assert_cmphex(rd(&f, c, ST), ==, 0);
    g_assert_false(qtest_get_irq(f.q, 0));
    wr(&f, c, CONF1, 2 | BIT(23));
    step(&f, 19999);
    g_assert_cmphex(rd(&f, c, RAW) & TIMEOUT, ==, 0);
    step(&f, 1);
    g_assert_cmphex(rd(&f, c, ST), ==, TIMEOUT);
    g_assert_true(qtest_get_irq(f.q, 0));
    g_assert_cmphex(rd(&f, c, FIFO), ==, 0x33);
    wr(&f, c, CLR, TIMEOUT);
    g_assert_false(qtest_get_irq(f.q, 0));
    teardown(&f);
}

static void test_fifo_empty_full(gconstpointer opaque)
{
    unsigned c = GPOINTER_TO_UINT(opaque);
    Fixture f = setup(c);

    g_assert_cmphex(rd(&f, c, FIFO), ==, 0);
    g_assert_cmpuint(rx_used(&f, c), ==, 0);
    wr(&f, c, CONF1, 128);
    for (unsigned i = 0; i < 128; i++) {
        frame(&f, c, i, 8, -1, 1, false, false);
    }
    g_assert_cmpuint(rx_used(&f, c), ==, 128);
    g_assert_cmphex(rd(&f, c, RAW) & RX_FULL, ==, RX_FULL);
    frame(&f, c, 0xff, 8, -1, 1, false, false);
    g_assert_cmpuint(rx_used(&f, c), ==, 128);
    g_assert_cmphex(rd(&f, c, RAW) & OVERFLOW, ==, OVERFLOW);
    for (unsigned i = 0; i < 128; i++) {
        g_assert_cmphex(rd(&f, c, FIFO), ==, i);
    }
    g_assert_cmpuint(rx_used(&f, c), ==, 0);
    g_assert_cmphex(rd(&f, c, FIFO), ==, 0);
    gate(&f, c, false);
    for (unsigned i = 0; i < 129; i++) {
        wr(&f, c, FIFO, i);
    }
    g_assert_cmpuint(tx_used(&f, c), ==, 128);
    g_assert_cmphex(rd(&f, c, RAW) & TX_EMPTY, ==, 0);
    reset_fifo(&f, c);
    g_assert_cmpuint(tx_used(&f, c), ==, 0);
    g_assert_cmphex(rd(&f, c, RAW) & TX_EMPTY, ==, TX_EMPTY);
    teardown(&f);
}

/* Zero SIZE encoding and over-extent allocation lack primary-source hardware
 * qualification. These vectors assert this model's fail-closed memory-safety
 * boundary only; they are not support or silicon-behavior qualification. */
static void test_allocation_bounds(gconstpointer opaque)
{
    unsigned c = GPOINTER_TO_UINT(opaque);
    Fixture f = setup(c);
    unsigned tx_capacity = MIN(7U * 128, 1024U - c * 128);
    unsigned rx_capacity = 512U - c * 128;

    wr(&f, c, MEM_CONF, 0);
    wr(&f, c, FIFO, 0x55);
    g_assert_cmpuint(tx_used(&f, c), ==, 0);
    frame(&f, c, 0x55, 8, -1, 1, false, false);
    g_assert_cmpuint(rx_used(&f, c), ==, 0);
    g_assert_cmphex(rd(&f, c, RAW) & OVERFLOW, ==, OVERFLOW);
    gate(&f, c, false);
    wr(&f, c, MEM_CONF, (7U << 1) | (7U << 4));
    reset_fifo(&f, c);
    for (unsigned i = 0; i <= tx_capacity; i++) {
        wr(&f, c, FIFO, i);
    }
    g_assert_cmpuint(tx_used(&f, c), ==, tx_capacity);
    reset_fifo(&f, c);
    gate(&f, c, true);
    for (unsigned i = 0; i < rx_capacity; i++) {
        frame(&f, c, i, 8, -1, 1, false, false);
    }
    g_assert_cmpuint(rx_used(&f, c), ==, rx_capacity);
    frame(&f, c, 0xff, 8, -1, 1, false, false);
    g_assert_cmpuint(rx_used(&f, c), ==, rx_capacity);
    g_assert_cmphex(rd(&f, c, RAW) & OVERFLOW, ==, OVERFLOW);
    reset_fifo(&f, c);
    g_assert_cmpuint(rx_used(&f, c), ==, 0);
    teardown(&f);
}

static void test_cts(gconstpointer opaque)
{
    unsigned c = GPOINTER_TO_UINT(opaque);
    Fixture f = setup(c);
    unsigned receiver = destination[c];

    qtest_writel(f.q, PAD(12), INPUT);
    qtest_writel(f.q, PAD(13), INPUT);
    peer(&f, 13, true); /* explicit physical net13 -> pad12 */
    qtest_writel(f.q, IN(cts_signal[c]), BIT(7) | 12);
    format(&f, c, 8, -1, 1, BIT(15));
    route_rx(&f, receiver);
    wr(&f, c, FIFO, 0x69);
    step(&f, 50000);
    g_assert_cmpuint(tx_used(&f, c), ==, 1);
    g_assert_cmpuint(rx_used(&f, receiver), ==, 0);
    g_assert_cmphex(rd(&f, c, FSM) & 0x10, ==, 0);
    sample(&f, c, rx_pad[receiver], true);
    gpio(&f, 13, false);
    g_assert_cmpuint(tx_used(&f, c), ==, 0);
    step(&f, 10000);
    g_assert_cmphex(rd(&f, receiver, FIFO), ==, 0x69);
    /* CTS inhibits only a new frame, not one already in the shifter. */
    wr(&f, c, FIFO, 0x96);
    step(&f, 1000);
    gpio(&f, 13, true);
    wr(&f, c, FIFO, 0x55);
    step(&f, 9000);
    g_assert_cmphex(rd(&f, receiver, FIFO), ==, 0x96);
    g_assert_cmpuint(tx_used(&f, c), ==, 1);
    gpio(&f, 13, false);
    step(&f, 10000);
    g_assert_cmphex(rd(&f, receiver, FIFO), ==, 0x55);
    teardown(&f);
}

static void test_rts(gconstpointer opaque)
{
    unsigned c = GPOINTER_TO_UINT(opaque);
    Fixture f = setup(c);
    unsigned output = c == 2 ? 13 : 11;
    unsigned input = c == 2 ? 12 : 14;
    unsigned rts = cts_signal[c];

    qtest_writel(f.q, PAD(output), INPUT);
    qtest_writel(f.q, PAD(input), INPUT);
    qtest_writel(f.q, OUT(output), rts);
    qtest_writel(f.q, GPIO(A_GPIO_ENABLE_W1TS), BIT(output));
    wr(&f, c, MEM_CONF, MEM_128 | (2U << 7));
    wr(&f, c, CONF1, 1 | BIT(22));
    sample(&f, c, input, false);
    frame(&f, c, 0x11, 8, -1, 1, false, false);
    sample(&f, c, input, false);
    frame(&f, c, 0x22, 8, -1, 1, false, false);
    sample(&f, c, input, true);
    g_assert_cmphex(rd(&f, c, FIFO), ==, 0x11);
    sample(&f, c, input, false);
    if (!c) {
        /* RX_FLOW_THRHD is ten bits: 512 must not alias threshold zero.
         * Four RX blocks physically fit UART0's upper SRAM window. */
        reset_fifo(&f, c);
        wr(&f, c, MEM_CONF, (4U << 1) | (1U << 4) | (512U << 7));
        sample(&f, c, input, false);
        for (unsigned i = 0; i < 511; i++) {
            frame(&f, c, i, 8, -1, 1, false, false);
        }
        g_assert_cmpuint(rx_used(&f, c), ==, 511);
        sample(&f, c, input, false);
        frame(&f, c, 0x5a, 8, -1, 1, false, false);
        g_assert_cmpuint(rx_used(&f, c), ==, 512);
        sample(&f, c, input, true);
    }
    teardown(&f);
}

static void test_gate_reset(gconstpointer opaque)
{
    unsigned c = GPOINTER_TO_UINT(opaque);
    Fixture f = setup(c);

    /* Gated queued bytes cannot enter the shifter. RX is kept on known idle;
     * this vector observes the native TX FSM, not a disconnected RX frame. */
    gate(&f, c, false);
    wr(&f, c, FIFO, 0x55);
    step(&f, 100000);
    g_assert_cmpuint(tx_used(&f, c), ==, 1);
    g_assert_cmphex(rd(&f, c, FSM), ==, 0);
    gate(&f, c, true);
    step(&f, 250);
    gate(&f, c, false);
    step(&f, 100000);
    g_assert_cmphex(rd(&f, c, FSM) & 0x10, ==, 0x10);
    g_assert_cmphex(rd(&f, c, RAW) & TX_DONE, ==, 0);
    gate(&f, c, true);
    step(&f, 9749);
    g_assert_cmphex(rd(&f, c, RAW) & TX_DONE, ==, 0);
    step(&f, 1);
    g_assert_cmphex(rd(&f, c, RAW) & TX_DONE, ==, TX_DONE);
    wr(&f, c, FIFO, 0x12);
    step(&f, 250);
    qtest_writel(f.q, RESET(c), qtest_readl(f.q, RESET(c)) | GATE(c));
    g_assert_cmphex(rd(&f, c, DIV), ==, 694);
    g_assert_cmpuint(tx_used(&f, c), ==, 0);
    g_assert_cmpuint(rx_used(&f, c), ==, 0);
    g_assert_cmphex(rd(&f, c, FSM), ==, 0);
    step(&f, 100000);
    g_assert_cmphex(rd(&f, c, FSM), ==, 0);
    qtest_writel(f.q, RESET(c), qtest_readl(f.q, RESET(c)) & ~GATE(c));
    clock_config(&f, c, 3, 40, 0, 1, 0, 0);
    wr(&f, c, IDLE_CONF, 256);
    wr(&f, c, FIFO, 0x34);
    step(&f, 10000);
    g_assert_cmphex(rd(&f, c, RAW) & TX_DONE, ==, TX_DONE);
    teardown(&f);
}

static void test_tx_done_clear(gconstpointer opaque)
{
    unsigned c = GPOINTER_TO_UINT(opaque);
    Fixture f = setup(c);

    wr(&f, c, CLR, TX_DONE);
    wr(&f, c, ENA, TX_DONE);
    g_assert_cmphex(rd(&f, c, RAW) & TX_DONE, ==, 0);
    wr(&f, c, FIFO, 0xa5);
    step(&f, 10000);
    g_assert_true(qtest_get_irq(f.q, 0));
    wr(&f, c, CLR, TX_DONE);
    g_assert_cmphex(rd(&f, c, RAW) & TX_DONE, ==, 0);
    g_assert_false(qtest_get_irq(f.q, 0));
    /* Unrelated configuration writes and idle time cannot regenerate a
     * completion; a second real transmission must assert it again. */
    wr(&f, c, CONF1, rd(&f, c, CONF1));
    step(&f, 2000);
    g_assert_cmphex(rd(&f, c, RAW) & TX_DONE, ==, 0);
    wr(&f, c, FIFO, 0x5a);
    step(&f, 10000);
    g_assert_cmphex(rd(&f, c, ST), ==, TX_DONE);
    g_assert_true(qtest_get_irq(f.q, 0));
    teardown(&f);
}

static void test_tx_idle(gconstpointer opaque)
{
    unsigned c = GPOINTER_TO_UINT(opaque);
    Fixture f = setup(c);
    unsigned receiver = destination[c];

    wr(&f, c, IDLE_CONF, 256 | (4U << 10));
    wr(&f, c, ENA, TX_DONE);
    route_rx(&f, receiver);
    wr(&f, c, FIFO, 0xa5);
    g_assert_false(qtest_get_irq(f.q, 0));
    step(&f, 10000);
    g_assert_cmpuint(tx_used(&f, c), ==, 0);
    g_assert_cmphex(rd(&f, c, FSM) & 0x10, ==, 0x10);
    g_assert_cmphex(rd(&f, c, RAW) & TX_DONE, ==, 0);
    g_assert_cmphex(rd(&f, receiver, FIFO), ==, 0xa5);
    sample(&f, c, rx_pad[receiver], true);
    step(&f, 1000);
    gate(&f, c, false);
    step(&f, 100000);
    g_assert_cmphex(rd(&f, c, FSM) & 0x10, ==, 0x10);
    g_assert_false(qtest_get_irq(f.q, 0));
    gate(&f, c, true);
    step(&f, 2999);
    g_assert_cmphex(rd(&f, c, RAW) & TX_DONE, ==, 0);
    step(&f, 1);
    g_assert_cmphex(rd(&f, c, FSM), ==, 0);
    g_assert_cmphex(rd(&f, c, ST), ==, TX_DONE);
    g_assert_true(qtest_get_irq(f.q, 0));
    sample(&f, c, rx_pad[receiver], true);
    teardown(&f);
}

static void test_local_clock_update(gconstpointer opaque)
{
    unsigned c = GPOINTER_TO_UINT(opaque);
    Fixture f = setup(c);

    wr(&f, c, UPDATE, 0); /* APB shadow writes do not affect the core yet. */
    wr(&f, c, DIV, 80);
    wr(&f, c, CLK, (1U << 20) | CLOCK_ON); /* Pending APB source. */
    format(&f, c, 5, -1, 1, 0); /* Pending format5N1. */
    gate(&f, c, false);
    gate(&f, c, true);
    /* APB becomes XTAL/2=20MHz, but live UART remains latched XTAL8N1. */
    qtest_writel(f.q, SYSTEM_SYSCLK, 1);
    wr(&f, c, FIFO, 0x55);
    step(&f, 9999);
    g_assert_cmphex(rd(&f, c, RAW) & TX_DONE, ==, 0);
    step(&f, 1);
    g_assert_cmphex(rd(&f, c, RAW) & TX_DONE, ==, TX_DONE);
    wr(&f, c, UPDATE, BIT(31));
    g_assert_cmphex(rd(&f, c, UPDATE) & BIT(31), ==, 0);
    wr(&f, c, FIFO, 0x55);
    /* After UPDATE: APB20MHz/div80=4us, format5N1=seven bits=28us. */
    step(&f, 27999);
    g_assert_cmphex(rd(&f, c, RAW) & TX_DONE, ==, 0);
    step(&f, 1);
    g_assert_cmphex(rd(&f, c, RAW) & TX_DONE, ==, TX_DONE);
    wr(&f, c, UPDATE, BIT(30));
    wr(&f, c, CLK, (3U << 20) | BIT(22) | BIT(25)); /* local TX gate */
    wr(&f, c, FIFO, 0x44);
    step(&f, 100000);
    g_assert_cmpuint(tx_used(&f, c), ==, 1);
    wr(&f, c, CLK, (3U << 20) | CLOCK_ON | BIT(26)); /* local TX reset */
    g_assert_cmpuint(tx_used(&f, c), ==, 0);
    g_assert_cmphex(rd(&f, c, FSM), ==, 0);
    teardown(&f);
}

static void test_autobaud(gconstpointer opaque)
{
    unsigned c = GPOINTER_TO_UINT(opaque);
    Fixture f = setup(c);

    peer(&f, peer_pad[c], true);
    route_rx(&f, c);
    format(&f, c, 8, -1, 1, BIT(27));
    step(&f, 1000);
    for (unsigned i = 0; i < 8; i++) {
        gpio(&f, peer_pad[c], !!(i & 1));
        step(&f, 1000);
    }
    /* Primary autobaud equations encode elapsed reference cycles minus one. */
    g_assert_cmpuint(rd(&f, c, LOW), ==, 39);
    g_assert_cmpuint(rd(&f, c, HIGH), ==, 39);
    g_assert_cmpuint(rd(&f, c, EDGES), ==, 8);
    g_assert_cmpuint(rd(&f, c, POS_EDGE), ==, 79);
    g_assert_cmpuint(rd(&f, c, NEG_EDGE), ==, 79);
    /* Read-only measurement registers cannot be fabricated by MMIO writes. */
    wr(&f, c, LOW, 1);
    wr(&f, c, EDGES, 1023);
    g_assert_cmpuint(rd(&f, c, LOW), ==, 39);
    g_assert_cmpuint(rd(&f, c, EDGES), ==, 8);
    teardown(&f);
}

static void test_at_guards(gconstpointer opaque)
{
    unsigned c = GPOINTER_TO_UINT(opaque);
    Fixture f = setup(c);

    wr(&f, c, AT_PRE, 4);
    wr(&f, c, AT_POST, 8);
    wr(&f, c, AT_GAP, 4);
    wr(&f, c, AT_CHAR, '+' | (3U << 8));
    peer(&f, peer_pad[c], true);
    route_rx(&f, c);
    step(&f, 4000);
    frame(&f, c, '+', 8, -1, 1, false, false);
    frame(&f, c, '+', 8, -1, 1, false, false);
    frame(&f, c, '+', 8, -1, 1, false, false);
    g_assert_cmphex(rd(&f, c, RAW) & AT_DONE, ==, 0);
    step(&f, 6499);
    g_assert_cmphex(rd(&f, c, RAW) & AT_DONE, ==, 0);
    step(&f, 1);
    g_assert_cmphex(rd(&f, c, RAW) & AT_DONE, ==, AT_DONE);
    reset_fifo(&f, c);
    step(&f, 4000);
    frame(&f, c, '+', 8, -1, 1, false, false);
    step(&f, 5000); /* too long between AT characters */
    frame(&f, c, '+', 8, -1, 1, false, false);
    frame(&f, c, '+', 8, -1, 1, false, false);
    step(&f, 10000);
    g_assert_cmphex(rd(&f, c, RAW) & AT_DONE, ==, 0);
    teardown(&f);
}

static void test_rs485(gconstpointer opaque)
{
    unsigned c = GPOINTER_TO_UINT(opaque);
    Fixture f = setup(c);

    /* TRM 26.17: bit0 RS485_EN, bit3 RS485TX_RX_EN (receiver works while
     * transmitting), bit4 RS485RXBY_TX_EN (transmitter sends while the
     * receiver line is busy). The mirror rows need all three. */
    wr(&f, c, RS485, BIT(0) | BIT(3) | BIT(4));
    format(&f, c, 8, 0, 1, 0);
    frame(&f, c, 0x5a, 8, 0, 1, true, false);
    g_assert_cmphex(rd(&f, c, RAW) & (PARITY_ERR | RS_PARITY), ==, PARITY_ERR);
    reset_fifo(&f, c);
    if (!c) {
        /* GPIO4 remains the genuine peer on RX5; move UART0's actual TX
         * to explicit pad10's finite net for independent RX error stimulus. */
        qtest_writel(f.q, PAD(10), INPUT);
        qtest_writel(f.q, OUT(10), matrix_signal[c]);
        qtest_writel(f.q, GPIO(A_GPIO_ENABLE_W1TS), BIT(10));
    }
    /* Actual TX frames keep RS485 TX mode active through the physical peer
     * frame's parity/stop sample. RS error IRQs are TX-mode-only. Three queued
     * bytes cover the peer frame exactly: two bytes end 500 ns before its stop
     * sample at this 10-bit framing, which would test the window boundary
     * rather than the mirror status. */
    wr(&f, c, FIFO, 0x00);
    wr(&f, c, FIFO, 0x00);
    wr(&f, c, FIFO, 0x00);
    frame(&f, c, 0x5a, 8, 0, 1, true, false);
    g_assert_cmphex(rd(&f, c, RAW) & (PARITY_ERR | RS_PARITY), ==,
                    PARITY_ERR | RS_PARITY);
    step(&f, 30000);
    reset_fifo(&f, c);
    format(&f, c, 8, -1, 1, 0);
    wr(&f, c, FIFO, 0x00);
    wr(&f, c, FIFO, 0x00);
    wr(&f, c, FIFO, 0x00);
    frame(&f, c, 0x5a, 8, -1, 1, false, true);
    g_assert_cmphex(rd(&f, c, RAW) & (FRAME_ERR | RS_FRAME), ==,
                    FRAME_ERR | RS_FRAME);
    step(&f, 30000);
    reset_fifo(&f, c);
    if (!c) {
        qtest_writel(f.q, OUT(10), GPIO_FUNC_OUT_SEL_NONE);
        qtest_writel(f.q, GPIO(A_GPIO_ENABLE_W1TC), BIT(10));
    }
    /* Actual physical TX pad echo, not CONF0 internal loopback. */
    qtest_writel(f.q, OUT(tx_pad[c]), matrix_signal[c]);
    qtest_writel(f.q, IN(matrix_signal[c]), BIT(7) | tx_pad[c]);
    wr(&f, c, FIFO, 0xa5);
    step(&f, 10000);
    g_assert_cmpuint(rx_used(&f, c), ==, 1);
    g_assert_cmphex(rd(&f, c, FIFO), ==, 0xa5);
    reset_fifo(&f, c);
    wr(&f, c, RS485, BIT(0)); /* RX FSM disabled during own TX. */
    wr(&f, c, FIFO, 0x96);
    step(&f, 10000);
    g_assert_cmpuint(rx_used(&f, c), ==, 0);
    g_assert_cmphex(rd(&f, c, RAW) & (PARITY_ERR | FRAME_ERR | BREAK), ==, 0);
    wr(&f, c, RS485, BIT(0) | BIT(3));
    peer(&f, peer_pad[c], true);
    qtest_writel(f.q, IN(matrix_signal[c]), BIT(7) | (c ? rx_pad[c] : 21));
    if (!c) {
        qtest_writel(f.q, OUT(tx_pad[c]), matrix_signal[c]);
    }
    wr(&f, c, FIFO, 0x00);
    if (c) {
        gpio(&f, peer_pad[c], false);
        gpio(&f, peer_pad[c], true);
    }
    step(&f, 1000);
    g_assert_cmphex(rd(&f, c, RAW) & RS_CLASH, ==, RS_CLASH);
    step(&f, 9000);
    teardown(&f);
}

static void test_rs485_delays_dtr(gconstpointer opaque)
{
    unsigned c = GPOINTER_TO_UINT(opaque);
    Fixture f = setup(c);
    unsigned receiver = c ? destination[c] : 1;
    unsigned rx = c ? rx_pad[receiver] : 5;
    unsigned output = c == 2 ? 13 : 11;
    unsigned input = c == 2 ? 12 : 14;
    static const unsigned dtr_signal[3] = {14, 17, 20};
    uint64_t origin;
    g_autofree char *log = NULL;
    GError *error = NULL;

    /* Observe actual GPIO DTR, not a differential transceiver substitute. */
    wr(&f, c, RS485, BIT(0) | BIT(1) | BIT(2));
    qtest_writel(f.q, PAD(output), INPUT);
    qtest_writel(f.q, PAD(input), INPUT);
    qtest_writel(f.q, OUT(output), dtr_signal[c]);
    qtest_writel(f.q, GPIO(A_GPIO_ENABLE_W1TS), BIT(output));
    qtest_writel(f.q, IN(matrix_signal[receiver]), BIT(7) | rx);
    wr(&f, c, ENA, TX_DONE);
    sample(&f, c, input, false);
    origin = f.now;
    wr(&f, c, FIFO, 0xa5);
    g_assert_cmpuint(tx_used(&f, c), ==, 0); /* Persistent PRE-phase shifter. */
    g_assert_cmphex(rd(&f, c, FSM) & 0x10, ==, 0x10);
    g_assert_cmphex(rd(&f, c, STATUS) & BIT(29), ==, BIT(29));
    sample(&f, c, input, true);
    sample(&f, c, rx, true);
    step(&f, 999);
    sample(&f, c, rx, true);
    g_assert_cmpuint(rx_used(&f, receiver), ==, 0);
    step(&f, 1); /* DL0 one-bit PRE phase ends at1000ns. */
    sample(&f, c, rx, false);
    step(&f, 9499);
    g_assert_cmpuint(rx_used(&f, receiver), ==, 0);
    step(&f, 1);
    g_assert_cmphex(rd(&f, receiver, FIFO), ==, 0xa5);
    step(&f, 500); /* Stop ends11000ns; DL1 POST phase still owns DTR. */
    sample(&f, c, input, true);
    sample(&f, c, rx, true);
    g_assert_cmphex(rd(&f, c, RAW) & TX_DONE, ==, 0);
    g_assert_false(qtest_get_irq(f.q, 0));
    step(&f, 999);
    sample(&f, c, input, true);
    step(&f, 1);
    sample(&f, c, input, false);
    g_assert_cmphex(rd(&f, c, STATUS) & BIT(29), ==, 0);
    g_assert_cmphex(rd(&f, c, FSM), ==, 0);
    g_assert_cmphex(rd(&f, c, ST), ==, TX_DONE);
    g_assert_true(qtest_get_irq(f.q, 0));
    g_assert_true(g_file_get_contents(f.log, &log, NULL, &error));
    g_assert_no_error(error);
    trace_at(log, c, "tx-drive", origin + 1000, " level=0");
    trace_at(log, receiver, "rx-sample", origin + 1500, " bit=0 level=0");
    teardown(&f);
}

static void test_irda(gconstpointer opaque)
{
    unsigned c = GPOINTER_TO_UINT(opaque);
    Fixture f = setup(c);
    /* UART0's explicit physical4/5 wire feeds a different RX-only controller;
     * SIR is half-duplex, so UART0 cannot transmit and receive simultaneously. */
    unsigned receiver = c ? destination[c] : 1;
    unsigned observed = c ? rx_pad[receiver] : 5;
    const uint8_t byte = 0xa5;
    /* Dedicated SIR rate100 kbit/s lies within TRM's115.2 kbit/s limit. */
    clock_config(&f, c, 3, 400, 0, 1, 0, 0);
    clock_config(&f, receiver, 3, 400, 0, 1, 0, 0);

    for (unsigned wctl = 0; wctl <= 1; wctl++) {
        uint64_t origin;
        unsigned pulse_end = wctl ? 6875 : 6250;
        g_autofree char *log = NULL;
        GError *error = NULL;

        format(&f, c, 8, -1, 1, BIT(16) | BIT(10) | (wctl ? BIT(11) : 0));
        wr(&f, receiver, CLK, (3U << 20) | (CLOCK_ON & ~BIT(25)));
        format(&f, receiver, 8, -1, 1, BIT(16) | BIT(13));
        qtest_writel(f.q, IN(matrix_signal[receiver]), BIT(7) | observed);
        wr(&f, receiver, CLK, (3U << 20) | CLOCK_ON);
        step(&f, 10000);
        sample(&f, c, observed, false);
        wr(&f, c, FIFO, byte);
        origin = f.now;
        for (unsigned i = 0; i < 9; i++) {
            bool pulse = i == 0 || !(byte & BIT(i - 1));
            step(&f, origin + i * 10000 - f.now);
            sample(&f, c, observed, false);
            step(&f, 4999);
            sample(&f, c, observed, false);
            step(&f, 1); /* Ninth oversampling cycle: tick8. */
            sample(&f, c, observed, pulse);
            step(&f, pulse_end - 5001);
            sample(&f, c, observed, pulse);
            step(&f, 1); /* WCTL0 tick10, WCTL1 tick11. */
            sample(&f, c, observed, false);
            step(&f, origin + (i + 1) * 10000 - f.now);
            sample(&f, c, observed, false);
        }
        step(&f, origin + 99999 - f.now);
        g_assert_cmphex(rd(&f, c, RAW) & TX_DONE, ==, 0);
        step(&f, 1);
        g_assert_cmphex(rd(&f, c, RAW) & TX_DONE, ==, TX_DONE);
        g_assert_cmpuint(rx_used(&f, receiver), ==, 1);
        g_assert_cmphex(rd(&f, receiver, FIFO), ==, byte);
        g_assert_cmphex(rd(&f, receiver, RAW) & (PARITY_ERR | FRAME_ERR), ==, 0);
        g_assert_true(g_file_get_contents(f.log, &log, NULL, &error));
        g_assert_no_error(error);
        trace_at(log, c, "tx-drive", origin + 5000, " level=1");
        trace_at(log, c, "tx-drive", origin + pulse_end, " level=0");
    }
    teardown(&f);

    /* Independent RX-only proof: real GPIO peer emits active-LOW SIR pulses
     * at tick8 on the canonical physical wire, with default RX inversion0. */
    f = setup(c);
    clock_config(&f, c, 3, 400, 0, 1, 0, 0);
    peer(&f, peer_pad[c], true);
    wr(&f, c, CLK, (3U << 20) | (CLOCK_ON & ~BIT(25)));
    format(&f, c, 8, -1, 1, BIT(16));
    route_rx(&f, c);
    wr(&f, c, CLK, (3U << 20) | CLOCK_ON);
    step(&f, 10000);
    uint64_t origin = f.now;
    for (unsigned i = 0; i < 9; i++) {
        bool pulse = i == 0 || !(0x96 & BIT(i - 1));
        step(&f, origin + i * 10000 - f.now);
        sample(&f, c, rx_pad[c], true);
        step(&f, 5000);
        gpio(&f, peer_pad[c], !pulse);
        sample(&f, c, rx_pad[c], !pulse);
        step(&f, 1875);
        gpio(&f, peer_pad[c], true);
        sample(&f, c, rx_pad[c], true);
    }
    step(&f, origin + 110000 - f.now);
    g_assert_cmpuint(rx_used(&f, c), ==, 1);
    g_assert_cmphex(rd(&f, c, FIFO), ==, 0x96);
    g_assert_cmphex(rd(&f, c, RAW) & (PARITY_ERR | FRAME_ERR), ==, 0);
    teardown(&f);
}

static void test_shared_tx_alias(void)
{
    Fixture f = setup(0);

    gate(&f, 0, false);
    gate(&f, 1, false);
    wr(&f, 0, MEM_CONF, (1U << 1) | (2U << 4));
    for (unsigned i = 0; i < 129; i++) {
        wr(&f, 0, FIFO, 0x40 + (i & 15));
    }
    wr(&f, 1, FIFO, 0xd3); /* UART1 TX base128 aliases expanded UART0. */
    g_assert_cmpuint(tx_used(&f, 0), ==, 129);
    g_assert_cmpuint(tx_used(&f, 1), ==, 1);
    g_assert_cmpuint(rd(&f, 1, TX_PTR) & 1023, ==, 129);
    route_rx(&f, 0);
    gate(&f, 0, true);
    for (unsigned i = 0; i < 129; i++) {
        step(&f, 10000);
        g_assert_cmphex(rd(&f, 0, FIFO), ==, i == 128 ? 0xd3 : 0x40 + (i & 15));
    }
    g_assert_cmpuint(tx_used(&f, 0), ==, 0);
    g_assert_cmpuint(tx_used(&f, 1), ==, 1); /* alias bytes, not queue counts */
    teardown(&f);
}

static void test_shared_rx_alias(void)
{
    Fixture f = setup(0);

    wr(&f, 0, MEM_CONF, (2U << 1) | (1U << 4));
    for (unsigned i = 0; i < 129; i++) {
        frame(&f, 0, 0x40 + (i & 15), 8, -1, 1, false, false);
    }
    frame(&f, 1, 0xd3, 8, -1, 1, false, false);
    g_assert_cmpuint(rx_used(&f, 0), ==, 129);
    g_assert_cmpuint(rx_used(&f, 1), ==, 1);
    g_assert_cmpuint(rd(&f, 1, RX_PTR) & 1023, ==, 640);
    for (unsigned i = 0; i < 129; i++) {
        g_assert_cmphex(rd(&f, 0, FIFO), ==, i == 128 ? 0xd3 : 0x40 + (i & 15));
    }
    g_assert_cmphex(rd(&f, 1, FIFO), ==, 0xd3);
    teardown(&f);
}

static void test_negative_graph(gconstpointer opaque)
{
    Fixture f = setup(1);
    const char *name = opaque;

    graph_live(&f, name);
    route_rx(&f, 1);
    route_rx(&f, 2);
    for (unsigned c = 1; c <= 2; c++) {
        sample(&f, c, rx_pad[c], true);
        wr(&f, c, FIFO, 0xa5);
        sample(&f, c, tx_pad[c], false);
        step(&f, 10000);
        g_assert_cmphex(rd(&f, c, RAW) & TX_DONE, ==, TX_DONE);
        g_assert_cmpuint(rx_used(&f, 1), ==, 0);
        g_assert_cmpuint(rx_used(&f, 2), ==, 0);
        sample(&f, c, rx_pad[c], true);
    }
    teardown(&f);
}

static void uhci_setup(Fixture *f, uint32_t flags)
{
    qtest_writel(f->q, 0x600c0020ULL, qtest_readl(f->q, 0x600c0020ULL) & ~BIT(8));
    qtest_writel(f->q, 0x600c0024ULL, qtest_readl(f->q, 0x600c0024ULL) & ~BIT(6));
    qtest_writel(f->q, 0x600c0018ULL, qtest_readl(f->q, 0x600c0018ULL) | BIT(8));
    qtest_writel(f->q, 0x600c001cULL, qtest_readl(f->q, 0x600c001cULL) | BIT(6));
    qtest_writel(f->q, UHCI(0), BIT(0) | BIT(1) | BIT(3));
    qtest_writel(f->q, UHCI(0x24), 0); /* No escape substitution. */
    qtest_writel(f->q, UHCI(0x18), 0); /* No software-send mode. */
    qtest_writel(f->q, UHCI(0x10), 0x1ff);
    qtest_writel(f->q, UHCI(0), BIT(3) | flags); /* Select actual UART1 only. */
}

static void dma_descriptor(Fixture *f, uint32_t address, uint32_t config,
                           uint32_t buffer, uint32_t next)
{
    /* Guest little-endian words, not host-native descriptor structs. */
    qtest_writel(f->q, address, config);
    qtest_writel(f->q, address + 4, buffer);
    qtest_writel(f->q, address + 8, next);
}

static void dma_arm(Fixture *f, unsigned c, bool out, unsigned peripheral,
                    uint32_t first)
{
    qtest_writel(f->q, GDMA(c, out, D_CONF0), out ? BIT(2) : 0);
    qtest_writel(f->q, GDMA(c, out, D_CONF1), BIT(12)); /* Real owner check. */
    qtest_writel(f->q, GDMA(c, out, D_CLR), UINT32_MAX);
    qtest_writel(f->q, GDMA(c, out, D_PERI), peripheral);
    qtest_writel(f->q, GDMA(c, out, D_LINK),
                 (first & 0xfffff) | BIT(out ? 21 : 22));
    /* QMP round-trip lets the genuine GDMA/UHCI bottom halves run. */
    qtest_qmp_assert_success(f->q, "{'execute':'query-status'}");
}

static void untouched_channels(Fixture *f, unsigned active, bool out)
{
    for (unsigned c = 0; c < 5; c++) {
        if (c != active) {
            g_assert_cmphex(qtest_readl(f->q, GDMA(c, out, D_RAW)), ==, 0);
        }
        g_assert_cmphex(qtest_readl(f->q, GDMA(c, !out, D_RAW)), ==, 0);
    }
}

static void test_uhci_tx_packet(void)
{
    const uint8_t payload[] = {0x31, 0x62};
    const uint8_t wire[] = {0xc0, 0x31, 0x62, 0xc0};
    Fixture f = setup(1);
    const Period p = {1000, 1};
    uint64_t origin = f.now;
    g_autofree char *log = NULL;
    GError *error = NULL;

    uhci_setup(&f, BIT(5)); /* Programmed physical separator framing. */
    qtest_writel(f.q, UHCI(0x70), 0xc0);
    route_rx(&f, 2);
    qtest_memwrite(f.q, D_BUFFER, payload, sizeof(payload));
    dma_descriptor(&f, D_DESC, D_OWNER | D_EOF | 2 | (2U << 12), D_BUFFER, 0);
    dma_arm(&f, 3, true, D_UART_PERI, D_DESC);
    for (unsigned n = 0; n < sizeof(wire); n++) {
        uint64_t frame_origin = origin + n * 10000;
        for (unsigned bit = 0; bit < 9; bit++) {
            bool level = bit ? !!(wire[n] & BIT(bit - 1)) : false;
            at(&f, frame_origin, p, bit * 2);
            sample(&f, 1, rx_pad[2], level);
            at(&f, frame_origin, p, bit * 2 + 1);
            sample(&f, 2, rx_pad[2], level);
        }
        at(&f, frame_origin, p, 18);
        sample(&f, 1, rx_pad[2], true);
        at(&f, frame_origin, p, 20);
        g_assert_cmpuint(rx_used(&f, 2), ==, 1);
        g_assert_cmphex(rd(&f, 2, FIFO), ==, wire[n]);
    }
    g_assert_cmphex(qtest_readl(f.q, D_DESC) & D_OWNER, ==, 0);
    g_assert_cmphex(qtest_readl(f.q, GDMA(3, true, D_RAW)) &
                    (BIT(0) | BIT(1) | BIT(2) | BIT(3)), ==,
                    BIT(0) | BIT(1) | BIT(3));
    g_assert_cmphex(qtest_readl(f.q, GDMA(3, true, D_EOF_DESC)), ==, D_DESC);
    g_assert_cmphex(rd(&f, 1, RAW) & TX_DONE, ==, TX_DONE);
    g_assert_cmpuint(rx_used(&f, 0), ==, 0);
    g_assert_cmpuint(rx_used(&f, 1), ==, 0);
    g_assert_true(g_file_get_contents(f.log, &log, NULL, &error));
    g_assert_no_error(error);
    for (unsigned n = 0; n < sizeof(wire); n++) {
        trace_at(log, 1, "tx-drive", origin + n * 10000, " level=0");
        trace_at(log, 2, "rx-sample", origin + n * 10000 + 500, " bit=0 level=0");
    }
    untouched_channels(&f, 3, true);
    teardown(&f);
}

static void test_uhci_tx_rejected(gconstpointer opaque)
{
    bool wrong_peripheral = GPOINTER_TO_UINT(opaque);
    Fixture f = setup(1);
    uint32_t config = 1 | (1U << 12) | D_EOF |
                      (wrong_peripheral ? D_OWNER : 0);

    uhci_setup(&f, 0); /* Raw mode: no framing byte may precede owner validation. */
    route_rx(&f, 2);
    qtest_writeb(f.q, D_BUFFER, 0xa5);
    dma_descriptor(&f, D_DESC, config, D_BUFFER, 0);
    dma_arm(&f, 3, true, wrong_peripheral ? 0 : D_UART_PERI, D_DESC);
    step(&f, 50000);
    sample(&f, 1, rx_pad[2], true);
    g_assert_cmpuint(tx_used(&f, 1), ==, 0);
    g_assert_cmphex(rd(&f, 1, FSM), ==, 0);
    for (unsigned c = 0; c < 3; c++) {
        g_assert_cmpuint(tx_used(&f, c), ==, 0);
        g_assert_cmphex(rd(&f, c, FSM), ==, 0);
        g_assert_cmpuint(rx_used(&f, c), ==, 0);
    }
    g_assert_cmphex(qtest_readl(f.q, D_DESC), ==, config);
    g_assert_cmphex(qtest_readl(f.q, GDMA(3, true, D_RAW)), ==,
                    wrong_peripheral ? 0 : BIT(2));
    g_autofree char *log = NULL;
    GError *error = NULL;
    g_assert_true(g_file_get_contents(f.log, &log, NULL, &error));
    g_assert_no_error(error);
    const char *cursor = log;
    while ((cursor = strstr(cursor, "esp32s3-uart1 event=tx-drive "))) {
        const char *end = strchr(cursor, '\n');
        const char *low = strstr(cursor, " level=0");
        g_assert_nonnull(end);
        g_assert_true(!low || low > end);
        cursor = end + 1;
    }
    untouched_channels(&f, 3, true);
    teardown(&f);
}

static void test_uhci_rx_length_packet(void)
{
    const uint8_t expected[] = {0x31, 0x62, 0x93};
    uint8_t stored[sizeof(expected)];
    Fixture f = setup(1);

    uhci_setup(&f, BIT(9));
    qtest_writel(f.q, UHCI(0x80), sizeof(expected));
    qtest_memset(f.q, D_BUFFER, 0xee, 32);
    dma_descriptor(&f, D_DESC, D_OWNER | 32, D_BUFFER, 0);
    dma_arm(&f, 2, false, D_UART_PERI, D_DESC);
    for (unsigned i = 0; i < sizeof(expected); i++) {
        frame(&f, 1, expected[i], 8, -1, 1, false, false);
        if (i + 1 < sizeof(expected)) {
            /* A byte reached RAM, but neither the descriptor nor packet has
             * completed: a DMA ISR must not recycle this buffer yet. */
            g_assert_cmphex(qtest_readl(f.q, GDMA(2, false, D_RAW)) & BIT(0), ==, 0);
            g_assert_cmphex(qtest_readl(f.q, D_DESC) & D_OWNER, ==, D_OWNER);
        }
    }
    qtest_memread(f.q, D_BUFFER, stored, sizeof(stored));
    g_assert_cmpmem(stored, sizeof(stored), expected, sizeof(expected));
    g_assert_cmphex(qtest_readl(f.q, D_DESC) & (D_OWNER | D_EOF | (0xfffU << 12)),
                    ==, D_EOF | (sizeof(expected) << 12));
    g_assert_cmphex(qtest_readl(f.q, GDMA(2, false, D_RAW)), ==, BIT(0) | BIT(1));
    g_assert_cmphex(qtest_readl(f.q, GDMA(2, false, D_EOF_DESC)), ==, D_DESC);
    g_assert_cmpuint(rx_used(&f, 1), ==, 0); /* Same native FIFO actually drained. */
    g_assert_cmpuint(rx_used(&f, 0) + rx_used(&f, 2), ==, 0);
    untouched_channels(&f, 2, false);
    teardown(&f);
}

static void test_uhci_rx_next_owner_error(void)
{
    Fixture f = setup(1);

    uhci_setup(&f, 0);
    qtest_memset(f.q, D_BUFFER, 0xee, 64);
    dma_descriptor(&f, D_DESC, D_OWNER | 1, D_BUFFER, D_NEXT);
    dma_descriptor(&f, D_NEXT, 1, D_BUFFER_NEXT, 0); /* Next belongs to CPU. */
    dma_arm(&f, 4, false, D_UART_PERI, D_DESC);
    frame(&f, 1, 0xa5, 8, -1, 1, false, false);
    g_assert_cmphex(qtest_readb(f.q, D_BUFFER), ==, 0xa5);
    g_assert_cmphex(qtest_readl(f.q, D_DESC) & (D_OWNER | D_EOF | (0xfffU << 12)),
                    ==, 1U << 12);
    g_assert_cmphex(qtest_readl(f.q, D_NEXT), ==, 1);
    g_assert_cmphex(qtest_readb(f.q, D_BUFFER_NEXT), ==, 0xee);
    g_assert_cmphex(qtest_readl(f.q, GDMA(4, false, D_RAW)), ==, BIT(0) | BIT(3));
    g_assert_cmphex(qtest_readl(f.q, GDMA(4, false, D_ERR_DESC)), ==, D_NEXT);
    g_assert_cmpuint(rx_used(&f, 1), ==, 0);
    /* A later genuine frame cannot silently advance through a CPU-owned node. */
    frame(&f, 1, 0x5a, 8, -1, 1, false, false);
    g_assert_cmpuint(rx_used(&f, 1), ==, 1);
    g_assert_cmphex(rd(&f, 1, FIFO), ==, 0x5a);
    g_assert_cmphex(qtest_readb(f.q, D_BUFFER), ==, 0xa5);
    g_assert_cmphex(qtest_readb(f.q, D_BUFFER_NEXT), ==, 0xee);
    g_assert_cmphex(qtest_readl(f.q, GDMA(4, false, D_RAW)), ==, BIT(0) | BIT(3));
    g_assert_cmpuint(rx_used(&f, 0) + rx_used(&f, 2), ==, 0);
    untouched_channels(&f, 4, false);
    teardown(&f);
}

/* When launched through the lane's delegated driver the lane's graph root is
 * exported; in the shared meson qtest suite it is not, and the tests would
 * abort at the first graph application. Skip the whole set instead. */
static void lane_skip(void)
{
    g_test_skip("UART_QTEST_PROJECT_DIR is only set by the lane's delegated driver");
}

int main(int argc, char **argv)
{
    if (!g_getenv("UART_QTEST_PROJECT_DIR")) {
        g_test_init(&argc, &argv, NULL);
        g_test_add_func("/esp32s3/uart/lane-driver-only", lane_skip);
        return g_test_run();
    }
    static const struct {
        const char *name;
        GTestDataFunc function;
    } tests[] = {
        {"formats-physical-timing", test_formats},
        {"clock-sources-fractions", test_clock_sources},
        {"controller-net-isolation", test_isolation},
        {"rx-parity-frame-break-errors", test_rx_errors},
        {"threshold-timeout-irq", test_threshold_timeout},
        {"fifo-empty-full-overflow", test_fifo_empty_full},
        {"physical-fifo-allocation-safety-bounds", test_allocation_bounds},
        {"cts-physical-stall-resume", test_cts},
        {"rts-physical-watermark", test_rts},
        {"system-gate-reset", test_gate_reset},
        {"tx-idle-quantum-gate-resume", test_tx_idle},
        {"tx-done-clear-next-completion", test_tx_done_clear},
        {"local-clock-update-reset", test_local_clock_update},
        {"autobaud-physical-edges", test_autobaud},
        {"at-physical-guard-times", test_at_guards},
        {"rs485-physical-errors-clash", test_rs485},
        {"rs485-pre-post-dtr-physical", test_rs485_delays_dtr},
        {"irda-physical-pulse-sampling", test_irda},
    };

    g_test_init(&argc, &argv, NULL);
    g_assert_nonnull(g_getenv("UART_QTEST_PROJECT_DIR"));
    for (unsigned c = 0; c < 3; c++) {
        for (unsigned i = 0; i < G_N_ELEMENTS(tests); i++) {
            g_autofree char *name = g_strdup_printf("/esp32s3/uart%u/%s", c, tests[i].name);
            qtest_add_data_func(name, GUINT_TO_POINTER(c), tests[i].function);
        }
    }
    qtest_add_func("/esp32s3/uart/shared-tx-physical-fifo-alias", test_shared_tx_alias);
    qtest_add_func("/esp32s3/uart/shared-rx-physical-fifo-alias", test_shared_rx_alias);
    qtest_add_data_func("/esp32s3/uart/disconnected-explicit-nets",
                        "project-disconnected.json", test_negative_graph);
    qtest_add_data_func("/esp32s3/uart/wrong-explicit-nets",
                        "project-wrong.json", test_negative_graph);
    qtest_add_func("/esp32s3/uart/uhci-actual-tx-nonzero-channel-packet",
                    test_uhci_tx_packet);
    qtest_add_data_func("/esp32s3/uart/uhci-actual-tx-wrong-owner",
                        GUINT_TO_POINTER(0), test_uhci_tx_rejected);
    qtest_add_data_func("/esp32s3/uart/uhci-actual-tx-wrong-peripheral",
                        GUINT_TO_POINTER(1), test_uhci_tx_rejected);
    qtest_add_func("/esp32s3/uart/uhci-actual-rx-length-packet",
                    test_uhci_rx_length_packet);
    qtest_add_func("/esp32s3/uart/uhci-actual-rx-next-owner-error",
                    test_uhci_rx_next_owner_error);
    qtest_add_func("/esp32s3/uart1/iomux-tx-release-restore", test_uart1_iomux_tx);
    return g_test_run();
}
