/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Native ESP32-S3 SPI2/3 MMIO consumers. No firmware, mock controller,
 * injected provider, GPIO input stimulus or synthetic receiver acknowledgement.
 * TX DONE proves the hardware shifter completed, not an attached slave ACK.
 * Electrical routing, actual CS levels and transmitted bit values belong to
 * the native graph fixture; these tests deliberately do not claim them.
 */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qemu/bitops.h"

#define SPI_BASE(c) (0x60024000ULL + (c) * 0x1000ULL)
#define SYSTEM_EN0 0x600c0018ULL
#define SYSTEM_RST0 0x600c0020ULL
#define SYSTEM_SPI(c) ((c) ? BIT(16) : BIT(6))
#define SYSTEM_SYSCLK 0x600c0060ULL
#define RTC_OPTIONS0 0x60008000ULL
#define BBPLL_FORCE_PD BIT(10)
#define SYSTEM_GDMA_EN1 0x600c001cULL
#define SYSTEM_GDMA BIT(6)
#define CMD 0x00
#define ADDR 0x04
#define CTRL 0x08
#define CLOCK 0x0c
#define USER 0x10
#define USER1 0x14
#define USER2 0x18
#define DLEN 0x1c
#define MISC 0x20
#define DMA 0x30
#define ENA 0x34
#define CLR 0x38
#define RAW 0x3c
#define ST 0x40
#define SET 0x44
#define W0 0x98
#define SLAVE 0xe0
#define CLK_GATE 0xe8
#define USR BIT(24)
#define UPDATE BIT(23)
#define TX BIT(27)
#define RX BIT(28)
#define SETUP BIT(7)
#define HOLD BIT(6)
#define KEEP_CS BIT(30)
#define DONE BIT(12)
#define TX_EMPTY BIT(18)
#define INT_MASK 0x1fffffU
/* XTAL=40 MHz, pre=1, N=4, H=2: period=100 ns, edges every 50 ns. */
#define CLOCK_10MHZ ((3U << 12) | (1U << 6))

#define GDMA_BASE 0x6003f000ULL
#define GDMA_IN(c, r) (GDMA_BASE + 0xc0 * (c) + (r))
#define GDMA_OUT(c, r) (GDMA_BASE + 0xc0 * (c) + 0x60 + (r))
#define G_CONF0 0x00
#define G_CONF1 0x04
#define G_RAW 0x08
#define G_CLR 0x14
#define G_LINK 0x20
#define G_EOF_DESC 0x28
#define G_DESC 0x30
#define G_BF0 0x34
#define G_BF1 0x38
#define G_PERI 0x48
#define G_AUTO_WRBACK BIT(2)
#define G_CHECK_OWNER BIT(12)
#define G_START BIT(21)
#define G_PARK BIT(23)
#define G_ADDR_MASK 0xfffffU
#define G_DONE BIT(0)
#define G_EOF BIT(1)
#define G_DESC_ERR BIT(2)
#define G_TOTAL_EOF BIT(3)
#define DESC_OWNER BIT(31)
#define DESC_EOF BIT(30)
#define DESC_AREA 0x3fc90000U
#define SOURCE_AREA 0x3fc91000U
#define OTHER_DESC 0x3fc94000U
#define OTHER_SOURCE 0x3fc94100U
#define CHUNK 2048U
#define TX_BYTES (2 * CHUNK + 9)

static uint32_t rd(QTestState *s, unsigned c, unsigned reg)
{
    return qtest_readl(s, SPI_BASE(c) + reg);
}

static void wr(QTestState *s, unsigned c, unsigned reg, uint32_t value)
{
    qtest_writel(s, SPI_BASE(c) + reg, value);
}

/* Qtest CONT starts dummy vCPUs, not firmware, and enables virtual timers.
 * It also makes a genuine running -> strict-paused transition observable.
 * Repeated pause-case setup must not wait for RESUME if already running. */
static void run_for_pause(QTestState *s)
{
    QDict *r = qtest_qmp(s, "{'execute':'query-status'}");
    bool running;

    g_assert_false(qdict_haskey(r, "error"));
    running = qdict_get_bool(qdict_get_qdict(r, "return"), "running");
    qobject_unref(r);
    if (!running) {
        qtest_qmp_assert_success(s, "{'execute':'cont'}");
        qtest_qmp_eventwait(s, "RESUME");
    }
}

static QTestState *setup(unsigned c)
{
    QTestState *s = qtest_init("-machine esp32s3 -S -L pc-bios "
                              "-global driver=esp32s3.gpio,property=strap_mode,value=0x00");
    g_autofree char *path = g_strdup_printf("/machine/soc/spi%u", c + 2);

    qtest_irq_intercept_out_named(s, path, "sysbus-irq");
    qtest_writel(s, SYSTEM_RST0, qtest_readl(s, SYSTEM_RST0) &
                 ~(SYSTEM_SPI(0) | SYSTEM_SPI(1)));
    qtest_writel(s, SYSTEM_EN0, qtest_readl(s, SYSTEM_EN0) |
                 SYSTEM_SPI(0) | SYSTEM_SPI(1));
    for (unsigned i = 0; i < 2; i++) {
        wr(s, i, CLK_GATE, 3); /* Local enable and master-clock enable, XTAL. */
        wr(s, i, CLOCK, CLOCK_10MHZ);
        wr(s, i, USER, TX);
        wr(s, i, USER1, 0);
        wr(s, i, USER2, 0);
        wr(s, i, CTRL, 0);
        wr(s, i, MISC, 0);
        wr(s, i, DMA, 0);
        wr(s, i, ENA, 0);
        wr(s, i, CLR, INT_MASK);
    }
    run_for_pause(s);
    return s;
}

static void tx_config(QTestState *s, unsigned c, unsigned bits)
{
    wr(s, c, USER, TX);
    wr(s, c, USER1, 0);
    wr(s, c, DLEN, bits - 1);
    for (unsigned i = 0; i < 16; i++) {
        wr(s, c, W0 + 4 * i, 0x67452301U ^ (0x11111111U * i));
    }
}

static void busy(QTestState *s, unsigned c)
{
    g_assert_cmphex(rd(s, c, CMD) & USR, ==, USR);
    g_assert_cmphex(rd(s, c, RAW) & DONE, ==, 0);
}

static void done(QTestState *s, unsigned c)
{
    g_assert_cmphex(rd(s, c, CMD) & USR, ==, 0);
    g_assert_cmphex(rd(s, c, RAW), ==, DONE);
}

static void irq(QTestState *s, unsigned c, uint32_t raw, uint32_t ena)
{
    g_assert_cmphex(rd(s, c, RAW), ==, raw);
    g_assert_cmphex(rd(s, c, ST), ==, raw & ena);
    g_assert_cmpint(qtest_get_irq(s, 0), ==, !!(raw & ena));
}

static void assert_paused(QTestState *s)
{
    QDict *r;
    QDict *status;

    qtest_qmp_eventwait(s, "STOP");
    r = qtest_qmp(s, "{'execute':'query-status'}");
    g_assert_false(qdict_haskey(r, "error"));
    status = qdict_get_qdict(r, "return");
    g_assert_cmpstr(qdict_get_str(status, "status"), ==, "paused");
    g_assert_false(qdict_get_bool(status, "running"));
    qobject_unref(r);
}

static void test_busy_last_edge(gconstpointer opaque)
{
    unsigned c = GPOINTER_TO_UINT(opaque);
    QTestState *s = setup(c);

    tx_config(s, c, 8);
    wr(s, c, ENA, DONE);
    wr(s, c, CMD, USR);
    busy(s, c);
    irq(s, c, 0, DONE);
    qtest_clock_step(s, 49); /* Before the first edge. */
    busy(s, c);
    qtest_clock_step(s, 750); /* One ns before the last trailing edge. */
    busy(s, c);
    irq(s, c, 0, DONE);
    qtest_clock_step(s, 1);
    done(s, c);
    irq(s, c, DONE, DONE);
    wr(s, c, CLR, DONE);
    irq(s, c, 0, DONE);
    qtest_clock_step(s, 10000);
    irq(s, c, 0, DONE); /* No second completion from a stale timer. */
    wr(s, c, USER, 0); /* No phases: still retire on a module-clock boundary. */
    wr(s, c, CMD, USR);
    busy(s, c);
    qtest_clock_step(s, 24);
    busy(s, c);
    qtest_clock_step(s, 1);
    done(s, c);
    qtest_quit(s);
}

/*
 * Qtest traverses timer deadlines even for a large requested distance.
 * This regression does not represent a genuinely overdue guest callback;
 * guest electrical horizon/late-edge equivalence remains unqualified.
 */
static void test_deadline_traversal_cancel(gconstpointer opaque)
{
    unsigned c = GPOINTER_TO_UINT(opaque);
    QTestState *s = setup(c);

    tx_config(s, c, 8);
    wr(s, c, ENA, DONE);
    wr(s, c, CMD, USR);
    qtest_clock_step(s, 49);
    busy(s, c);
    qtest_clock_step(s, 750); /* Traverses edges to one ns before completion. */
    busy(s, c);
    irq(s, c, 0, DONE);
    qtest_clock_step(s, 1);
    done(s, c);
    irq(s, c, DONE, DONE);
    wr(s, c, CLR, DONE);
    irq(s, c, 0, DONE);
    qtest_clock_step(s, 10000);
    irq(s, c, 0, DONE);

    for (unsigned kind = 0; kind < 2; kind++) {
        wr(s, c, CMD, USR);
        qtest_clock_step(s, 49);
        busy(s, c);
        qtest_clock_step(s, 750);
        busy(s, c);
        irq(s, c, 0, DONE);
        if (kind == 0) {
            wr(s, c, SLAVE, BIT(27));
            g_assert_cmphex(rd(s, c, SLAVE) & BIT(27), ==, 0);
        } else {
            wr(s, c, CLK_GATE, 0);
        }
        g_assert_cmphex(rd(s, c, CMD) & USR, ==, 0);
        irq(s, c, 0, DONE);
        qtest_clock_step(s, 100000); /* Cancelled, no active peer. */
        g_assert_cmphex(rd(s, c, CMD) & USR, ==, 0);
        irq(s, c, 0, DONE);
        wr(s, c, CLK_GATE, 3);
        qtest_clock_step(s, 1000);
        g_assert_cmphex(rd(s, c, CMD) & USR, ==, 0);
        irq(s, c, 0, DONE);
    }
    qtest_quit(s);
}

static void test_interrupt_registers(gconstpointer opaque)
{
    unsigned c = GPOINTER_TO_UINT(opaque);
    QTestState *s = setup(c);
    uint32_t other = BIT(17);

    wr(s, c, SET, DONE | other | BIT(31));
    irq(s, c, DONE | other, 0);
    g_assert_cmphex(rd(s, c, SET), ==, 0);
    wr(s, c, ENA, DONE | BIT(31));
    g_assert_cmphex(rd(s, c, ENA), ==, DONE);
    irq(s, c, DONE | other, DONE);
    wr(s, c, ST, UINT32_MAX); /* Read-only masked status. */
    irq(s, c, DONE | other, DONE);
    wr(s, c, CLR, other);
    irq(s, c, DONE, DONE);
    g_assert_cmphex(rd(s, c, CLR), ==, 0);
    wr(s, c, ENA, other);
    irq(s, c, DONE, other);
    wr(s, c, ENA, DONE);
    irq(s, c, DONE, DONE);
    wr(s, c, RAW, DONE); /* RAW has the same W1C semantics as CLR. */
    irq(s, c, 0, DONE);
    wr(s, c, ENA, UINT32_MAX);
    g_assert_cmphex(rd(s, c, ENA), ==, INT_MASK);
    qtest_quit(s);
}

static void test_update_snapshot(gconstpointer opaque)
{
    unsigned c = GPOINTER_TO_UINT(opaque);
    QTestState *s = setup(c);

    tx_config(s, c, 8);
    wr(s, c, CMD, UPDATE);
    g_assert_cmphex(rd(s, c, CMD) & (UPDATE | USR), ==, 0);
    qtest_clock_step(s, 1000);
    irq(s, c, 0, 0);
    wr(s, c, CMD, UPDATE | USR);
    busy(s, c);
    qtest_clock_step(s, 100);
    /* Program the following transaction while this one's snapshot is active. */
    wr(s, c, DLEN, 15);
    wr(s, c, CLOCK, (7U << 12) | (3U << 6)); /* Next period 200 ns. */
    wr(s, c, CMD, UPDATE);
    g_assert_cmphex(rd(s, c, CMD) & UPDATE, ==, 0);
    busy(s, c);
    wr(s, c, CMD, USR); /* BUSY does not restart/reload the active transfer. */
    qtest_clock_step(s, 699);
    busy(s, c);
    qtest_clock_step(s, 1);
    done(s, c);
    wr(s, c, CMD, UPDATE);
    done(s, c); /* UPDATE is not DONE-clear or a new transfer. */
    wr(s, c, CMD, USR);
    busy(s, c); /* START, unlike UPDATE, clears old hardware DONE. */
    qtest_clock_step(s, 3199);
    busy(s, c);
    qtest_clock_step(s, 1);
    done(s, c);
    qtest_quit(s);
}

static void test_setup_hold_keep_cs(gconstpointer opaque)
{
    unsigned c = GPOINTER_TO_UINT(opaque);
    QTestState *s = setup(c);

    tx_config(s, c, 8);
    wr(s, c, USER, TX | SETUP | HOLD);
    wr(s, c, USER1, (2U << 17) | (2U << 22)); /* Setup 3, hold 2 cycles. */
    wr(s, c, MISC, KEEP_CS);
    wr(s, c, CMD, USR);
    qtest_clock_step(s, 299);
    busy(s, c);
    qtest_clock_step(s, 1); /* Setup ends; shifter must still be busy. */
    busy(s, c);
    qtest_clock_step(s, 800); /* Final data edge, hold not yet elapsed. */
    busy(s, c);
    qtest_clock_step(s, 199);
    busy(s, c);
    qtest_clock_step(s, 1);
    done(s, c);
    /* KEEP_CS does not hold BUSY or suppress DONE. No electrical CS claim. */
    wr(s, c, CMD, USR);
    qtest_clock_step(s, 1299);
    busy(s, c);
    qtest_clock_step(s, 1);
    done(s, c);
    wr(s, c, MISC, 0);
    wr(s, c, USER, TX | BIT(31) | BIT(30) | BIT(29) | SETUP | HOLD);
    wr(s, c, USER1, (23U << 27) | (3U << 22) | (1U << 17) | 2);
    wr(s, c, USER2, (7U << 28) | 0x9f);
    wr(s, c, ADDR, 0x12345600);
    wr(s, c, CMD, USR);
    /* 2 setup + 8 command + 24 address + 3 dummy + 8 TX + 3 hold. */
    qtest_clock_step(s, 4799);
    busy(s, c);
    qtest_clock_step(s, 1);
    done(s, c);
    tx_config(s, c, 8);
    wr(s, c, USER, TX | HOLD);
    wr(s, c, USER1, 0); /* Raw hold zero must append no SCLK cycles. */
    wr(s, c, CMD, USR);
    qtest_clock_step(s, 799);
    busy(s, c);
    qtest_clock_step(s, 1);
    done(s, c);
    qtest_quit(s);
}

static void test_clock_sources_modes(gconstpointer opaque)
{
    unsigned c = GPOINTER_TO_UINT(opaque);
    QTestState *s = setup(c);

    tx_config(s, c, 8);
    for (unsigned pll = 0; pll < 2; pll++) {
        wr(s, c, CLK_GATE, 3 | (pll ? BIT(2) : 0));
        for (unsigned mode = 0; mode < 4; mode++) {
            wr(s, c, USER, TX | ((mode & 1) ? BIT(9) : 0));
            wr(s, c, MISC, (mode & 2) ? BIT(29) : 0);
            wr(s, c, CLOCK, CLOCK_10MHZ);
            wr(s, c, CMD, USR);
            qtest_clock_step(s, (pll ? 400 : 800) - 1);
            busy(s, c);
            qtest_clock_step(s, 1);
            done(s, c);
            wr(s, c, CLOCK, BIT(31)); /* Equal source: no divider. */
            wr(s, c, CMD, USR);
            qtest_clock_step(s, (pll ? 100 : 200) - 1);
            busy(s, c);
            qtest_clock_step(s, 1);
            done(s, c);
        }
    }
    qtest_quit(s);
}

static void test_pll_independent_cpu_mux(gconstpointer opaque)
{
    unsigned c = GPOINTER_TO_UINT(opaque);
    QTestState *s = setup(c);
    uint32_t options = qtest_readl(s, RTC_OPTIONS0) & ~BBPLL_FORCE_PD;
    qtest_writel(s, RTC_OPTIONS0, options);
    tx_config(s, c, 8);
    wr(s, c, CLK_GATE, 7); /* Actual PLL_F80M, not CPU/APB. */
    uint32_t cfg = qtest_readl(s, SYSTEM_SYSCLK) & ~0xc00U;
    for (unsigned i = 0; i < 2; i++) {
        qtest_writel(s, SYSTEM_SYSCLK, cfg | (i ? 2U << 10 : 0));
        wr(s, c, CMD, USR);
        qtest_clock_step(s, 100);
        busy(s, c);
        /* Changing only the CPU mux mid-transfer cannot cancel GP-SPI or
         * change its 20 MHz (/4) clock from the independently powered PLL. */
        qtest_writel(s, SYSTEM_SYSCLK, cfg | (i ? 0 : 2U << 10));
        qtest_clock_step(s, 299);
        busy(s, c);
        qtest_clock_step(s, 1);
        done(s, c);
    }
    wr(s, c, CMD, USR);
    qtest_clock_step(s, 50);
    busy(s, c);
    qtest_writel(s, RTC_OPTIONS0, options | BBPLL_FORCE_PD);
    qtest_clock_step(s, 500);
    g_assert_cmphex(rd(s, c, CMD) & USR, ==, 0);
    g_assert_cmphex(rd(s, c, RAW) & DONE, ==, 0);
    qtest_writel(s, RTC_OPTIONS0, options);
    wr(s, c, CMD, USR);
    qtest_clock_step(s, 400);
    done(s, c);
    qtest_quit(s);
}

static void test_abort(gconstpointer opaque)
{
    unsigned c = GPOINTER_TO_UINT(opaque);
    QTestState *s = setup(c);
    uint32_t enables = qtest_readl(s, SYSTEM_EN0);

    for (unsigned kind = 0; kind < 4; kind++) {
        tx_config(s, c, 512);
        wr(s, c, ENA, DONE);
        wr(s, c, CMD, USR);
        qtest_clock_step(s, 175);
        busy(s, c);
        switch (kind) {
        case 0:
            wr(s, c, SLAVE, BIT(27));
            g_assert_cmphex(rd(s, c, SLAVE) & BIT(27), ==, 0);
            break;
        case 1:
            wr(s, c, CLK_GATE, 0);
            break;
        case 2:
            qtest_writel(s, SYSTEM_EN0, enables & ~SYSTEM_SPI(c));
            break;
        case 3:
            qtest_writel(s, SYSTEM_RST0, SYSTEM_SPI(c));
            g_assert_cmphex(rd(s, c, ENA), ==, 0);
            break;
        }
        g_assert_cmphex(rd(s, c, CMD) & USR, ==, 0);
        g_assert_cmphex(rd(s, c, RAW), ==, 0);
        g_assert_false(qtest_get_irq(s, 0));
        qtest_clock_step(s, 60000); /* Well beyond the aborted final edge. */
        g_assert_cmphex(rd(s, c, RAW), ==, 0);
        qtest_writel(s, SYSTEM_RST0, 0);
        qtest_writel(s, SYSTEM_EN0, enables);
        wr(s, c, CLK_GATE, 3);
        wr(s, c, CLOCK, CLOCK_10MHZ);
        qtest_clock_step(s, 1000);
        g_assert_cmphex(rd(s, c, CMD) & USR, ==, 0);
        g_assert_cmphex(rd(s, c, RAW), ==, 0); /* Ungate never resumes abort. */
        tx_config(s, c, 8);
        wr(s, c, CMD, USR);
        qtest_clock_step(s, 799);
        busy(s, c);
        qtest_clock_step(s, 1);
        done(s, c);
        wr(s, c, CLR, DONE);
    }
    qtest_quit(s);
}

static void test_independence(void)
{
    QTestState *s = setup(0);
    uint32_t enables = qtest_readl(s, SYSTEM_EN0);

    tx_config(s, 0, 8);
    tx_config(s, 1, 16);
    wr(s, 0, ENA, DONE);
    wr(s, 0, CMD, USR);
    wr(s, 1, CMD, USR);
    qtest_clock_step(s, 799);
    busy(s, 0);
    busy(s, 1);
    qtest_clock_step(s, 1);
    done(s, 0);
    busy(s, 1);
    wr(s, 0, CLR, DONE);
    wr(s, 0, SLAVE, BIT(27));
    qtest_writel(s, SYSTEM_EN0, enables & ~SYSTEM_SPI(0));
    qtest_clock_step(s, 799);
    busy(s, 1);
    qtest_clock_step(s, 1);
    done(s, 1);
    irq(s, 0, 0, DONE);
    qtest_writel(s, SYSTEM_EN0, enables);
    /* This time abort only SPI3 while SPI2 is shifting. */
    wr(s, 0, CMD, USR);
    wr(s, 1, CMD, USR);
    qtest_clock_step(s, 50);
    wr(s, 1, SLAVE, BIT(27));
    g_assert_cmphex(rd(s, 1, CMD) & USR, ==, 0);
    qtest_clock_step(s, 750);
    done(s, 0);
    g_assert_cmphex(rd(s, 1, RAW), ==, 0);
    qtest_quit(s);
}

static void test_register_bounds(gconstpointer opaque)
{
    unsigned c = GPOINTER_TO_UINT(opaque);
    QTestState *s = setup(c);

    tx_config(s, c, 512); /* Entire real 64-byte PIO buffer. */
    qtest_writeb(s, SPI_BASE(c) + W0, 0xff);
    qtest_writew(s, SPI_BASE(c) + W0, 0xffff);
    qtest_writel(s, SPI_BASE(c) + W0 + 1, UINT32_MAX);
    g_assert_cmphex(rd(s, c, W0), ==, 0x67452301);
    /* QEMU's rejected MMIO access path returns zero, not register data. */
    g_assert_cmphex(qtest_readb(s, SPI_BASE(c) + W0), ==, 0);
    g_assert_cmphex(qtest_readw(s, SPI_BASE(c) + W0), ==, 0);
    g_assert_cmphex(qtest_readl(s, SPI_BASE(c) + W0 + 1), ==, 0);
    for (unsigned reg = 0x100; reg < 0x1000; reg += 4) {
        wr(s, c, reg, UINT32_MAX);
        g_assert_cmphex(rd(s, c, reg), ==, 0);
    }
    g_assert_cmphex(rd(s, c, W0 + 60), ==, 0x67452301U ^ 0xffffffffU);
    g_assert_cmphex(rd(s, c ^ 1, RAW), ==, 0);
    wr(s, c, CMD, USR);
    qtest_clock_step(s, 51199);
    busy(s, c);
    qtest_clock_step(s, 1);
    done(s, c);
    /* High-half selection leaves only 32 bytes, which remains legal. */
    wr(s, c, USER, TX | BIT(25));
    wr(s, c, DLEN, 255);
    wr(s, c, CMD, USR);
    qtest_clock_step(s, 25599);
    busy(s, c);
    qtest_clock_step(s, 1);
    done(s, c);
    qtest_quit(s);
}

static void test_pio_overflow(gconstpointer opaque)
{
    unsigned c = GPOINTER_TO_UINT(opaque);

    for (unsigned variant = 0; variant < 3; variant++) {
        QTestState *s = setup(c);
        uint32_t words[16];

        tx_config(s, c, variant == 1 ? 264 : 520);
        wr(s, c, USER, variant == 2 ? RX : TX | (variant == 1 ? BIT(25) : 0));
        for (unsigned i = 0; i < 16; i++) {
            words[i] = rd(s, c, W0 + i * 4);
        }
        run_for_pause(s);
        wr(s, c, CMD, USR);
        assert_paused(s);
        qtest_clock_step(s, 100000);
        busy(s, c);
        g_assert_cmphex(rd(s, c, RAW), ==, 0);
        for (unsigned i = 0; i < 16; i++) {
            g_assert_cmphex(rd(s, c, W0 + i * 4), ==, words[i]);
        }
        g_assert_cmphex(rd(s, c, SLAVE), ==, 0);
        g_assert_cmphex(rd(s, c, CLK_GATE), ==, 3);
        g_assert_cmphex(rd(s, c ^ 1, RAW), ==, 0);
        wr(s, c, SLAVE, BIT(27));
        g_assert_cmphex(rd(s, c, CMD) & USR, ==, 0);
        qtest_quit(s);
    }
}

static void test_unknown_rx(gconstpointer opaque)
{
    unsigned c = GPOINTER_TO_UINT(opaque);
    QTestState *s = setup(c);
    uint32_t words[16];

    tx_config(s, c, 8);
    wr(s, c, USER, RX);
    for (unsigned i = 0; i < 16; i++) {
        words[i] = rd(s, c, W0 + 4 * i);
    }
    run_for_pause(s);
    wr(s, c, CMD, USR);
    qtest_clock_step(s, 49);
    busy(s, c);
    qtest_clock_step(s, 1); /* First sampling edge: no known MISO. */
    assert_paused(s);
    qtest_clock_step(s, 10000);
    busy(s, c);
    irq(s, c, 0, 0);
    for (unsigned i = 0; i < 16; i++) {
        g_assert_cmphex(rd(s, c, W0 + 4 * i), ==, words[i]);
    }
    wr(s, c, SLAVE, BIT(27));
    g_assert_cmphex(rd(s, c, CMD) & USR, ==, 0);
    qtest_quit(s);
}

static void descriptor(QTestState *s, uint32_t addr, unsigned length,
                       bool eof, uint32_t buf, uint32_t next)
{
    /* MMIO helpers write little-endian guest words, independent of host endian. */
    qtest_writel(s, addr, length | (length << 12) | DESC_OWNER |
                 (eof ? DESC_EOF : 0));
    qtest_writel(s, addr + 4, buf);
    qtest_writel(s, addr + 8, next);
}

static void arm_out(QTestState *s, unsigned channel, unsigned peripheral,
                    uint32_t first)
{
    qtest_writel(s, GDMA_OUT(channel, G_CONF0), G_AUTO_WRBACK);
    qtest_writel(s, GDMA_OUT(channel, G_CONF1), G_CHECK_OWNER);
    qtest_writel(s, GDMA_OUT(channel, G_CLR), UINT32_MAX);
    qtest_writel(s, GDMA_OUT(channel, G_PERI), peripheral);
    qtest_writel(s, GDMA_OUT(channel, G_LINK), (first & G_ADDR_MASK) | G_START);
}

static void assert_other_channel(QTestState *s)
{
    g_assert_cmphex(qtest_readl(s, OTHER_DESC) & DESC_OWNER, ==, DESC_OWNER);
    g_assert_cmphex(qtest_readl(s, GDMA_OUT(1, G_RAW)), ==, 0);
    g_assert_cmphex(qtest_readl(s, GDMA_OUT(1, G_PERI)), ==, 1);
    g_assert_cmphex(qtest_readl(s, GDMA_OUT(1, G_LINK)) & G_PARK, ==, 0);
}

static void test_gdma_opposite_reset(gconstpointer opaque)
{
    unsigned c = GPOINTER_TO_UINT(opaque);
    QTestState *s = setup(c);

    qtest_writel(s, SYSTEM_GDMA_EN1,
                 qtest_readl(s, SYSTEM_GDMA_EN1) | SYSTEM_GDMA);
    qtest_memset(s, SOURCE_AREA, 0x96, 8);
    descriptor(s, DESC_AREA, 8, true, SOURCE_AREA, 0);
    arm_out(s, 0, c, DESC_AREA);
    tx_config(s, c, 64);
    wr(s, c, DMA, BIT(28));
    wr(s, c, CMD, USR);
    qtest_clock_step(s, 100);
    busy(s, c);

    /* Each peripheral-handshake direction owns its FSM. Resetting IN must
     * not discard this controller's already-active OUT descriptor cursor. */
    qtest_writel(s, GDMA_IN(0, G_CONF0), BIT(0));
    qtest_writel(s, GDMA_IN(0, G_CONF0), 0);
    g_assert_cmphex(qtest_readl(s, GDMA_OUT(0, G_LINK)) & G_PARK, ==, 0);
    qtest_clock_step(s, 6299);
    busy(s, c);
    qtest_clock_step(s, 1);
    done(s, c);
    g_assert_cmphex(qtest_readl(s, DESC_AREA), ==,
                    8U | (8U << 12) | DESC_EOF);
    g_assert_cmphex(qtest_readl(s, GDMA_OUT(0, G_RAW)), ==,
                    G_DONE | G_EOF | G_TOTAL_EOF);
    g_assert_cmphex(qtest_readl(s, GDMA_OUT(0, G_EOF_DESC)), ==, DESC_AREA);
    qtest_quit(s);
}

static void test_gdma_large_tx(gconstpointer opaque)
{
    bool cpha = GPOINTER_TO_UINT(opaque);
    int64_t first_fetch = cpha ? 50 : 0;
    QTestState *s = setup(0);
    uint8_t source[TX_BYTES];
    int64_t boundary = (CHUNK - 1) * 800LL + first_fetch;
    int64_t second_boundary = (2 * CHUNK - 1) * 800LL + first_fetch;
    int64_t eof_fetch = (TX_BYTES - 1) * 800LL + first_fetch;

    qtest_writel(s, SYSTEM_GDMA_EN1,
                 qtest_readl(s, SYSTEM_GDMA_EN1) | SYSTEM_GDMA);
    for (unsigned i = 0; i < TX_BYTES; i++) {
        source[i] = (i * 37U + (i >> 8)) & 255;
    }
    qtest_memwrite(s, SOURCE_AREA, source, sizeof(source));
    qtest_memset(s, OTHER_SOURCE, 0xa5, 32);
    descriptor(s, DESC_AREA, CHUNK, false, SOURCE_AREA, DESC_AREA + 12);
    descriptor(s, DESC_AREA + 12, CHUNK, false, SOURCE_AREA + CHUNK, DESC_AREA + 24);
    descriptor(s, DESC_AREA + 24, 9, true, SOURCE_AREA + 2 * CHUNK, 0);
    descriptor(s, OTHER_DESC, 32, true, OTHER_SOURCE, 0);
    arm_out(s, 0, 0, DESC_AREA); /* Actual GDMA_SPI2 peripheral selection. */
    arm_out(s, 1, 1, OTHER_DESC); /* Armed SPI3 outlink must not be consumed. */
    tx_config(s, 0, TX_BYTES * 8);
    wr(s, 0, USER, TX | (cpha ? BIT(9) : 0));
    wr(s, 0, DMA, BIT(28));
    wr(s, 0, CMD, USR);
    busy(s, 0);
    g_assert_cmphex(qtest_readl(s, GDMA_OUT(0, G_BF0)), ==, DESC_AREA);
    qtest_clock_step(s, boundary - 1);
    g_assert_cmphex(qtest_readl(s, DESC_AREA) & DESC_OWNER, ==, DESC_OWNER);
    qtest_clock_step(s, 1); /* Byte cursor reaches last byte, not first pump. */
    g_assert_cmphex(qtest_readl(s, DESC_AREA) & DESC_OWNER, ==, 0);
    g_assert_cmphex(qtest_readl(s, DESC_AREA + 12) & DESC_OWNER, ==, DESC_OWNER);
    g_assert_cmphex(qtest_readl(s, GDMA_OUT(0, G_BF0)), ==, DESC_AREA + 12);
    g_assert_cmphex(qtest_readl(s, GDMA_OUT(0, G_BF1)), ==, DESC_AREA);
    g_assert_cmphex(qtest_readl(s, GDMA_OUT(0, G_DESC)), ==, DESC_AREA + 24);
    busy(s, 0);
    assert_other_channel(s);
    qtest_clock_step(s, second_boundary - boundary - 1);
    g_assert_cmphex(qtest_readl(s, DESC_AREA + 12) & DESC_OWNER, ==, DESC_OWNER);
    qtest_clock_step(s, 1);
    g_assert_cmphex(qtest_readl(s, DESC_AREA + 12) & DESC_OWNER, ==, 0);
    g_assert_cmphex(qtest_readl(s, GDMA_OUT(0, G_BF0)), ==, DESC_AREA + 24);
    g_assert_cmphex(qtest_readl(s, GDMA_OUT(0, G_RAW)) & (G_EOF | G_TOTAL_EOF | G_DESC_ERR), ==, 0);
    qtest_clock_step(s, eof_fetch - second_boundary - 1);
    g_assert_cmphex(qtest_readl(s, DESC_AREA + 24) & DESC_OWNER, ==, DESC_OWNER);
    qtest_clock_step(s, 1);
    g_assert_cmphex(qtest_readl(s, DESC_AREA + 24), ==, 9U | (9U << 12) | DESC_EOF);
    g_assert_cmphex(qtest_readl(s, GDMA_OUT(0, G_RAW)), ==, G_DONE | G_EOF | G_TOTAL_EOF);
    g_assert_cmphex(qtest_readl(s, GDMA_OUT(0, G_EOF_DESC)), ==, DESC_AREA + 24);
    g_assert_cmphex(qtest_readl(s, GDMA_OUT(0, G_LINK)) & G_PARK, ==, G_PARK);
    busy(s, 0); /* DMA EOF precedes final byte's trailing edge. */
    qtest_clock_step(s, 799 - first_fetch);
    busy(s, 0);
    qtest_clock_step(s, 1);
    done(s, 0);
    assert_other_channel(s);
    g_assert_cmphex(rd(s, 1, CMD) & USR, ==, 0);
    g_assert_cmphex(rd(s, 1, RAW), ==, 0);
    for (unsigned i = 0; i < 3; i++) {
        unsigned length = i < 2 ? CHUNK : 9;
        g_assert_cmphex(qtest_readl(s, DESC_AREA + 12 * i), ==,
                        length | (length << 12) | (i == 2 ? DESC_EOF : 0));
        g_assert_cmphex(qtest_readl(s, DESC_AREA + 12 * i + 4), ==,
                        SOURCE_AREA + CHUNK * i);
        g_assert_cmphex(qtest_readl(s, DESC_AREA + 12 * i + 8), ==,
                        i == 2 ? 0 : DESC_AREA + 12 * (i + 1));
    }
    qtest_quit(s);
}

int main(int argc, char **argv)
{
    static const struct {
        const char *name;
        void (*fn)(gconstpointer);
    } cases[] = {
        { "busy-last-edge-no-receiver", test_busy_last_edge },
        { "deadline-traversal-reset-gate-cancel", test_deadline_traversal_cancel },
        { "interrupt-mask-w1c-software-set", test_interrupt_registers },
        { "update-and-active-snapshot", test_update_snapshot },
        { "setup-hold-keep-cs-phase-duration", test_setup_hold_keep_cs },
        { "xtal-pll80-all-clock-modes", test_clock_sources_modes },
        { "pll-powered-independent-cpu-mux-and-power-gate", test_pll_independent_cpu_mux },
        { "softreset-local-system-gate-abort", test_abort },
        { "register-width-bounds-full-pio", test_register_bounds },
        { "pio-overflow-strict-pause", test_pio_overflow },
        { "unknown-rx-strict-pause", test_unknown_rx },
        { "gdma-opposite-direction-reset-preserves-active-tx", test_gdma_opposite_reset },
    };

    g_test_init(&argc, &argv, NULL);
    for (unsigned c = 0; c < 2; c++) {
        for (unsigned i = 0; i < G_N_ELEMENTS(cases); i++) {
            g_autofree char *path = g_strdup_printf("/esp32s3/spi%u/%s",
                                                   c + 2, cases[i].name);
            qtest_add_data_func(path, GUINT_TO_POINTER(c), cases[i].fn);
        }
    }
    qtest_add_func("/esp32s3/spi/independent-controllers", test_independence);
    qtest_add_data_func("/esp32s3/spi/gdma-multidescriptor-4105-byte-tx-cpha0",
                        GUINT_TO_POINTER(0), test_gdma_large_tx);
    qtest_add_data_func("/esp32s3/spi/gdma-multidescriptor-4105-byte-tx-cpha1",
                        GUINT_TO_POINTER(1), test_gdma_large_tx);
    return g_test_run();
}
